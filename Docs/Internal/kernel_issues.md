# Orthox-64 Kernel Issues (2026-05-06)

このファイルは、xv6fs 上での native self-compile 達成後に残っている kernel 課題を、優先順位つきで追跡するための作業リスト。
既存の全体レビューは `Docs/Internal/issues.md`、ASSERT 導入の背景メモは `Docs/Issue_Kernel_Assert_Panic.md` を参照する。

## Current Evidence

- xv6fs rootfs 上で guest native build を実行し、`/kernel.elf` の作成まで到達済み。
- host 側で `rootfs.img` から `/kernel.elf` を抽出し、サイズ `6,340,544 bytes` の ELF として確認済み。
- 抽出した native kernel の boot smoke は `native-kernel-boot-ok` まで到達済み。
- 既知の失敗要因だった 1KiB 抽出問題は、host extractor が sparse hole (`blk == 0`) を EOF 扱いしていたことが原因。

## Background: xv6fs Scope and Orthox-64 Extension

xv6 fs は教育用 OS の教材としては非常に信頼できるが、元の設計範囲は小さい。
元 xv6 の前提は、小規模 disk、小規模 file、単純な block mapping、少数の system call path、教材として読みやすい実装であり、full toolchain を rootfs に置いて guest 内で self-compile する用途は想定していない。

Orthox-64 では、`gcc`、`cc1`、`as`、`ld`、`make`、libc headers/libs、`/kbuild/*.o`、`/kernel.elf` を xv6fs 上に置き、さらに guest 内から更新する。
この時点で、単なる「xv6fs の移植」ではなく、xv6fs を self-hosting 用 rootfs として拡張する作業になっている。

今回の失敗要因は、元 xv6 fs が不安定だったことではない。
元 xv6 fs が信頼できる範囲を超えて、Orthox-64 側で large file / triple indirect / sparse file / persistent build cache / native self-compile へ拡張したにもかかわらず、block allocation/read/write だけでなく truncate/unlink/iput reclaim、host inspection、consistency check、smoke test まで同時に追従させる必要があるという前提が不足していた。

今後の判断基準:

- xv6fs 原型は、元 xv6 の制約内では信頼できる教材実装として扱う。
- Orthox-64 版 xv6fs は、triple indirect と self-compile 対応を入れた時点で独自派生 fs として扱う。
- large file 対応を変更する場合は、allocation / lookup / read / write / truncate / unlink / iput / bitmap consistency / host tool / smoke test を同時に確認する。
- 「boot した」だけでは filesystem correctness の evidence としない。
- host inspection と `--check`、guest smoke、抽出後の byte-level comparison を evidence とする。

## Progress Log

- 2026-05-06: Rank 1 の host xv6fs inspection tool として `scripts/build_rootfs_xv6fs.py` に `--ls` / `--stat` / `--dump-inode` / `--check` を追加。
  - `/tmp/orthox-xv6fs-check.img` を `rootfs/` から作成し、`--ls /`、`--stat /hello.c`、`--dump-inode 1`、`--check` を確認。
  - `--check` は `OK: 0 warning(s)`。
  - 既存の `--extract /hello.c` も確認し、`rootfs/hello.c` との `cmp` が PASS。
- 2026-05-06: Rank 2 の sparse file semantics を固定。
  - kernel 側 `xv6fs_readi()` は `bmap_lookup(..., alloc=0)` で `blk == 0` を zero-fill する実装であることを確認。
  - host 側 `scripts/build_rootfs_xv6fs.py` に検証用 `--sparsify-zero-blocks FS_PATH IMG_FILE` を追加。
  - `/tmp/orthox-sparse.img` で `/sparse.bin` の 4 block を hole 化し、`--stat` で `sparse holes: 4`、`--check` で `OK: 0 warning(s)` を確認。
  - `--extract /sparse.bin` 後、元データとの `cmp` が PASS。host extractor は hole を zero-fill し、file size を保持する。
  - guest kernel read path 用に `user/xv6_sparse_test.c` と `tests/xv6_sparse_smoke.sh` を追加し、`make user/xv6_sparse_test.elf` は PASS。
  - `Limine` boot files は local `template/limine/` から復元する Makefile fallback を追加。
  - `doomgeneric` は local-only port のため rootfs の必須依存から外し、`user/doomgeneric.elf` が存在する場合だけ rootfs にコピーする方針へ変更。
  - `make rootfs.img` と `make orthos.iso` が PASS。
  - `bash tests/xv6_sparse_smoke.sh orthos.iso` が PASS。QEMU 内で `xv6-sparse-smoke: PASS` を確認。
  - smoke 後、`rootfs.img` 内の `/etc/bootcmd` を通常 bootcmd に戻し、`--check rootfs.img` は `OK: 0 warning(s)`。
