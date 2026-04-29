# Orthox-64 Documentation Process

この文書は、kernel 整理や syscall 分割を進めるときに、実装変更と同じ粒度で更新する内部文書の計画を定義する。

目的は、文書を後追いの作業にしないこと。リファクタリング後に「なぜこの分割になったか」「どの ABI を守ったか」「どの smoke で確認したか」が追える状態を保つ。

## 1. 基本方針

- kernel の責務分割は、対応する内部文書の更新を伴う。
- 実装だけの変更で、設計前提、ABI、検証方法が変わる場合は不完全な変更として扱う。
- 文書は理想形ではなく、現在の実装に合わせて書く。
- 不明点は推測で断定せず、`要確認` として残す。
- 大きな設計変更は、作業前に「変更前の前提」と「変更後の境界」を文書へ書く。

## 2. 主要文書の役割

| 文書 | 役割 | 更新タイミング |
|:---|:---|:---|
| `Docs/Internal/Kernel.md` | kernel 全体の構造、ABI、subsystem 間の責務、分割計画 | kernel / include / syscall / task / VM / FS の設計境界を変えるとき |
| `Docs/Internal/Documentation_Process.md` | 文書更新のルール、完了条件、変更種別ごとの更新先 | 文書運用ルールを変えるとき |
| `Docs/Dayly_log/Day*_*.md` | 作業ログ、試行錯誤、次回引き継ぎ | その日の作業終了時 |
| `Docs/Issue_*.md` | 個別の不具合、調査メモ、再現条件 | 問題が長期化する、または複数日にまたがるとき |
| `Docs/setup.md` | 環境構築、依存ツール、実行手順 | 開発環境や必須ツールが変わるとき |

## 3. 変更種別ごとの文書更新

| 変更種別 | 更新する文書 | 書く内容 |
|:---|:---|:---|
| syscall 分割 | `Docs/Internal/Kernel.md` | 移動した syscall、移動先ファイル、ABI 変更なしの確認、実行した smoke |
| syscall ABI 変更 | `Docs/Internal/Kernel.md` | syscall 番号、引数、戻り値、`-errno`、musl 影響、互換性リスク |
| VM / memory 変更 | `Docs/Internal/Kernel.md` | address layout、`brk` / `mmap` / COW の仕様、trace counter、性能影響 |
| task / scheduler 変更 | `Docs/Internal/Kernel.md` | task lifecycle、run queue、SMP、fork / exec / wait / signal との境界 |
| FS / rootfs 変更 | `Docs/Internal/Kernel.md` | VFS、RetroFS、RAMFS、mount、rootfs 生成手順、対応 smoke |
| userland ABI 変更 | `Docs/Internal/Kernel.md` | musl / BusyBox / Python / toolchain への影響 |
| build / toolchain 変更 | `Docs/setup.md` と必要なら `Docs/Internal/Kernel.md` | build 手順、依存ツール、rootfs 反映方法、native toolchain 前提 |
| 長期調査 | `Docs/Issue_*.md` または `Docs/Dayly_log/Day*_*.md` | 再現手順、失敗ログ、仮説、切り分け、次に見る counter |

## 4. syscall 分割時の更新手順

syscall 分割では、実装変更ごとに以下を行う。

1. 変更前に `Docs/Internal/Kernel.md` の `kernel/syscall.c` 分割計画を確認する。
2. 移動対象の syscall を 1 つのカテゴリに限定する。
3. `kernel/syscall.c` から移した関数と、新しい所有ファイルを文書に反映する。
4. `include/syscall.h` の Linux 互換番号と `ORTH_SYS_*` 番号が変わっていないことを確認する。
5. musl 互換性に関わる syscall なら、対応する smoke を実行する。
6. 実行した確認を day log または該当 issue に残す。

`syscall.c` から関数を移すだけの場合、文書上は「所有ファイル変更」として記録する。戻り値、引数、lock 粒度、user pointer の扱いを変えた場合は「ABI / 挙動変更」として別に記録する。

## 5. 完了条件

kernel 整理作業は、以下を満たして完了とする。

| 条件 | 内容 |
|:---|:---|
| build | 少なくとも `make kernel.elf` が通る |
| smoke | 変更領域に対応する smoke を最低 1 本実行する |
| ABI | syscall 番号、引数 ABI、`-errno` 規約を変えていないことを確認する |
| docs | 変更した責務、所有ファイル、検証結果を文書へ反映する |
| log | 未解決点、次回作業、失敗した検証を day log に残す |

文書更新が終わっていない場合、実装が動いていても整理作業は完了扱いにしない。

## 6. 文書に残すべき粒度

文書はコードの逐語的な説明ではなく、変更判断に必要な情報を残す。

残すべき情報:
- subsystem の責務境界
- ABI と userland への影響
- lock / lifetime / ownership の前提
- 代表的なデータフロー
- smoke test が保証する範囲
- 既知の未解決問題

残さなくてよい情報:
- 関数内の自明な処理手順
- 一時的な debug print の詳細
- すぐ消す実験コードの全差分
- build log の全文

## 7. 変更記録テンプレート

day log または issue には、必要に応じて以下の形で残す。

```text
## 文書更新
- 更新した文書:
- 変更した設計前提:
- ABI 影響:
- 実行した smoke:
- 残る未解決点:
```

syscall 分割の場合:

```text
## syscall 分割記録
- 移動元:
- 移動先:
- 移動した syscall:
- ABI 変更:
- musl 影響:
- 確認:
- 次に切り出す候補:
```

## 8. 次に整備する文書

優先順位は以下。

1. `Docs/Internal/Kernel.md` の `task.c` 分割計画
2. `Docs/Internal/Kernel.md` の smoke test カタログ
3. `Docs/Internal/Kernel.md` の VM / COW 詳細仕様
4. 必要なら `Docs/Internal/Syscall_Map.md` を分離し、syscall ごとの所有ファイル、ABI、検証 smoke を表にする

`Syscall_Map.md` は、実際に `sys_time.c` などを切り出し始めてから作る。現時点では `Kernel.md` の 10 章を source of truth とする。
