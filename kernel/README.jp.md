# Orthox-64 Kernel Source Structure

このディレクトリには、Orthox-64 カーネルのコアソースコードが含まれています。

## ファイル一覧

### コアと初期化
- **`init.c`**: カーネルのエントリポイント兼初期 bring-up シーケンス。Limine から制御を受け取り、メモリ管理、GDT/IDT、割り込み、syscall、タスク管理、PCI、ネットワーク、USB を順に初期化し、最初のユーザープロセスを起動します。
- **`elf.c`**: ELF ローダー。初回ユーザータスクの起動と、その後の `execve()` 系の実行ファイルロード処理で共通に使われます。

### メモリ管理
- **`pmm.c`**: 物理メモリマネージャー。空き物理ページを追跡し、ページ単位の割り当てと解放を提供します。
- **`vmm.c`**: 仮想メモリマネージャー。x86_64 の 4 段ページテーブルを構築・更新し、ユーザー/カーネル空間のマッピングとアドレス空間切り替えを扱います。

### CPU セットアップと割り込み
- **`gdt.c` / `gdt_flush.S`**: GDT と TSS の設定。カーネル/ユーザー間の特権遷移に必要な基盤を構成します。
- **`idt.c` / `interrupt.S`**: IDT の設定と、割り込み・例外エントリの低レベル stub を実装します。
- **`lapic.c`**: Local APIC タイマーと時刻取得まわりを担当し、スケジューラや `lwIP` timeout 処理に使われます。
- **`pic.c`**: 旧来の 8259 PIC の制御を担当し、IRQ ルーティング互換を支えます。

### タスク管理とプロセス制御
- **`task.c` / `task_switch.S`**: プリエンプティブ・スケジューラ、タスク生成、プロセスコンテキスト、fork/exec 補助、低レベルのコンテキスト切り替えを実装します。
- **`syscall.c` / `syscall_entry.S`**: `syscall` エントリとディスパッチャ。ファイル、プロセス、TTY、signal、network、USB、および Orthox-64 固有 syscall を処理します。

### ファイルシステムとコンソール I/O
- **`fs.c`**: VFS 層。ファイル記述子テーブル、tar ベース rootfs、ディレクトリ走査、TTY/console 経路、汎用 `read`/`write` ディスパッチを担当します。
- **`keyboard.c`**: PS/2 キーボード入力と serial 入力経路を処理し、shell が読むコンソール入力バッファへ文字を供給します。

### PCI, USB, 各種デバイス
- **`pci.c`**: PCI 列挙。`virtio-net`、オーディオ、xHCI などのデバイスを発見します。
- **`usb.c`**: USB/xHCI と mass storage 寄りの処理。USB ストレージ読み出しや rootfs mount 実験の土台です。
- **`sound.c`**: Intel HD Audio / PCM 再生機能を実装します。

### ネットワーク
- **`virtio_net.c`**: 最小 `virtio-net` ドライバ。legacy virtio-pci I/O BAR、RX/TX virtqueue、MAC 取得、polling ベースの frame 送受信を実装します。
- **`net.c`**: NIC の薄い抽象層。frame 送信、MAC 参照、RX callback 登録、polling を上位へ公開します。
- **`lwip_port.c`**: `lwIP` 統合層 (`NO_SYS=1`)。netif bring-up、DHCP/DNS/timeout 処理、ARP/ICMP 診断、UDP echo、kernel 側 DNS lookup glue を担当します。
- **`net_socket.c`**: `lwIP` 上の kernel socket backend。現在の `AF_INET` 向け UDP/TCP socket、`bind/connect/listen/accept/send/recv`、fd 統合を実装します。

### freestanding libc 断片
- **`cstring.c`**: カーネル本体や `lwIP` などの vendored component が必要とする最小限の文字列・メモリ操作を実装します。
- **`cstdio.c`**: カーネル内で使う最小限の整形出力と serial 指向の stdio 補助を実装します。
- **`cstdlib.c`**: freestanding カーネルで必要になる最小限の libc 風ユーティリティ関数を実装します。

## 補足
- **`idt.c.bak_doom_sched_test`**: DOOM bring-up 期のスケジューラ実験から残してあるバックアップファイルです。