- 2026-05-06: Rank 3 の xv6fs block reclaim を修正。
  - 原因は元 xv6 fs の欠陥ではなく、Orthox-64 側で `xv6fs_truncate_file()` と `xv6fs_iput()` が `xv6fs_itrunc()` に接続されておらず、inode size/type だけ更新して block bitmap を戻していなかったこと。
  - `xv6fs_itrunc_to()` を追加し、direct / indirect / double indirect / triple indirect の data block と空になった indirect metadata block を解放するよう修正。
  - `truncate(path, smaller)` / `ftruncate(fd, smaller)` は縮小範囲の block を解放し、`unlink` 後の最終 `iput` は全 block を解放する。
  - `user/xv6_reclaim_test.c` と `tests/xv6_reclaim_smoke.sh` を追加。
  - `bash tests/xv6_reclaim_smoke.sh orthos.iso` が PASS。QEMU 内で `xv6-reclaim-smoke: PASS` を確認。
  - guest 実行後の host `--check rootfs.img` は `OK: 0 warning(s)`、`unreferenced: 0 bitmap-allocated data blocks`。
  - smoke 後、`rootfs.img` 内の `/etc/bootcmd` は通常の `/bin/ash` に復元済み。
- 2026-05-06: Rank 4 の xv6fs large write / log chunk を修正。
  - `kernel/fs.c` の `XV6FS_WRITE_CHUNK_MAX` を 64 KiB から 112 KiB に戻した。`XV6FS_LOGBLOCKS=126` に対し、worst-case allocating write は data block 112 + bitmap block + indirect metadata + inode で 126 未満に収まる前提。
  - triple-indirect 境界の write smoke で、`xv6fs_writei()` の最大 file size 判定が 32-bit 乗算で wrap し、triple-indirect 開始位置以降を reject していることを確認。
  - `xv6fs_writei()` の `XV6FS_MAXFILE * XV6FS_BSIZE` 判定を 64-bit 計算へ修正した。
  - `user/xv6_largewrite_test.c` と `tests/xv6_largewrite_smoke.sh` を追加。通常の 112 KiB sequential write と、triple-indirect 開始位置への sparse 112 KiB write/readback/unlink を確認する。
  - `bash tests/xv6_largewrite_smoke.sh orthos.iso` が PASS。QEMU 内で `xv6-largewrite-smoke: PASS` を確認。
  - guest 実行後の host `--check rootfs.img` は `OK: 0 warning(s)`、`unreferenced: 0 bitmap-allocated data blocks`。
  - smoke 後、`rootfs.img` 内の `/etc/bootcmd` は通常の `/bin/ash` に復元済み。
- 2026-05-06: Rank 5 の ASSERT policy を文書化。
  - `Docs/Internal/Kernel.md` に `ASSERT / panic policy` を追加し、kernel 内部 invariant だけを ASSERT / panic 対象にする方針を source of truth 化した。
  - syscall 引数、user pointer、path、fd、missing file、容量不足、unsupported syscall など userland 由来の通常エラーは Linux 互換 ABI の負の `-errno` で返す。
  - `KASSERT()` は外部入力 validation shortcut として使わない。`KBUG_ON()` / panic は復旧不能な内部矛盾に限定する。
  - Rank 5 の invalid input smoke は `tests/vm_syscall_smoke.sh` / `user/vmerrno_test.c` とする。
  - `bash tests/vm_syscall_smoke.sh orthos.iso` が PASS。QEMU 内で `vmerrno_test: PASS` を確認し、missing path、bad fd、unmapped `mprotect`、unsupported syscall が panic せず errno で戻ることを確認。
  - smoke 後、`rootfs.img` 内の `/etc/bootcmd` は通常の `/bin/ash` に復元済み。host `--check rootfs.img` は `OK: 0 warning(s)`。
