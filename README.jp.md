# Orthox-64

![Orthox-64 デスクトップ](assets/screenshot.png)
![Orthox-64 DOOM](assets/doom.png)

Orthox-64（オーソックス・シックスティフォー）は、現代的なOS自作の在り方を提示するプロジェクトである。

## コンセプトと設計哲学
- **名前の由来:** 「Ortho-」は直交性と正しさに由来し、「-x」は Unix の伝統に由来する。Orthox-64 は、正統派で最小主義の Unix 系 OS を目指す。
- **軽量・堅牢 (Lightweight & Robust):** 不要なシステム複雑性を避け、小さく安定したカーネルとユーザーランド基盤に集中する。
- **実用本位の Unix 互換性:** フル POSIX 準拠を目標にするのではなく、BusyBox、GCC、および関連ツールチェーンを動かすために必要な最小限のカーネル機能と libc 表面を実装する。
- **再発明より統合:** すべての部品をゼロから再実装することを価値とはみなさない。既存の高品質なオープンソース資産を統合し、移植すること自体を主要なエンジニアリング課題として位置づける。

## 特徴
- **64ビット ロングモード:** 完全な 64ビット モードで動作。
- **ブートローダー:** モダンな UEFI/BIOS ブートのために [Limine](https://github.com/limine-bootloader/limine) を採用。
- **メモリ管理:** PMM (物理メモリマネージャ) およびページングによる VMM (仮想メモリマネージャ)。
- **マルチタスク:** プリエンプティブ・マルチタスク、カーネルスレッド、および SMP 対応の per-CPU scheduler 基盤。
- **ファイルシステム:** 仮想ファイルシステム (VFS) と tar 形式の初期ラムディスク。
- **USB サポート:** 基本的な USB スタックとマスストレージクラス (MSC) のサポート。
- **ネットワーク:** `virtio-net` と `lwIP` に基づく IPv4 ネットワーク機能を実装済み。DHCP、DNS、ICMP、UDP、TCP、socket syscall、BusyBox `httpd`、外向き HTTP クライアント、さらに BearSSL ベースの userspace HTTPS クライアントまで到達。
- **SMP:** 4 CPU 起動、LAPIC timer、resched IPI、per-CPU run queue、および pipe / wait / socket の blocking wakeup 経路を実機確認済み。
- **サウンド:** Intel HD Audio によるオーディオサポート。
- **ユーザーランド:** 標準互換性のための `musl libc` をベースとした環境。
- **移植アプリ:** `doomgeneric` などの既存ソフトウェアのポーティング。

## 移植済みユーザーランドコンポーネント
- **musl libc:** `1.2.5`
- **BusyBox (`ash` および基本 applet 群):** `1.27.0.git`
- **GNU Binutils:** `2.26`
- **GCC:** `4.7.4`
- **doomgeneric:** upstream の版番号がツリー内に記録されていない vendored local port

## ステータス
プロジェクトは現在活発に開発中です。現在の SMP scheduler フェーズは受け入れ条件まで到達しており、Orthox-64 は QEMU 上で 4 CPU 起動、per-CPU run queue、fork spread 復帰、pipe / `wait4()` / DNS / socket / UDP echo / BusyBox `httpd` / userspace HTTPS の SMP 実経路確認まで完了しています。今後は、この安定化した SMP 基盤の上で、より広い load balancing とユーザーランド互換性の拡充を進める段階です。

## 謝辞
Orthox-64 は、以下のプロジェクトからインスピレーションを受け、実装の参考にしています。
- **[MikanOS](https://github.com/uchan-nos/mikanos)**: [uchan-nos](https://github.com/uchan-nos) 氏による現代的な自作OS。カーネルアーキテクチャや一部のプリミティブなセットアップにおいて、その優れた実装を参考に開発されています。
- **[Limine](https://github.com/limine-bootloader/limine)**: モダンなUEFI/BIOSブートをサポートするためのブートローダーとして採用しています。
