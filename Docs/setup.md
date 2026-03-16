# orthOS-64 プロジェクト引き継ぎドキュメント (macOS -> WSL2)

## 1. プロジェクト概要
- **名称**: orthOS-64
- **コンセプト**: Unix V6 の精神を継承したシンプルで本質的な 64bit OS。
- **ブートローダー**: Limine v7.x (Protocol version 3+)
- **開発環境**: macOS (iTerm2) -> WSL2 (Ubuntu 24.04)

## 2. 現在のディレクトリ構成と主要ファイル
```text
orthOS-64/
├── kernel/main.c      # カーネルエントリーポイント (_start)
├── include/limine.h   # Limine プロトコルヘッダ (v7.x-binary 準拠)
├── scripts/kernel.ld  # リンカスクリプト (2MB アラインメント済)
├── iso/limine.conf    # Limine 設定ファイル (path: /boot/kernel.elf)
├── limine/            # Limine バイナリ (v7.x-branch-binary)
└── Makefile           # 修正済み。kernel.elf を出力対象とする
```

## 3. 開発における重大な教訓 (Caution)
macOS 開発中に発生した以下の問題は、WSL2 でも再発の恐れがあるため厳守すること：

- **ディレクトリ削除の禁止**: `rm -rf` は極めて危険。特にソースコードを含む `kernel/` ディレクトリを巻き込むリスクを排除するため、`Makefile` の `clean` ターゲットには `rm -f` を使用し、対象ファイルを個別に指定する。
- **バイナリ名の衝突**: 出力名を `kernel` にすると、`kernel/` ディレクトリと衝突し `make clean` 時やパス解決でエラーになる。必ず **`kernel.elf`** という拡張子付きの名前を使用する。
- **ソースコードの復元**: 万が一削除された場合は、本チャットのログから `main.c` の最新（v7.x 準拠、マーカーなし版）を復元すること。

## 4. 現在の問題点と分析 (WSL2 での解決目標)

### (1) `[config file not found]` エラーの真意
- **分析**: Limine v7.x において、このエラーは「設定ファイルがない」場合だけでなく、**「設定ファイル内で指定したパス（/boot/kernel.elf）にファイルが見つからない」** 場合にも表示される。
- **画像証拠**: 右上の `Blank Entry` は設定ファイルの読み込みには成功している証拠。つまり、課題は「ファイルシステム上でのカーネル発見」にある。

### (2) QEMU のマシンタイプ
- **現状**: `-M q35` (SATA) を使用。BIOS ブート時の互換性を高めるため、WSL2 では **`-M pc` (IDE)** への変更を検討する。

## 5. WSL2 (Ubuntu 24.04) セットアップ

### 必須パッケージ
```bash
sudo apt update
sudo apt install -y build-essential clang lld llvm nasm xorriso qemu-system-x86 mtools git
```

### 移行後の Makefile 調整
- **QEMU ディスプレイ**: `-display cocoa` を削除。
- **コンソール出力**: 確実にログを見るため `-nographic -serial stdio` をデフォルトとする。

---
**マイルストーン**: WSL2 で `make run` を実行し、Limine メニューから "orthOS-64" を選択。シリアルターミナルに "Hello, orthOS-64 World!" と表示されること。