- 2026-05-06: Rank 6 の ASSERT core を追加。
  - `include/kassert.h` と `kernel/kassert.c` を追加し、kernel 共通の `KASSERT()` / `KBUG_ON()` / `kernel_panic()` を実装。
  - `kernel_panic()` は serial に `*** KERNEL PANIC ***`、expression、function、file、line を出力し、`cli; hlt` loop で停止する。
  - `kernel/init.c` に `ORTHOX_KASSERT_SELFTEST` 付き build 専用の意図的 `KASSERT(0 && "ORTHOX_KASSERT_SELFTEST")` を追加。通常 build では無効。
  - `make kernel.elf` が PASS。
  - `make -B KERNEL_CFLAGS_EXTRA=-DORTHOX_KASSERT_SELFTEST kernel.elf` と `ROOTFS_REBUILD=0 make orthos.iso` 後、QEMU serial で `*** KERNEL PANIC ***`、`expr: 0 && "ORTHOX_KASSERT_SELFTEST"`、`func: _start`、`file: kernel/init.c`、`HALTING...` を確認。
  - selftest 後に `make -B kernel.elf` と `ROOTFS_REBUILD=0 make orthos.iso` で通常 kernel / ISO に戻した。
  - 通常 ISO の QEMU boot は `/bin/ash` まで到達し、selftest なしの kernel が panic しないことを確認。
- 2026-05-06: Rank 7 の xv6fs / xv6log ASSERT 適用を実施。
  - `kernel/xv6fs.c`、`kernel/xv6log.c`、`kernel/xv6bio.c` の独自 `*_PANIC` macro を廃止し、Rank 6 の `KASSERT()` に接続した。
  - 対象は既に panic 相当だった内部 invariant に限定した。例: bfree double-free、inode cache exhaustion、bad inode lock/unlock、inode type 0、bmap out of range、dirlink short read、log transaction overflow、transaction outside write、buffer cache exhaustion。
  - path lookup failure、missing file、disk full、allocation failure など userland / environment 由来の通常エラーは ASSERT 化していない。
  - `make kernel.elf` が PASS。
  - `bash tests/xv6_largewrite_smoke.sh orthos.iso` が PASS。QEMU 内で `xv6-largewrite-smoke: PASS`、host `--check rootfs.img` は `OK: 0 warning(s)`。
  - `bash tests/xv6_reclaim_smoke.sh orthos.iso` が PASS。QEMU 内で `xv6-reclaim-smoke: PASS`、host `--check rootfs.img` は `OK: 0 warning(s)`。
  - `bash tests/xv6_sparse_smoke.sh orthos.iso` が PASS。QEMU 内で `xv6-sparse-smoke: PASS` を確認。
  - smoke 後、`rootfs.img` と `rootfs/etc/bootcmd` は通常の `/bin/ash` に復元済み。host `--check rootfs.img` は `OK: 0 warning(s)`。
  - `native_kernel_boot_smoke.sh` は最大 3600 秒の長時間 smoke のため、Rank 9 の native boot smoke 標準統合で扱う。
- 2026-05-06: Rank 8 の `pread64` / `pwrite64` ABI smoke を追加。
  - `kernel/syscall.c` の `pread64` / `pwrite64` negative offset 戻り値を `-EINVAL` に修正。従来の `-1` は musl 側で `EPERM` になり、Linux 互換 errno とずれていた。
  - `user/preadpwrite_test.c` と `tests/pread_pwrite_smoke.sh` を追加。
  - smoke は `pread()` / `pwrite()` が fd current offset を変更しないこと、negative offset が `EINVAL` を返すこと、negative offset 後も fd current offset が保持されることを確認する。
  - `make kernel.elf` と `make user/preadpwrite_test.elf` が PASS。
  - `bash tests/pread_pwrite_smoke.sh orthos.iso` が PASS。QEMU 内で `preadpwrite-smoke: PASS` を確認。
  - smoke 後、`rootfs.img` と `rootfs/etc/bootcmd` は通常の `/bin/ash` に復元済み。host `--check rootfs.img` は `OK: 0 warning(s)`。
- 2026-05-06: Rank 9 の native kernel boot smoke 標準検証統合を実施。
  - 既存の `make nativekernelbootsmoke` / `tests/native_kernel_boot_smoke.sh` は、guest native build、`/kernel.elf` 抽出、native kernel boot marker 確認までを 1 command で実行できる。
  - `tests/final_smoke_suite.sh` に native kernel boot smoke を opt-in 統合した。
  - 通常の `make finalsmokesuite` では長時間 native build を skip 表示する。`RUN_NATIVE_KERNEL_BOOT_SMOKE=1 make finalsmokesuite` のときだけ `tests/native_kernel_boot_smoke.sh` を実行する。
  - `ROOTFS_REBUILD=0` を使う既存 smoke 手順により、`/kbuild/*.o` cache を保持したまま ISO を作る。
  - `bash -n tests/final_smoke_suite.sh` と `bash -n tests/native_kernel_boot_smoke.sh` が PASS。
  - `make kernel.elf` と `git diff --check` が PASS。
