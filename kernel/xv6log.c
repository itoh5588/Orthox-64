/*
 * xv6log.c — ジャーナリング層
 * xv6-riscv/kernel/log.c を Orthox-64 向けに移植。
 *
 * 変更点:
 *   struct proc / myproc() / sleep() / wakeup() を除去
 *   sleep 待機 → spinlock + スピン待機で代替
 *   initlock / acquire / release → spinlock_init / spin_lock / spin_unlock
 *   bread/bwrite/brelse/bpin/bunpin → xv6 プレフィックス版
 *   BSIZE / LOGBLOCKS → XV6FS_BSIZE / XV6FS_LOGBLOCKS
 */

#include "xv6fs.h"
#include <stdarg.h>

extern int vsnprintf(char *dst, size_t size, const char *fmt, va_list ap);
extern int64_t sys_write_serial(const char *buf, size_t count);
extern void *memcpy(void *dst, const void *src, size_t n);

static void xv6log_print(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) sys_write_serial(buf, (size_t)n);
}

#define XV6LOG_PANIC(msg) do { \
    xv6log_print("xv6log PANIC: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); \
    __builtin_trap(); \
} while(0)

/* ------------------------------------------------------------------ */
/* on-disk ログヘッダ                                                  */
/* ------------------------------------------------------------------ */

struct logheader {
    int n;
    int block[XV6FS_LOGBLOCKS];
};

/* ------------------------------------------------------------------ */
/* in-memory ログ状態                                                  */
/* ------------------------------------------------------------------ */

struct xv6log {
    spinlock_t lock;
    int        start;        /* logstart ブロック番号 */
    int        outstanding;  /* 進行中の FS システムコール数 */
    int        committing;   /* commit() 実行中フラグ */
    uint32_t   dev;
    struct logheader lh;
};

static struct xv6log lg;

static void commit(void);
static void recover_from_log(void);

/* ------------------------------------------------------------------ */
/* 初期化                                                              */
/* ------------------------------------------------------------------ */

void xv6log_init(uint32_t dev, struct xv6fs_superblock *sb) {
    if (sizeof(struct logheader) >= XV6FS_BSIZE)
        XV6LOG_PANIC("initlog: logheader too large");

    spinlock_init(&lg.lock);
    lg.start       = (int)sb->logstart;
    lg.dev         = dev;
    lg.outstanding = 0;
    lg.committing  = 0;
    lg.lh.n        = 0;

    recover_from_log();
}

/* ------------------------------------------------------------------ */
/* ログブロックをホーム位置にコピー                                    */
/* ------------------------------------------------------------------ */

static void install_trans(int recovering) {
    for (int tail = 0; tail < lg.lh.n; tail++) {
        if (recovering)
            xv6log_print("xv6log: recover tail=%d dst=%d\n",
                         tail, lg.lh.block[tail]);
        struct xv6buf *lbuf = xv6bread(lg.dev, (uint32_t)(lg.start + tail + 1));
        struct xv6buf *dbuf = xv6bread(lg.dev, (uint32_t)lg.lh.block[tail]);
        memcpy(dbuf->data, lbuf->data, XV6FS_BSIZE);
        xv6bwrite(dbuf);
        if (!recovering)
            xv6bunpin(dbuf);
        xv6brelse(lbuf);
        xv6brelse(dbuf);
    }
}

/* ------------------------------------------------------------------ */
/* ログヘッダ読み書き                                                  */
/* ------------------------------------------------------------------ */

static void read_head(void) {
    struct xv6buf *buf = xv6bread(lg.dev, (uint32_t)lg.start);
    struct logheader *lh = (struct logheader *)buf->data;
    lg.lh.n = lh->n;
    for (int i = 0; i < lg.lh.n; i++)
        lg.lh.block[i] = lh->block[i];
    xv6brelse(buf);
}

static void write_head(void) {
    struct xv6buf *buf = xv6bread(lg.dev, (uint32_t)lg.start);
    struct logheader *hb = (struct logheader *)buf->data;
    hb->n = lg.lh.n;
    for (int i = 0; i < lg.lh.n; i++)
        hb->block[i] = lg.lh.block[i];
    xv6bwrite(buf);
    xv6brelse(buf);
}

/* ------------------------------------------------------------------ */
/* クラッシュリカバリ                                                  */
/* ------------------------------------------------------------------ */

void xv6log_recover(void) {
    recover_from_log();
}

static void recover_from_log(void) {
    read_head();
    install_trans(1);
    lg.lh.n = 0;
    write_head();
}

/* ------------------------------------------------------------------ */
/* トランザクション開始 / 終了                                         */
/* ------------------------------------------------------------------ */

/*
 * xv6 オリジナルは sleep() で待機するが、Orthox-64 では
 * スリープ機構が FS と切り離されているためスピン待機で代替する。
 * SMP 有効化後に task wakeup へ改善予定。
 */
void xv6log_begin_op(void) {
    for (;;) {
        spin_lock(&lg.lock);
        if (!lg.committing &&
            lg.lh.n + (lg.outstanding + 1) * 10 <= XV6FS_LOGBLOCKS) {
            lg.outstanding++;
            spin_unlock(&lg.lock);
            return;
        }
        spin_unlock(&lg.lock);
        /* ログが満杯 or commit 中: スピン待機 */
        __asm__ volatile ("pause" ::: "memory");
    }
}

void xv6log_end_op(void) {
    int do_commit = 0;

    spin_lock(&lg.lock);
    lg.outstanding--;
    if (lg.committing)
        XV6LOG_PANIC("xv6log_end_op: log.committing");
    if (lg.outstanding == 0) {
        do_commit = 1;
        lg.committing = 1;
    }
    spin_unlock(&lg.lock);

    if (do_commit) {
        commit();
        spin_lock(&lg.lock);
        lg.committing = 0;
        spin_unlock(&lg.lock);
    }
}

/* ------------------------------------------------------------------ */
/* キャッシュ → ログへの書き込み                                       */
/* ------------------------------------------------------------------ */

static void write_log(void) {
    for (int tail = 0; tail < lg.lh.n; tail++) {
        struct xv6buf *to   = xv6bread(lg.dev, (uint32_t)(lg.start + tail + 1));
        struct xv6buf *from = xv6bread(lg.dev, (uint32_t)lg.lh.block[tail]);
        memcpy(to->data, from->data, XV6FS_BSIZE);
        xv6bwrite(to);
        xv6brelse(from);
        xv6brelse(to);
    }
}

static void commit(void) {
    if (lg.lh.n > 0) {
        write_log();
        write_head();
        install_trans(0);
        lg.lh.n = 0;
        write_head();
    }
}

/* ------------------------------------------------------------------ */
/* ブロックをログに登録する                                            */
/* ------------------------------------------------------------------ */

void xv6log_write(struct xv6buf *b) {
    int i;

    spin_lock(&lg.lock);
    if (lg.lh.n >= XV6FS_LOGBLOCKS)
        XV6LOG_PANIC("xv6log_write: transaction too large");
    if (lg.outstanding < 1)
        XV6LOG_PANIC("xv6log_write: outside of transaction");

    for (i = 0; i < lg.lh.n; i++) {
        if (lg.lh.block[i] == (int)b->blockno)
            break;   /* log absorption: 同一ブロックは1エントリ */
    }
    lg.lh.block[i] = (int)b->blockno;
    if (i == lg.lh.n) {
        xv6bpin(b);
        lg.lh.n++;
    }
    spin_unlock(&lg.lock);
}
