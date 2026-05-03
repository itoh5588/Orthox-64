# Future Notes

## 1. RPi4 移植は延期

Raspberry Pi 4 への移植は、しばらく延期する。

理由:

- ARM 実機移植は board / firmware / interrupt / timer / UART / boot の固有性が強い
- 「CPU の違い」ではなく「プラットフォーム全体の違い」への対応が必要
- 今の Orthox-64 に対しては、投資効率があまり良くない

結論:

- 次の移植先は ARM 実機より `RISC-V virt (QEMU)` を優先する

## 2. 次アーキテクチャ候補は RISC-V `virt`

RISC-V `virt` を優先する理由:

- QEMU 上で標準的な bring-up 経路を取りやすい
- OpenSBI + DTB + virtio-mmio で整理しやすい
- `arch` 分離と portable kernel 化の学習に向いている

今後の基本方針:

1. `arch/platform` 分離
2. RISC-V `virt` single-core bring-up
3. shell / file / process / network を接続
4. SMP は最後

## 3. xv6-riscv は低レベル実装の参考になる

xv6-riscv は、Orthox-64 にとって次の点で有用。

- trap / interrupt entry
- syscall entry / return
- Sv39 page table
- context switch
- hart bring-up
- timer interrupt
- UART
- virtio の最小 transport

ただし位置づけは、

- 完成形の見本ではない
- RISC-V 下回りの最小手本

である。

Orthox-64 はすでに次の領域で xv6 より進んでいる。

- SMP scheduler
- per-CPU run queue
- limited migration
- network
- socket
- userspace の厚み

## 4. Orthox-64 の強み

Orthox-64 は hobby OS としてかなり高水準。

特に強い点:

- SMP
  - 4 CPU bring-up
  - per-CPU run queue
  - resched IPI
  - limited migration
- wait/wake の race 整理
  - pipe
  - wait4
  - socket
  - DNS
- network
  - virtio-net
  - HTTPS client
  - UDP echo
  - BusyBox httpd
- userspace
  - shell
  - BusyBox ash
  - musl/newlib 系
- DOOM 起動

また、強さの大きな理由として次がある。

- 車輪の再発明を避けた
- 良い OSS を積極的に利用した
  - musl
  - BusyBox
  - lwIP
  - BearSSL
  - DOOM
- それらを動かすために kernel 側を改善した

## 5. 将来の userspace 拡張

RISC-V 移植後の大きな目標として、次が挙がっている。

- Python の移植
- Apache の移植

ただし順番としては、

1. Python
2. その後に Apache

が自然。

理由:

- Python は単体プロセスで動かしやすく、OS の userspace 互換性を高める効果が大きい
- Apache は process model, socket, signal, config, permission model まで含めて要求が重い

## 6. その前に必要なもの: protection model

Python や Apache の前に、Orthox-64 には保護モデルの導入が必要。

現状:

- file metadata に一部 `uid/gid` 情報はある
- しかし task credential は未整備
- permission check は未実装
- 実質 root 固定に近い

必要な段階:

1. task credential
   - uid
   - gid
   - euid
   - egid
2. file / directory ownership
   - mode
   - uid
   - gid
3. permission check
   - open
   - create
   - unlink
   - mkdir
   - exec
4. userspace API
   - getuid/getgid
   - setuid/setgid
   - chmod/chown
   - umask
   - access
5. `/etc/passwd`, `/etc/group`, login

結論:

- portability の次に来る大テーマは security / multi-user

## 7. 中長期の流れ

今後の大きな流れは次のように考える。

1. x86_64 で `arch` 分離を進める
2. RISC-V `virt` bring-up
3. portable kernel 化を進める
4. protection model を導入する
5. Python などの大型 userspace を移植する
6. その後に Apache のような本格 server を検討する

## 8. 一言でまとめると

Orthox-64 はすでに、

- SMP
- network
- userspace
- DOOM

まで到達している、かなり本格的な experimental hobby OS である。

次の段階は、

- multi-arch 化
- protection model
- より厚い userspace

であり、その最初の現実的なターゲットは `RISC-V virt`。