- 2026-05-06: Rank 10 の `/kbuild` cache workflow を文書化。
  - `ROOTFS_REBUILD=0 make orthos.iso`、`make persist-run` 系 target、`scripts/build_rootfs_xv6fs.py --replace` は既存 `rootfs.img` を保持する操作として整理した。
  - default の `make rootfs.img` / `make orthos.iso`、clean rebuild、`rootfs.img` の削除・置換は `/kbuild/*.o` と `/kernel.elf` を失う操作として明記した。
  - Limine module rootfs への guest write は host `rootfs.img` へ戻らないため、cache 永続化には writable VirtIO block の `rootfs.img` を使う前提を明記した。
  - 事前/事後確認は `python3 scripts/build_rootfs_xv6fs.py --ls /kbuild rootfs.img`、`--check rootfs.img`、`/etc/bootcmd` 抽出で行う。
- 2026-05-06: Rank 11 の VFS / fd ASSERT を追加。
  - `kernel/fs.c` に `fs_assert_open_file_consistent()` と `fs_fd_data_required()` を追加し、open fd の shared file refcount、descriptor type と file type の一致、pipe / socket / xv6fs / dir data pointer の non-null を確認する。
  - `read` / `write` / `close` / `fstat` / `getdents` / `getdents64` の dereference 直前に限定して ASSERT を入れた。
  - bad fd、missing path、RAMFS の path lookup failure、disk full など userland / environment 由来の通常エラーは ASSERT 化していない。
  - `make kernel.elf` が PASS。
- 2026-05-06: Rank 12 の VM / task ASSERT を追加。
  - `kernel/vmm.c` に page table pointer non-null、page-aligned physical address、user PML4 と kernel PML4 の取り違え、COW page refcount > 0 の ASSERT を追加した。
  - `kernel/task.c` に run queue 上 task の state、reap 対象 task の kernel stack / CR3 alignment の ASSERT を追加した。
  - `kernel/task_fork.c` は child を run queue に載せる前に COW PML4 copy、kernel stack allocation、fd clone を完了する順序へ修正した。allocation failure は panic ではなく rollback して `-1` を返す。
  - `make kernel.elf` が PASS。
  - `ROOTFS_REBUILD=0` と `--replace` で `/etc/bootcmd` だけを `/bin/muslforkprobe.elf` へ差し替え、QEMU 内で `muslforkprobe:child`、`muslforkprobe:status=42`、`muslforkprobe:ok` を確認。
  - smoke 後、`rootfs.img` の `/etc/bootcmd` は `/bin/ash` に復元済み。host `--check rootfs.img` は `OK: 0 warning(s)`。
- 2026-05-07: Rank 13 の VirtIO / storage ASSERT を追加。
  - `kernel/virtio.c` に virtqueue size、ring alignment、descriptor / avail / used pointer の ASSERT を追加した。
  - `kernel/virtio_blk.c` に request context ownership、descriptor head 範囲、header/status DMA address、block request type、data DMA address の ASSERT を追加した。
  - `kernel/virtio_net.c` に RX/TX queue readiness、TX used descriptor id、RX descriptor id 範囲、RX/TX DMA buffer alignment の ASSERT を追加した。
  - device 未検出、request timeout、I/O status failure、TX busy、invalid send length など通常失敗は ASSERT 化していない。
  - 途中、kernel source 変更後は `/kbuild/*.o` cache を守る意味が薄いと判断を修正し、`rootfs.img` / `orthos.iso` を正規 rebuild して `/etc/bootcmd=/bin/ash` と `--check` warning 0 を回復した。
  - `make kernel.elf` が PASS。
  - `bash tests/virtio_blk_inflight_smoke.sh orthos.iso` が PASS。`reqs=0x8`、write、readback persistence を確認。
  - `bash tests/virtio_net_irq_smoke.sh orthos.iso` は sandbox 内 socket 作成が `PermissionError` になったため、許可付きで再実行して PASS。`virtio-net ready ... msix=1`、IRQ bottom half、DHCP bound、UDP echo を確認。
  - smoke 後、`rootfs.img` / `orthos.iso` を再生成し、`/etc/bootcmd` は `/bin/ash`、host `--check rootfs.img` は `OK: 0 warning(s)`。
