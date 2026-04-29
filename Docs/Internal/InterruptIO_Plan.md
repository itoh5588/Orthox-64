# Interrupt-driven I/O 実装計画書（改訂版）

## 1. 概要

Orthox-64 の現状の I/O 実装（VirtIO Block/Net）はポーリング方式であり、CPU効率と並行性に制限がある。本計画では割り込み駆動型へ移行し、I/O待機中のCPU利用効率を改善する。

本改訂版では特に以下を重視する：
- レースコンディション回避
- SMP安全性
- 段階的非同期化

---

## 2. 現在の問題点

- CPU資源の浪費（busy wait）
- マルチタスク阻害
- ネットワーク応答遅延

---

## 3. 設計上の重要課題（追加）

### 3.1 Lost Wakeup 問題
単純な sleep/wake では以下が発生する：

1. wake 発生
2. まだ sleep していない
3. sleep → 永久停止

→ condition付き待機が必須

---

### 3.2 割り込みコンテキスト制約
- IRQ内で重い処理は禁止
- scheduler直接呼び出し禁止

→ bottom half（deferred処理）導入が必要

---

### 3.3 SMP競合
- 別CPUでwake発生
- run queue競合

→ CPU間wake設計が必要

---

## 4. コア設計（改訂）

### 4.1 wait_queue（安全版）

```c
struct wait_queue {
    spinlock_t lock;
    struct task *head;
};
```

### 4.2 wait_event パターン

```c
#define wait_event(q, cond) \
    while (!(cond)) { \
        task_sleep_on(q); \
    }
```

※ 実装では lock と atomic 保証が必要

---

### 4.3 completion 導入（推奨）

```c
struct completion {
    int done;
    struct wait_queue q;
};

void wait_for_completion(struct completion *c);
void complete(struct completion *c);
```

用途：
- 単発I/O完了通知

---

### 4.4 タスク状態

最低限導入：

- TASK_RUNNING
- TASK_SLEEPING
- TASK_IO_WAIT

（将来的に interruptible 追加）

---

## 5. 実装ロードマップ（改訂）

### フェーズ1: 安全な待機機構

- wait_queue + lock 実装
- wait_event マクロ導入
- completion 実装

現状: 実施済み。`include/wait.h` / `kernel/wait.c` に `wait_queue`、`wait_event()`、`completion` を追加した。待機 task は `TASK_IO_WAIT` へ遷移し、`wake_up_one()` / `wake_up_all()` から既存の `task_wake()` 経由で run queue に戻る。まだ VirtIO Block/Net はこの基盤を使用していない。

---

### フェーズ2: 割り込み基盤

- MSI-X 初期化（または legacy fallback）
- IDT 登録
- IRQ handler は最小処理のみ

現状: 部分実施済み。legacy PIC IRQ 34-47 の IDT stub と `pic_unmask_irq()` / `pic_mask_irq()` を追加し、VirtIO Block の legacy PCI IRQ line を unmask する。MSI/MSI-X は未実装。

```c
irq_handler() {
    enqueue_bottom_half();
}
```

---

### フェーズ3: bottom half（必須追加）

- ソフトIRQまたはワークキュー導入
- 実際の wake / queue処理はここで実施

現状: 未実装。

---

### フェーズ4: VirtIO Block（段階化）

#### Step 1（安全）
- single request async
- completion で待機

現状: 部分実施済み。`kernel/virtio_blk.c` は legacy IRQ line が有効な場合、single request を `completion` で待機する。IRQ handler は VirtIO ISR を read ack し、used ring を回収して completion を完了する。IRQ line が無効な環境では従来の `used->idx` polling に fallback する。multi-request / inflight 管理は未実装。

#### Step 2（拡張）
- 複数リクエスト
- inflight 管理

※ いきなり multi-request は禁止

---

### フェーズ5: VirtIO Net

- RX interrupt → packet enqueue
- poll廃止

現状: 未実装。`kernel/virtio_net.c` は `virtio_net_poll()` による回収を維持している。

---

### フェーズ6: SMP対応

- wake先CPU決定
- run queueへの安全な投入

---

### フェーズ7: VFS整合性

- 同期APIは blocking wait で維持
- 内部のみ非同期化

---

## 6. 追加要素（新規）

### 6.1 timeout

```c
wait_event_timeout(q, cond, timeout);
```

用途：デバイスハング防止

---

### 6.2 エラーハンドリング

- I/O失敗時の return
- completion に status を持たせる

---

### 6.3 デバッグ支援

- wake trace
- queue length監視

---

## 7. 検証（強化）

追加項目：

- race condition テスト
- SMP stress test
- 高頻度I/Oテスト

既存：
- cc1 コンパイル時間
- CPU使用率

---

## 8. 実装方針まとめ

- 非同期化は段階的に行う
- correctness > performance
- IRQは軽く、処理は遅延実行

---

## 9. 結論

割り込み駆動I/Oは性能向上の鍵だが、
本質は「非同期状態管理」である。

本計画では race / SMP / IRQ制約を明示的に扱い、
安全に段階的移行を行う。
