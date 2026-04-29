# Orthox-64 Kernel Issues / Review Notes

このドキュメントは Kernel.md のレビューに基づく課題・改善点を整理したもの。
優先度と影響範囲を明確にし、今後の設計判断の指針とする。

---

# 1. 総評

- 設計の一貫性: 高い
- 実装との整合性: 良好
- 完成度: 約80〜85%
- 主な課題: 構造分離とパフォーマンス最適化

---

# 2. 重要課題（High Priority）

## 2.1 task.c の責務過多

### 問題
- task lifecycle
- run queue
- scheduler連携
- reap処理

が一箇所に集中している

### リスク
- 変更影響範囲が広い
- バグの局所化が困難
- 将来的なスケーラビリティ低下

### 対応方針
- task_reap() の分離
- run queue の sched 側への完全移動

---

## 2.2 run queue と scheduler の未分離

### 問題
- データ構造とポリシーが混在

### リスク
- SMP最適化が困難
- scheduler改良時の影響が大きい

### 対応方針
- run queue を kernel/sched.c に移動
- task.c は状態遷移APIのみに限定

---

## 2.3 interrupt-driven I/O 未実装

### 現状
- VirtIO はポーリング方式
- フェーズ1の待機基盤（`wait_queue`, `completion`, `TASK_IO_WAIT`）は追加済み
- VirtIO Block は legacy IRQ line が有効な場合、single request completion wait に移行済み
- VirtIO Net の割り込み駆動化、MSI/MSI-X、bottom half、multi-request inflight は未実装

### リスク
- CPU効率低下
- スケジューラ評価が不完全

### 対応方針
- MSI/MSI-X 対応
- 割り込み駆動I/Oの導入

---

# 3. 中優先課題（Medium Priority）

## 3.1 syscall dispatcher の肥大化

### 現状
- switch ベースディスパッチ

### リスク
- syscall数増加に伴う保守性低下

### 対応方針
- 将来的なテーブル化
- 関数ポインタ配列への移行

---

## 3.2 kernel lock 粒度

### 現状
- 粒度固定（分割中のため）

### リスク
- SMP性能のボトルネック

### 対応方針
- 分割完了後に再設計
- fine-grained lock 導入検討

---

## 3.3 sys_fs 分割の遅延

### 問題
- FS関連が最大規模にも関わらず後回し

### リスク
- 分割時の影響が最大化

### 対応方針
- 小さな syscall 単位で段階的に分離

---

# 4. 低優先課題（Low Priority）

## 4.1 task_internal.h の肥大化

### リスク
- 内部APIの拡張による構造崩壊

### 対応方針
- 内部APIの最小化
- 使用範囲の制限

---

## 4.2 private syscall の整理

### 問題
- ORTH_SYS_* の管理

### リスク
- ABIの複雑化

### 対応方針
- 名前空間と用途の明確化

---

# 5. 今後の優先実装順

1. task subsystem の完全分離
2. interrupt-driven I/O 実装
3. syscall dispatcher の改善
4. lock 粒度最適化
5. sys_fs 分割

---

# 6. フェーズ定義

## Phase 1: 構造分離
- task / scheduler 分離
- syscall 分割完了

## Phase 2: パフォーマンス改善
- interrupt I/O
- lock最適化

## Phase 3: 安定化
- 長時間テスト
- toolchain 安定性

---

# 7. 結論

現状は「機能完成フェーズ」から「構造最適化フェーズ」への移行段階。

設計の方向性は正しく、主な課題は構造の整理と性能向上である。
