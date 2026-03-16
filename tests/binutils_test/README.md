# Binutils 実用テスト (Day 8 - 2026-03-07)

このディレクトリには、orthOS-64 上で移植された `as` (アセンブラ) と `ld` (リンカ) の動作を検証するために作成されたスクリプト類が格納されています。

## ファイル構成

- **`run_test.sh`**: 
  QEMU をバックグラウンドで起動し、Python スクリプト (`send_string.py`) を介してシェルコマンドを自動入力するメインテストスクリプトです。以下の手順を自動実行します。
  1. `as test.s -o test.o` (アセンブル)
  2. `ld test.o -o test` (リンク)
  3. `./test` (生成バイナリの実行)
- **`send_string.py`**: 
  QEMU のモニタソケット (`qemu.sock`) を利用して、任意の文字列をキー入力としてゲスト OS に送信するユーティリティです。
- **`debug_keys.py`**: 
  QEMU とゲスト OS 間のキーマッピングを調査するためのデバッグ用スクリプトです。
- **`test.s`**: 
  テストに使用したアセンブリソースのバックアップです。実体は `rootfs/test.s` にあり、ISO の `rootfs.tar` に含まれています。

## テストの実行方法

事前に `make orthos.iso` で ISO をビルドした後、このディレクトリ内で `run_test.sh` を実行します。
シリアル出力 (`serial.log`) に "Hello from orthOS!" と表示されれば成功です。

```bash
# プロジェクトルートから実行する場合
bash tests/binutils_test/run_test.sh
```

## 本テストによる主な改善・修正点

- **キーボードドライバの修正**: `kernel/keyboard.c` の `keymap` のズレ（1文字ずれていたバグ）を修正。
- **システムコール互換性の向上**: Newlib と Linux (orthOS) カーネル間での `O_CREAT` 等のフラグの不一致を `user/syscalls.c` で吸収。
- **スタックサイズの拡張**: ユーザースタックを 4KB から 1MB へ拡大（Binutils 実行に必須）。
- **VFS ロードの実現**: `task_execve` を拡張し、Ramfs/TAR 内の ELF バイナリを透過的にロード可能に修正。
