# Issue: kernel assert / panic 基盤の導入検討

## Summary
- 2026-04-29 時点で、kernel 本体には共通の `assert()` / `KASSERT()` / `panic()` 基盤はない。
- `kernel/` と `include/` の kernel 対象コードで確認できる assert 相当は lwIP 向けの `LWIP_PLATFORM_ASSERT` のみ。
- 現在の kernel 整理 / `syscall.c` 分割を優先し、assert / panic 基盤の導入は別 issue として後で検討する。

## Current state
- `include/kassert.h` と `kernel/kassert.c` に kernel 共通の `KASSERT()` / `KBUG_ON()` / `kernel_panic()` がある。
- `include/arch/cc.h` の `LWIP_PLATFORM_ASSERT(x)` と `kernel/lwip_port.c` の lwIP 専用 assert 出力は、現時点では既存の lwIP port 専用経路として残す。
- 既存 subsystem の不変条件チェックはまだ `if (...) return -1;`、`puts("[warn] ...")`、または個別の停止処理に分散している。Rank 7 以降で、Rank 5 policy に沿って内部 invariant に限定して `KASSERT()` へ移行する。

## Decision
- `syscall.c` 分割や kernel 整理の途中では導入しない。
- 整理が完了してから、panic してよい内部不変条件と、user input 由来で `-errno` を返すべき条件を分けて設計する。
- 2026-05-06 Rank 5 で `Docs/Internal/Kernel.md` に ASSERT / panic policy を追加した。
- ASSERT は kernel 内部 invariant の破損検出に限定する。syscall 引数、user pointer、path、fd、missing file、容量不足、unsupported syscall など userland 由来の通常エラーは panic せず、Linux 互換 ABI の負の `-errno` で返す。
- ASSERT / panic 基盤を実装する Rank 6 以降では、invalid input smoke が panic しないことを先に確認してから内部 invariant へ `KASSERT()` を入れる。

## Candidate design
- `include/kassert.h` を追加する。
- `kernel_panic(const char* file, int line, const char* func, const char* expr)` 相当を追加する。
- `KASSERT(expr)` は file / line / function / expression を serial に出して `cli; hlt` loop に入る。
- `KERNEL_DEBUG_ASSERTS` のような build flag で有効 / 無効を切り替える。
- user pointer、syscall 引数、FS path など userland 由来の失敗には使わず、内部構造の破損検出に限定する。

## Implementation
- 2026-05-06 Rank 6 で `include/kassert.h` と `kernel/kassert.c` を追加した。
- `KASSERT(expr)` と `KBUG_ON(cond)` は `kernel_panic(__FILE__, __LINE__, __func__, ...)` に接続する。
- `kernel_panic()` は serial に panic header、expression、function、file、line を出し、`cli; hlt` loop で停止する。
- `ORTHOX_KASSERT_SELFTEST` を付けた build だけ、boot 直後に `KASSERT(0 && "ORTHOX_KASSERT_SELFTEST")` を発火できる。通常 build ではこの selftest は無効。
- Rank 6 の確認では QEMU serial に `*** KERNEL PANIC ***`、`expr: 0 && "ORTHOX_KASSERT_SELFTEST"`、`func: _start`、`file: kernel/init.c`、`HALTING...` が出ることを確認した。
- selftest 後に通常 kernel / ISO を再生成し、通常 boot が `/bin/ash` まで到達することを確認した。
- 2026-05-06 Rank 11 で VFS / fd 境界に限定して、open fd の shared file refcount、descriptor type と file type の一致、pipe / socket / xv6fs / dir private data の non-null を `KASSERT()` で確認するようにした。bad fd や missing path など userland 由来の通常エラーは引き続き errno path に残す。
- 2026-05-06 Rank 12 で VM / task 境界に限定して、page table alignment、user PML4 と kernel PML4 の取り違え、COW refcount、run queue state、reap 時の CR3 / kernel stack alignment を `KASSERT()` で確認するようにした。`task_fork()` の allocation failure は ASSERT ではなく rollback して `-1` を返す。
- 2026-05-07 Rank 13 で VirtIO / storage 境界に限定して、virtqueue layout、descriptor id 範囲、block request context ownership、DMA address alignment、RX/TX queue readiness を `KASSERT()` で確認するようにした。device 未検出、timeout、I/O status failure、TX busy など device / environment 由来の失敗は通常エラー path に残す。
- 2026-05-07 Rank 14 で `ORTHOX_ENABLE_ASSERTS` を追加した。default は `1` で `KASSERT()` 有効。`KERNEL_CFLAGS_EXTRA=-DORTHOX_ENABLE_ASSERTS=0` の build では `KASSERT()` は式を評価しない no-op になる。`KBUG_ON()` は常時有効。

## Revisit trigger
- `syscall.c` の主要分割が一段落した後。
- `task.c` / VM / FS の責務境界を整理し、不変条件を文書化できる段階。
- SMP / lock / run queue / page table の invariant を明確にしたい段階。

## Notes
- assert 導入は障害を早く発見する一方、panic 条件を誤ると userland の通常エラーで kernel が止まる。
- 先に `Docs/Internal/Kernel.md` 側で「panic すべき invariant」と「errno で返すべき recoverable error」の境界を決める。
- Rank 5 の基準 smoke は `tests/vm_syscall_smoke.sh` / `user/vmerrno_test.c`。missing path、bad fd `mmap`、unmapped `mprotect`、unsupported syscall を errno として確認する。
