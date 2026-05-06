# Orthox-64 カーネル セルフビルド 実装記録

Orthox-64 上で、Orthox-64 自身のカーネル (`kernel.elf`) をビルドする機能の設計・実装記録。

> **2026-05-03 達成。** Orthox-64 が自分自身をコンパイルし、そのカーネルで起動した。

> **2026-05-05 更新。** root filesystem は RetroFS から xv6fs へ移行済み。現在のネイティブカーネルビルドは
> `/tmp/kbuild`（揮発領域）ではなく `/kbuild`（xv6fs 上の永続ディレクトリ）を使い、
> `.o` キャッシュを `rootfs.img` に残してインクリメンタル再開できる構成へ移行中。

---

## 目的

「OS が自分自身をビルドできる」状態を実現する。  
具体的には、Orthox-64 の rootfs 内にカーネルソースを配置し、ゲスト上の `make` / `cc1` / `as` / `ld` で `kernel.elf` を生成できることを確認する。

---

## 前提条件（実装開始時点）

- セルフコンパイル環境（`gcc` / `cc1` / `as` / `ld` / `make`）は既に rootfs に存在
- `cc1`: GCC 4.7.4 のフロントエンド（`/bin/cc1`）
- `as`: GNU Binutils 2.26 の GAS（`/bin/as`）
- `ld`: GNU Binutils 2.26 のリンカ（`/bin/ld`）
- `make`: GNU Make 4.4.1（`/bin/make`）
- 動的リンク（musl libc）環境は完成済み

---

## 調査フェーズ

### ホスト側ビルドとの差分確認

ホスト `Makefile` の `KERNEL_CFLAGS`:
```
-target x86_64-elf   ← clang 専用。native GCC では不要（削除するだけ）
-std=c11 -ffreestanding -fno-stack-protector -fno-stack-check
-fno-lto -fno-PIE -mno-80387 -mno-mmx -mno-sse -mno-sse2
-mno-red-zone -mcmodel=kernel -O2 -Wall -Wextra
-Iinclude -Iports/lwip/src/include
```

GCC 4.7.4 での互換性確認：
- `-std=c11`: GCC 4.7 で追加（C11 初期サポート） ✓
- `-mcmodel=kernel`: x86_64 GCC 標準オプション ✓
- `-fno-stack-check`, `-fno-lto`, `-fno-PIE` 等: 全て GCC 4.7.4 対応 ✓
- `__attribute__((interrupt))`: **不使用**（割り込みは .S ファイルで実装） ✓
- `_Noreturn`: C11 キーワード、GCC 4.7 対応 ✓

### `user/gcc.c` ラッパーの制約（問題発覚）

Orthox-64 上の `gcc` コマンドは `user/gcc.c` によるラッパー。  
調査の結果、以下のフラグが**通らない**ことが判明：

| フラグ種別 | 例 | gcc.c の扱い |
|---|---|---|
| `-f*` フラグ | `-ffreestanding`, `-fno-PIE` | "unsupported option" でエラー |
| `-m*` フラグ | `-mcmodel=kernel`, `-mno-sse2` | "unsupported option" でエラー |
| `.S` 入力 | `kernel/gdt_flush.S` | "unsupported input file" でエラー |

また gcc.c は cc1 引数に `-nostdinc -I/usr/include` を**ハードコード**しており、カーネルの独自 `include/stdlib.h` より musl の `stdlib.h` が先に検索される問題もある。

### 解決策の選択

`gcc.c` を改修する代わりに、**`cc1` / `as` / `ld` を直接呼び出す** Makefile を用意する方針を採用。

理由：
- gcc.c 改修は既存の user-space ビルドへの影響リスクがある
- cc1 は GCC のすべてのコンパイルフラグを直接受け付ける
- cc1 → as → ld は GCC が内部で行うのと同じパイプライン

### インクルードパスの設計

問題：
- `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<stdarg.h>` は musl の `/usr/include` から取得が必要（GCC 4.7.4 の built-in ヘッダは rootfs に未展開）
- `<stdlib.h>` はカーネル独自の `include/stdlib.h` を優先する必要がある
- lwip の `arch.h` が `<stdlib.h>` を `abort()` のために include するが、カーネルの `stdlib.h` に `abort()` はない

解決策：
1. `-I$(SRCDIR)/include` を `-I/usr/include` より**前**に指定 → カーネルの `stdlib.h` / `stdio.h` が優先
2. lwip assertion を `-DLWIP_NOASSERT` で無効化 → `abort()` の宣言が不要になる