- 2026-05-07: Rank 14 の ASSERT build config を決定。
  - `include/kassert.h` に `ORTHOX_ENABLE_ASSERTS` を追加し、未指定時は `1` として `KASSERT()` を有効にする。
  - `KERNEL_CFLAGS_EXTRA=-DORTHOX_ENABLE_ASSERTS=0` の build では `KASSERT(expr)` は式を評価しない no-op になる。
  - `KBUG_ON(cond)` は release / assert-disabled build でも常時有効とした。継続不能な kernel bug 検出は消さない。
  - `ORTHOX_KASSERT_SELFTEST` は panic 出力確認専用で、通常 build では無効のまま。
- 2026-05-07: Rank 15 の repository hygiene を進めた。
  - `.gitignore` に `user/*.so` を追加し、`user/libdyn_*.so` のような generated shared library を local-only artifact に寄せた。
  - `Docs/` と `LOGs/` は引き続き local-only evidence / worklog として ignored のまま維持する。
  - `Limine` は submodule / external dependency として残す前提で、ignore ではなく submodule 管理を前提に扱う。
  - 追跡対象は source code と必要な build/test script に絞り、generated binary / image / log は ignore 側へ寄せる方針を明文化した。

---

## Priority List

| Rank | ID | Priority | Area | 課題 | 対応方針 | 完了条件 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | KISSUE-XV6FS-001 | Critical | host xv6fs tooling | xv6fs の観測手段が不足しており、失敗時に inode / block / indirect block の状態を即座に確認できない | host tool に `--ls` / `--stat` / `--dump-inode` / `--check` を追加する | 失敗時に `rootfs.img` の inode、size、direct / indirect block、free block 状態を host 側で説明できる |
| 2 | KISSUE-XV6FS-002 | Critical | xv6fs sparse file | sparse hole の semantics が kernel read path と host tooling で揃っているか要確認 | `blk == 0` は EOF ではなく zero-fill として扱うことを仕様化し、host extractor / kernel read を検証する | host sparse extract PASS。guest QEMU sparse smoke PASS |
| 3 | KISSUE-XV6FS-003 | High | xv6fs reclaim | truncate / unlink / iput 時の block 回収が保守的で、長期運用では block leak になる可能性がある | direct / indirect / double / triple indirect の shrink / unlink reclaim を実装。巨大 file の transaction 分割は継続課題 | reclaim smoke PASS。host `--check rootfs.img` で unreferenced block 0 |
| 4 | KISSUE-XV6FS-004 | High | xv6fs large write | 現在の write chunk は安全寄りだが、性能と log 制約の根拠が文書化されていない | xv6 log capacity から最大 transaction size を再計算し、必要なら chunk size を戻す | PASS: 112 KiB chunk に戻し、sequential / triple-indirect large write smoke と host `--check` が PASS |
| 5 | KISSUE-ASSERT-001 | High | ASSERT policy | panic すべき条件と errno で返す条件が未整理 | 「内部不変条件のみ ASSERT」「外部入力は errno」を `Kernel.md` に明記する | PASS: `Kernel.md` に方針を明記。`vm_syscall_smoke` で invalid input が panic せず errno で戻ることを確認 |
| 6 | KISSUE-ASSERT-002 | High | ASSERT core | kernel 共通の `KASSERT()` / `KBUG_ON()` / panic 出力基盤がない | `include/kassert.h` と `kernel_panic()` を追加し、file / line / function / expression を serial に出す | PASS: `ORTHOX_KASSERT_SELFTEST` 付き QEMU boot で panic 内容を serial 確認。通常 boot は `/bin/ash` 到達 |
| 7 | KISSUE-ASSERT-003 | High | xv6fs / xv6log ASSERT | bmap 範囲、inode 種別、log transaction 状態、dirent 境界の破損検出が弱い | xv6fs の内部専用パスに段階的に `KASSERT()` を入れる | PASS: xv6 largewrite / reclaim / sparse smoke と host `--check` が PASS。native boot smoke は Rank 9 で標準統合 |
| 8 | KISSUE-XV6FS-005 | Medium | syscall ABI | `pread64` / `pwrite64` の offset preservation と Linux ABI 互換が未整理 | fd offset が変わらないこと、negative offset の errno を smoke で確認する | PASS: offset preservation と negative offset `EINVAL` targeted smoke が PASS |
| 9 | KISSUE-XV6FS-006 | Medium | smoke test | native kernel boot smoke が標準検証手順に完全統合されていない | `nativekernelbootsmoke` を final smoke の一部として安定化する | PASS: `make nativekernelbootsmoke` は 1 command 済み。`RUN_NATIVE_KERNEL_BOOT_SMOKE=1 make finalsmokesuite` で opt-in 統合 |
| 10 | KISSUE-XV6FS-007 | Medium | `/kbuild` cache | `/kbuild/*.o` を保持する条件と失われる条件が明文化されていない | `ROOTFS_REBUILD=0`、`--replace`、clean rebuild の違いを文書化する | PASS: `Kernel.md` と `self_compiled_kernel_plan.md` に cache 保持/消失条件、確認コマンドを明記 |
| 11 | KISSUE-ASSERT-004 | Medium | VFS / fd ASSERT | fd type と private data の対応不整合を早期検出できない | `FT_XV6FS` / `FT_RAMFS` / pipe / socket ごとの invariant を ASSERT 化する | PASS: open fd object と private data の ASSERT を追加。fd / xv6fs smoke と host `--check` で通常 path を確認 |
| 12 | KISSUE-ASSERT-005 | Medium | VM / task ASSERT | page table、address range、COW refcount の破損検出が局所的 | task / VM の境界で non-null、range、refcount invariant を追加する | PASS: VM / task ASSERT 追加。`muslforkprobe` fork/COW smoke と host `--check` が PASS |
| 13 | KISSUE-ASSERT-006 | Medium | VirtIO / storage ASSERT | descriptor index、request lifecycle、DMA address の破損検出が弱い | VirtIO blk/net の request state machine に ASSERT を追加する | PASS: VirtIO blk inflight smoke と VirtIO net IRQ/MSI-X smoke が PASS。bootcmd `/bin/ash` と host `--check` warning 0 |
| 14 | KISSUE-ASSERT-007 | Medium | build config | debug / release で ASSERT 動作をどう切り替えるか未決定 | `ORTHOX_DEBUG_ASSERT` などの build flag を決める | PASS: default `ORTHOX_ENABLE_ASSERTS=1`、optional `=0`、`KBUG_ON` 常時有効を文書化 |
| 15 | KISSUE-REPO-001 | Low | repository hygiene | `Docs/`、`LOGs/`、rootfs artifacts、ports build products の追跡方針が未確定 | GitHub に載せるものと local-only artifact を分離する | PASS: `.gitignore` に generated shared libs を追加し、local-only artifact の境界を明文化 |

