# Orthox-64 インストールガイド

## 必要条件
ビルドには以下のツールが必要です：
- **ホストOS:** Linux または macOS (WSL2推奨)
- **コンパイラ:** x86_64-elf-gcc, x86_64-elf-binutils
- **ビルドツール:** make, python3
- **イメージ作成:** xorriso, mtools
- **エミュレータ:** QEMU (x86_64)

## ビルド手順

1. **リポジトリのクローン**
   ```bash
   git clone https://github.com/yourusername/Orthox-64.git
   cd Orthox-64
   ```

2. **カーネルとユーザーランドのビルド**
   ```bash
   make
   ```
   これにより、カーネル (`kernel/kernel.elf`) とユーザーランドバイナリがビルドされ、`rootfs.tar` が作成されます。

3. **ブートイメージ (ISO) の作成**
   ```bash
   make iso
   ```
   Limine ブートローダーを含むブート可能な ISO イメージが作成されます。

## 実行方法

QEMU を使用して実行するには、以下のスクリプトが利用可能です：
```bash
./run_qemu.sh
```
（DOOM を実行する場合は `./run_doom_qemu.sh` を使用してください）

## ツールチェインの構築
独自のツールチェインをビルドする必要がある場合は、`ports/` ディレクトリ内のスクリプトを使用してください：
```bash
cd ports
./build_gcc.sh
./build_binutils.sh
```