最終インクルード順：
```
-I$(SRCDIR)/include
-I$(SRCDIR)/ports/lwip/src/include
-I/usr/include
```

### アセンブリファイルの確認

`kernel/*.S` ファイルを確認したところ、C プリプロセッサディレクティブ（`#include`, `#define`）を**使用していない**。  
→ `as` に直接渡すだけでよい（cc1 の `-E` ステップ不要）

---

## 実装フェーズ

### ステップ 1: `scripts/Makefile.kernel-native` 作成 ✅

Orthox-64 上でカーネルをビルドするための専用 Makefile。

設計：
- `CC1 = /bin/cc1`, `AS = /bin/as`, `LD = /bin/ld`
- `.c` → `cc1 [flags] src.c -o src.s` → `as src.s -o src.o`（中間 `.s` は削除）
- `.S` → `as src.S -o src.o`（直接アセンブル）
- 現在のデフォルトは `OUTPUT = /kernel.elf`
- 現在のデフォルトは `BUILD = /kbuild`
- `/kbuild` は xv6fs 上の永続ディレクトリ。QEMU を終了しても `.o` が `rootfs.img` に残るため、次回の `make` は差分ビルドとして再開できる

```makefile
CC1FLAGS = \
    -quiet -nostdinc \
    -std=c11 -ffreestanding -fno-stack-protector -fno-stack-check \
    -fno-lto -fno-PIE -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
    -mno-red-zone -mcmodel=kernel -O2 \
    -DLWIP_NOASSERT \
    -I$(SRCDIR)/include \
    -I$(SRCDIR)/ports/lwip/src/include \
    -I/usr/include
```

PASS マーカー: `all` ターゲット完了時に `echo kernel-native-build: PASS` を出力。

### ステップ 2: `Makefile` にカーネルソース展開処理を追加 ✅

`$(ROOTFS_IMG)` ルール内（rootfs 再ビルド時）に以下を追加：

```bash
KBUILD=rootfs/src/kernel-build
rm -rf "$KBUILD"
cp -r kernel "$KBUILD/"
cp -r include "$KBUILD/"
cp -r ports/lwip/src/include "$KBUILD/ports/lwip/src/"
cp -r ports/lwip/src/core "$KBUILD/ports/lwip/src/"
mkdir -p "$KBUILD/ports/lwip/src/netif"
cp ports/lwip/src/netif/ethernet.c "$KBUILD/ports/lwip/src/netif/"
mkdir -p "$KBUILD/scripts"
cp scripts/kernel.ld "$KBUILD/scripts/"
cp scripts/Makefile.kernel-native "$KBUILD/Makefile"
```

展開サイズ: 約 5.3MB（kernel 600KB + include 188KB + lwip headers/core 4.5MB）

Makefile target も追加：
```makefile
nativekernelbuildsmoke: $(ISO)
    bash ./tests/native_kernel_build_smoke.sh $(ISO)

nativekernelbootsmoke: $(ISO)
    bash ./tests/native_kernel_boot_smoke.sh $(ISO)
```

### ステップ 3: `.gitignore` 更新 ✅

```
rootfs/src/kernel-build/
```
を追加（ホスト側でのソース展開ディレクトリをトラッキング対象から除外）

### ステップ 4: `tests/native_kernel_build_smoke.sh` 作成 ✅

テスト内容：
1. bootcmd を書き換えて ash スクリプトを実行
2. QEMU 起動（2G RAM、virtio-blk でのrootfs マウント）
3. ash 上で `make -f /src/kernel-build/Makefile BUILD=/kbuild OUTPUT=/kernel.elf` を実行
4. シリアルログから `kernel-native-build: PASS` マーカーを待機
5. タイムアウト: 3600 秒（フルビルド時を想定）

QEMU 引数（xv6fs rootfs を virtio-blk として渡す）：
```
-M q35 -cpu max -m 2G -cdrom ISO -no-reboot
-drive if=none,id=rootfs,file=rootfs.img,format=raw
-device virtio-blk-pci,drive=rootfs
```

---

## 実装の検証方法

```bash
# ISO をビルドしてスモークテストを実行
make nativekernelbuildsmoke
make nativekernelbootsmoke

# または手動
bash tests/native_kernel_build_smoke.sh orthos.iso
bash tests/native_kernel_boot_smoke.sh orthos.iso
```

