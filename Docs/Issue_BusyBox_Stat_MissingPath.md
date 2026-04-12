# Issue: BusyBox `stat` missing-path crash on RetroFS root

## Summary
- `RetroFS root` 上で BusyBox `stat` を既存ファイルに対して実行するのは成功する。
- 2026-04-11 時点では、missing path に対する `stat /definitely-missing` が userspace error exit ではなく kernel 例外に化けることがあった。
- 原因は BusyBox `stat` 固有ではなく、`task_execve()` が旧ユーザー CR3 を即時解放していたことだった。

## Evidence
- shell 本体の `stat("/non-existent-file-xyz")` は `ret=-1 errno=2` を返していた。
- 初回の再現ログでは BusyBox `stat` 実行直後に `Vector: 000000000000000D` が発生し、`RIP: 0xFFFFFFFF800025D2` は `vmm_free_user_pml4()` 内だった。
- focused regression として `fork -> exec(/bin/staterrno.elf) -> waitpid` を first user task にした `tests/musl_statmissing_smoke.sh` を追加した。
- 同様に `fork -> exec(/bin/stat /definitely-missing) -> waitpid` を first user task にした `tests/musl_busybox_stat_missing_smoke.sh` を追加した。
- 修正後の focused smoke では次が確認できる。
  - `staterrno:start`
  - `staterrno: rc=-1 errno=2`
  - `staterrno:end`
  - `muslstatmissingdriver:status=0`
  - `stat: can't stat '/definitely-missing': No such file or directory`
  - `muslbusyboxstatdriver:status=256`

## Root cause
- `task_execve()` が新しいアドレス空間へ切り替えた直後に旧 `cr3` を `vmm_free_user_pml4()` で解放していた。
- missing-path error exit を踏む child image では、この即時解放が不正な page-table teardown を引き起こし、BusyBox `stat` の userspace failure が kernel exception に見えていた。

## Fix
- `struct task` に `deferred_cr3` を追加した。
- `task_execve()` では旧 `cr3` を即時 free せず `deferred_cr3` に退避するよう変更した。
- `task_reap_locked()` で task 終了時に `deferred_cr3` を解放するよう変更した。

## Verification
- `bash tests/musl_statmissing_smoke.sh orthos.iso`
  - child 側 plain `stat()` missing-path が `errno=2` で正常終了することを確認。
- `bash tests/musl_busybox_stat_missing_smoke.sh orthos.iso`
  - BusyBox `stat` が `No such file or directory` を出して終了し、parent が `waitpid()` で回収できることを確認。

## Residual risk
- bootcmd 経由の広い統合シナリオは focused smoke よりノイズが多い。
- ただし first-user-task に縮めた focused regression では、今回の root cause に対する再現と修正確認は固定できている。