---

## Immediate Execution Order

1. host xv6fs inspection tool を強化する。
2. sparse file semantics を固定し、host / kernel 双方の smoke を追加する。
3. xv6fs block reclaim の現状を計測し、transaction 分割付き free 実装へ進む。
4. large write chunk size を xv6 log capacity から再計算し、性能劣化がある箇所を戻す。
5. ASSERT policy を文書化してから、最小 `KASSERT()` / panic 基盤を追加する。
6. xv6fs / xv6log の内部 invariant に限定して ASSERT を入れる。
7. `pread64` / `pwrite64`、native boot smoke、`/kbuild` cache workflow を標準検証へ入れる。

---

## ASSERT Policy

- ASSERT は user input の検証ではなく、kernel 内部 invariant の破損検出に使う。
- syscall 引数、user pointer、path、fd 番号など userland 由来の通常エラーは `-EINVAL` / `-EFAULT` / `-ENOENT` / `-EBADF` などで返す。
- `KASSERT()` は「ここが壊れていたら kernel bug」と言える場所に限定する。
- `KBUG_ON()` / panic は、復旧不能な内部矛盾や metadata 破損の検出に限定する。
- ASSERT 導入前に、invalid user input で panic しない smoke を用意する。
- Rank 5 の基準 smoke は `tests/vm_syscall_smoke.sh`。missing path、bad fd、unmapped address、unsupported syscall が panic ではなく errno で戻ることを確認する。

---

## Completion Criteria

- `make kernel.elf` が成功する。
- xv6fs rootfs 上で `/kernel.elf` を作成できる。
- host 側で `/kernel.elf` を抽出し、ELF として確認できる。
- 抽出した native kernel が boot marker まで到達する。
- host tool で inode / block / sparse hole / free block 状態を確認できる。
- invalid user input で kernel panic せず、errno で戻る。
- internal invariant 破損時だけ ASSERT / panic で止まる。