成功時のシリアルログ（期待値）：
```
native-kernel-build-start
/bin/cc1 -quiet -nostdinc ... kernel/init.c -o /kbuild/kernel/init.s
/bin/as /kbuild/kernel/init.s -o /kbuild/kernel/init.o
...（全57ファイル）...
/bin/ld -nostdlib -static -T /src/kernel-build/scripts/kernel.ld ... -o /kernel.elf
kernel-native-build: PASS
native-kernel-build-end
```

---

## 既知の制約・注意点

| 項目 | 内容 |
|---|---|
| GCC バージョン | GCC 4.7.4。clang 拡張構文や C23 機能は使用不可 |
| `-Wall -Wextra` | native Makefile では省略（GCC 4.7.4 の警告差異によるノイズ回避） |
| lwip assertions | `-DLWIP_NOASSERT` で無効化。デバッグ目的の assert は動作しない |
| ビルド時間 | QEMU 上で 5〜15 分程度を想定（57 ファイル × cc1/as パイプライン） |
| 出力先 | `/kernel.elf`。xv6fs 上に保存される |
| オブジェクト格納先 | `/kbuild`。xv6fs 上に保存され、次回ビルドの `.o` キャッシュとして再利用する |
| rootfs 再生成注意 | `make orthos.iso` が `rootfs.img` を再生成すると `/kbuild/*.o` キャッシュを消す。キャッシュ保持時は `ROOTFS_REBUILD=0 make orthos.iso` または image 直接更新を使う |
| リンカスクリプト | `kernel.ld` の PHDRS / higher-half 設定はそのまま有効 |

### `/kbuild` cache 保持ルール（2026-05-06）

`/kbuild/*.o` は `rootfs.img` の中にあるため、既存 image を writable VirtIO block として使い続ける場合だけ保持される。ISO を作り直すこと自体は問題ではないが、ISO 作成の過程で `rootfs.img` を再生成すると cache は消える。

| 操作 | `/kbuild/*.o` | 理由 |
|---|---|---|
| `ROOTFS_REBUILD=0 make orthos.iso` | 保持 | 既存 `rootfs.img` を ISO に取り込むだけで image を再生成しない |
| `make persist-run` / `make persistsmprun` / `make persistnetrun` | 保持 | target 側で `ROOTFS_REBUILD=0` を指定している |
| `scripts/build_rootfs_xv6fs.py --replace /etc/bootcmd ... rootfs.img` | 保持 | 既存 file の割り当て済み領域だけを差し替える。inode/block を新規割り当てしない |
| `tests/native_kernel_boot_smoke.sh` | 保持 | `--replace` と `ROOTFS_REBUILD=0` を使い、bootcmd/script 差し替え時も image を再生成しない |
| default の `make rootfs.img` / `make orthos.iso` | 消失 | `ROOTFS_REBUILD=1` で host `rootfs/` tree から fresh image を作る |
| `rootfs.img` の削除、置換、clean rebuild | 消失 | guest が作った `/kbuild/*.o` は host `rootfs/` tree には戻らない |
| Limine module rootfs だけでの guest write | 永続化されない | module は起動時に読み込まれるだけで、host `rootfs.img` へ write back されない |

確認コマンド:

```bash
python3 scripts/build_rootfs_xv6fs.py --ls /kbuild rootfs.img
python3 scripts/build_rootfs_xv6fs.py --check rootfs.img
python3 scripts/build_rootfs_xv6fs.py --extract /etc/bootcmd /tmp/orthox-bootcmd rootfs.img
```

cache を意図的に捨てて clean な xv6fs image を作る場合だけ、default の `make rootfs.img` または `ROOTFS_REBUILD=1 make orthos.iso` を使う。

---

## テスト実行記録

### 2026-05-02 実行 #1 — FAIL（mkdir -p 問題）

- 状態: **FAIL**
- エラー:
  ```
  mkdir: can't create directory '/': Operation not permitted
  make: *** [Makefile:84: /tmp/kbuild/kernel/init.o] Error 1
  ```
- 原因: `@mkdir -p $(@D)` でbusybox の `mkdir -p` がパスを `/` から順にたどり、retrofs が `/` に対して `EPERM` を返すため失敗。
- 対応: `Makefile.kernel-native` を修正。`mkdir -p` を廃止し、`setup` フェーズで必要ディレクトリを plain `mkdir` で事前作成する方式に変更。

### 2026-05-02 実行 #2 — FAIL（/bin/sh が for ループ未対応）

