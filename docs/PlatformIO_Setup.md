# PlatformIO セットアップ手順 (M5PaperS3 向け)

macOS を想定。VSCode + PlatformIO IDE を使います。CLI だけでも完結可能。

---

## 1. 前提ソフトのインストール

### 1-1. VSCode
[https://code.visualstudio.com/](https://code.visualstudio.com/) からダウンロードしてインストール。

### 1-2. Python (PlatformIO Core が必要)
macOS には標準で入っているが、なければ Homebrew で:

```bash
brew install python
```

### 1-3. USB-Serial ドライバ
M5PaperS3 は **USB-CDC (ネイティブUSB)** で認識されるので、通常は追加ドライバ不要。
もし認識しない場合は CH9102/CP210x ドライバをインストール:

- [CH9102 Driver (M5Stack公式)](https://docs.m5stack.com/en/download)

---

## 2. PlatformIO IDE 拡張を入れる

VSCode を起動 → 拡張機能タブ (`⇧⌘X`) → `PlatformIO IDE` を検索してインストール。
初回起動時に PlatformIO Core が自動でダウンロードされる (数分待つ)。

インストール完了後、左サイドバーに蟻のアイコン 🐜 が出れば OK。

### (任意) CLI だけで使う場合

```bash
python3 -m pip install --user platformio
# PATH を通す
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
pio --version
```

---

## 3. プロジェクトを開く

本プロジェクト (`m5paperS3/`) はすでに `platformio.ini` を持っているので、そのまま開けばOK。

### VSCode から

1. `File → Open Folder...` で `m5paperS3` フォルダを開く
2. PlatformIO が自動で依存ライブラリ (M5Unified / M5GFX) を取得する
3. ステータスバー下部に PlatformIO のツールバー (✓ / → / 🔌 など) が表示される

### CLI から

```bash
cd /Users/unyo/Desktop/cloude_project/m5paperS3
pio pkg install      # 依存ライブラリを取得
```

---

## 4. `platformio.ini` の要点

```ini
[env:m5stack-papers3]
platform  = espressif32
board     = m5stack-papers3       ; M5PaperS3 の公式ボード定義
framework = arduino

monitor_speed = 115200
upload_speed  = 1500000

board_build.arduino.memory_type = qio_opi   ; 8MB PSRAM 有効化
board_build.partitions          = default_8MB.csv
board_upload.flash_size         = 16MB

build_flags =
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_CDC_ON_BOOT=1    ; USB-CDC でシリアル

lib_deps =
    m5stack/M5Unified @ ^0.2.2
    m5stack/M5GFX     @ ^0.2.4
```

| キー | 役割 |
|---|---|
| `board = m5stack-papers3` | M5PaperS3 用のボード設定 (ピン配置・EPDパネル設定済み) |
| `memory_type = qio_opi` | 8MB PSRAM をオクタルモードで有効化 |
| `flash_size = 16MB` | 16MB フラッシュを全域利用 |
| `ARDUINO_USB_CDC_ON_BOOT=1` | 起動時に USB-CDC をシリアルとして有効化 |
| `lib_deps` | バージョン固定でライブラリ取得 (`^` はマイナー更新まで許容) |

---

## 5. ビルド / 書き込み / モニタ

### VSCode

画面下部 PlatformIO ツールバー:

| アイコン | 役割 | ショートカット |
|---|---|---|
| ✓ | Build | `⌘⌥B` |
| → | Upload | `⌘⌥U` |
| 🔌 | Serial Monitor | `⌘⌥S` |
| 🗑 | Clean | - |

### CLI

```bash
# ビルド
pio run

# 書き込み (端末を USB で接続してから)
pio run -t upload

# シリアルモニタ (115200bps)
pio device monitor

# ビルド+書き込み+モニタを一気に
pio run -t upload -t monitor
```

---

## 6. 書き込みで失敗するときのチェックリスト

1. **ポートが見えない**
   - `pio device list` でポート一覧を確認
   - USB ケーブルが「充電専用」でないか疑う (データ線ありのものに交換)
2. **Permission denied / port busy**
   - VSCode のシリアルモニタが開いたままになっていないか
3. **ダウンロードモードに入らない**
   - M5PaperS3 の **リセットボタンを押しながら USB接続** → すぐにリセットを離す
   - それでもダメなら `upload_speed = 921600` に下げる
4. **`A fatal error occurred: Failed to connect`**
   - `build_flags` に `-DARDUINO_USB_MODE=1` を追加で改善することあり

---

## 7. よく使うコマンド集

```bash
pio pkg update           # ライブラリを更新
pio pkg outdated         # 古いライブラリを確認
pio run -t clean         # ビルド成果物を削除
pio run -t erase         # フラッシュ全消去 (工場出荷状態)
pio boards m5stack       # 利用可能な M5Stack 系ボード一覧
pio device list          # 認識されているシリアルポート一覧
```

---

## 8. 参考リンク

- M5PaperS3 製品ページ: https://docs.m5stack.com/en/core/M5PaperS3
- PlatformIO Docs: https://docs.platformio.org/
- M5Unified: https://github.com/m5stack/M5Unified
- M5GFX: https://github.com/m5stack/M5GFX