- 状態: **FAIL**
- エラー:
  ```
  Exec: File not found: /bin/for
  for: Operation not permitted
  make: *** [Makefile:87: setup] Error 1
  ```
- 原因: make がレシピ実行に使う `/bin/sh` が `user/sh.elf`（カスタム最小シェル）であり、`for` をビルトインとして持たず外部コマンドとして exec しようとする。
- 対応: `Makefile.kernel-native` に `SHELL = /bin/ash` を追加し、busybox ash を使用するよう変更。

### 2026-05-02 実行 #3 — FAIL（retrofs が実行時 mkdir を拒否）

- 状態: **FAIL**
- エラー:
  ```
  mkdir: can't create directory '/tmp/kbuild': Operation not permitted
  make: *** [Makefile:91: setup] Error 1
  ```
- 原因: `sys_mkdir` の実装が `normalize_fs_path` 後のパスに `/` が含まれると即 `return -1`。  
  `/tmp/kbuild` の正規化結果 `tmp/kbuild` に `/` が含まれるため、**トップレベル以外のディレクトリはランタイムで作れない**。  
  ※ これは retrofs の既知の制約（後日調査予定）。
- 対応:
  - ホスト側 rootfs ビルド時（`$(ROOTFS_IMG)` ルール内）に `mkdir -p rootfs/tmp/kbuild/kernel rootfs/tmp/kbuild/lwip/core/ipv4 rootfs/tmp/kbuild/lwip/netif` を追加
  - `Makefile.kernel-native` の `setup` ターゲットを削除（dirs は image に事前収録）
  - `.gitignore` に `rootfs/tmp/kbuild/` を追加

### 2026-05-02 実行 #4 — PASS（ネイティブビルド確認）

- 状態: **PASS**
- 修正: kbuild ディレクトリをホスト側で事前作成し retrofs image に収録
- 結果: `kernel-native-build: PASS` 確認。`/tmp/kernel.elf` 生成成功。
- 備考: この時点ではビルド結果の取り出し・ブート確認は未実装

### 2026-05-03 実行 #5 — Phase 2 FAIL（kout img サイズ不足）

- 状態: **Phase 1 PASS / Phase 2 FAIL**
- 背景: RetroFS への書き込みが silently 失敗（Run #3 で確認）したため、第2 virtio-blk デバイス（kout）方式に全面移行した。
  - `/dev/kout` → `FT_RAWDEV` → `virtio_kout_write_raw()` を実装
  - `kernel-output.img`（4MB）をホスト側に第2ドライブとして渡す構成を追加
  - Phase 2 で Python による ELF 抽出 → boot ISO 生成 → QEMU ブート確認を追加
- エラー:
  ```
  struct.error: unpack_from requires a buffer of at least 6339080 bytes
  for unpacking 4 bytes at offset 6339076 (actual buffer size is 4194304)
  ```
- 原因: ゲスト内で GCC ビルドした `kernel.elf` は約 6.3MB（デバッグシンボル込み）だが、`kernel-output.img` が 4MB しかなかった。
- 対応: `dd bs=1M count=16`（16MB）に拡大。

### 2026-05-03 実行 #6 — **全体 PASS** ✅

- 状態: **PASS（Phase 1 + Phase 2 両方）**
- 修正: `kernel-output.img` を 16MB に拡大
- Phase 1 結果:
  ```
  native-kernel-build-start
  （全ファイルコンパイル・リンク）
  cp /tmp/kernel.elf /dev/kout
  native-kernel-saved
  Phase 1 passed: native kernel saved to RetroFS.
  ```
- Phase 2 結果:
  ```
  Extracted ELF: 6339648 bytes -> native-kernel.elf
  Built native-kernel-boot.iso (294M)
  Test passed: native kernel boots successfully on Orthox-64.
  ```
- ネイティブビルド済みカーネルサイズ: **6,339,648 bytes**

### 2026-05-03 実際の起動確認 — **金字塔達成** 🏆

テスト通過後、`native-kernel.elf` を実際に QEMU で起動した。

```
--- Orthox-64 v0.3.0 Boot ---
[pci] virtio-blk found: vendor=0x1AF4 device=0x1001
[vblk] virtio-blk ready: 0x117 MiB
[boot] registered virtio-blk as vblk0
[boot] mounted RetroFS root image on vblk0
SMP CPUs detected: 1
Starting first user task...
Welcome to Orthox-64 Shell!
muslcheck: PASS
# bootcmd: /bin/ash

BusyBox v1.27.0.git built-in shell (ash)
#
```

Orthox-64 が自分自身のカーネルをコンパイルし、そのカーネルで起動した。  
セルフホスティングカーネルビルドという目標が、Day 43 に完全に達成された。

### 2026-05-05 xv6fs 移行後の再検証 — 実行中

- 背景:
  - root filesystem は RetroFS から xv6fs へ移行済み。
  - `scripts/Makefile.kernel-native` の `BUILD` / `OUTPUT` デフォルトは `/kbuild` / `/kernel.elf` へ変更済み。
  - `/kbuild/*.o` は xv6fs 上に永続化され、失敗後や次回起動後のインクリメンタル再開に使う。
- テスト設計:
  - `tests/native_kernel_boot_smoke.sh` は `rootfs.img` を丸ごと再生成しない。
  - `scripts/build_rootfs_xv6fs.py --replace` で既存 image 内の `/etc/bootcmd` と `/etc/native_kernel_build_smoke.sh` だけを差し替える。
  - Phase 1 後は `scripts/build_rootfs_xv6fs.py --extract /kernel.elf ... rootfs.img` で xv6fs から ELF を取り出す。
  - Phase 2 は取り出した native kernel を使って ISO を組み、起動確認する。
- 現在の実行状況:
  - `native-kernel-build-start` は確認済み。
  - `/kbuild/kernel/*.o` への出力が進行中。
  - 最終 PASS/FAIL は smoke 完了後に追記する。

---

## 今後の課題

- [x] 実際のスモークテスト実行による動作確認 → Run #6 で達成
- [ ] xv6fs 移行後の `nativekernelbootsmoke` 完走結果を追記
- [ ] ビルド成功後に `readelf -h /kernel.elf` 相当の ELF 検証を追加
- [ ] `-DLWIP_NOASSERT` 相当の設定をホスト Makefile にも追加するか検討
- [ ] ビルド時間の計測・最適化（並列 make の可否確認）
- [ ] cc1 が `/usr/local/lib/gcc/x86_64-elf/4.7.4/include/` を探す場合の対処（現状は musl の `/usr/include` で代替）
- [ ] ネイティブビルドカーネルのサイズ削減（`-g` 除去 / strip）の検討
- [ ] `make finalsmokesuite` への `nativekernelbootsmoke` 組み込み

---

## 変更ファイル一覧

| ファイル | 変更種別 | 内容 |
|---|---|---|
| `scripts/Makefile.kernel-native` | 新規作成 | Orthox-64 ネイティブ用カーネルビルド Makefile |
| `Makefile` | 変更 | rootfs 生成時にカーネルソースを展開する処理・`nativekernelbuildsmoke` / `nativekernelbootsmoke` target を追加 |
| `.gitignore` | 変更 | `rootfs/src/kernel-build/`、`kernel-output.img` 等を追加 |
| `tests/native_kernel_build_smoke.sh` | 新規作成 | ネイティブカーネルビルド検証スモークテスト（Phase 1 のみ） |
| `tests/native_kernel_boot_smoke.sh` | 新規作成 / 更新 | Phase 1（xv6fs 上の `/kbuild` + `/kernel.elf` へビルド）+ Phase 2（xv6fs から抽出 + ブート確認）の2フェーズテスト |
| `scripts/build_rootfs_xv6fs.py` | 変更 | `--extract` に加え、既存 image 内の小ファイルをキャッシュ破壊なしで差し替える `--replace` を追加 |
| `include/pci.h` | 変更 | `pci_find_virtio_blk_n(int n, ...)` 追加 |
| `kernel/pci.c` | 変更 | virtio-blk 検出を最大2台に拡張 |
| `include/virtio_blk.h` | 変更 | `virtio_kout_init()` / `virtio_kout_write_raw()` 宣言追加 |
| `kernel/virtio_blk.c` | 変更 | kout デバイス（第2 virtio-blk、ポーリング書き込み）実装追加 |
| `include/fs.h` | 変更 | `FT_RAWDEV` を `file_type_t` に追加 |
| `kernel/fs.c` | 変更 | `/dev/kout` open / `FT_RAWDEV` write サポート追加 |
| `rootfs/src/kernel-build/kernel/fs.c` | 変更 | kernel/fs.c と同じ変更（ゲスト内ビルド用） |
| `kernel/init.c` | 変更 | `virtio_kout_init()` 呼び出しを追加 |
