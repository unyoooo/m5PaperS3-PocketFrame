// M5PaperS3-PocketFrame
//
// 状態:
//   VIEW  : 画像表示のみ。タップで MENU へ
//   MENU  : 画像の上にメニューバー [←][→][●][画像一覧] を表示
//            ←次画像(戻る) / →次画像(進む) / ●メニュー閉じる / 画像一覧→LIST
//   LIST  : フォルダ内容の一覧 (フォルダ/画像)。タップで選択、フリックでページ送り
//
// 省電力:
//   最後のタッチから 60 秒無操作で deep sleep。直前に右上 "SLEEP"。

// NOTE: SD.h は M5Unified.h より先にインクルード
#include <SPI.h>
#include <SD.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_sleep.h>
#include <esp32-hal-cpu.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>
#include <string>

// ----- SD pins -----
static constexpr int SD_SCK  = 39;
static constexpr int SD_MISO = 40;
static constexpr int SD_MOSI = 38;
static constexpr int SD_CS   = 47;
static SPIClass SDSPI(HSPI);

// ----- 設定 -----
static constexpr const char* ROOT_DIR   = "/gallery";
static constexpr uint32_t IDLE_TIMEOUT_MS = 60UL * 1000UL;
static constexpr int LIST_PER_PAGE      = 8;

// ----- 型 -----
enum ImgKind { IMG_NONE, IMG_JPG, IMG_PNG };
enum AppState {
    STATE_VIEW,
    STATE_MENU,
    STATE_LIST,
    STATE_SS_PICK_FOLDER,   // LIST 同等だがフッタで「ここで再生」
    STATE_SS_INTERVAL,      // 間隔選択画面
    STATE_SLIDESHOW,
    STATE_COMM,             // Wi-Fi AP + Web サーバ (SLEEP 一時停止)
    STATE_SETTINGS          // 設定画面
};

// ----- 多言語 -----
enum Lang { LANG_JA = 0, LANG_EN = 1 };
RTC_DATA_ATTR static uint8_t s_lang = LANG_JA;

struct Strings {
    const char* menu_list;
    const char* menu_slide;
    const char* menu_comm;
    const char* menu_cfg;
    const char* no_image;
    const char* battery;
    const char* battery_na;
    const char* msg_no_sd;
    const char* msg_prev_fail;
    const char* msg_no_images;
    const char* msg_no_images_folder;
    const char* list_close;
    const char* list_play_folder;
    const char* picker_title;
    const char* picker_ls_desc;
    const char* picker_ds_desc;
    const char* picker_start;
    const char* picker_cancel;
    const char* picker_hour;
    const char* picker_min;
    const char* picker_sec;
    const char* comm_title;
    const char* comm_desc1;
    const char* comm_desc2;
    const char* comm_wifi_step;
    const char* comm_browser_step;
    const char* comm_warn1;
    const char* comm_warn2;
    const char* comm_exit;
    const char* settings_title;
    const char* settings_lang;
    const char* settings_back;
    const char* msg_header;
};

static const Strings STR_JA = {
    "画像リスト", "スライド", "通信", "設定",
    "(画像なし)",
    "Battery: %d%%", "Battery: N/A",
    "SDカードがありません。",
    "前回の画像にアクセスできませんでした。",
    "SDカードに画像がありません。",
    "このフォルダに画像がありません。",
    "%d / %d  close",
    "%d/%d  このフォルダを再生",
    "スライドショー間隔  (%s)",
    "60秒未満 → Light Sleep (タップで応答・省電力40倍)",
    "60秒以上 → Deep Sleep (電源ボタンで応答・省電力2000倍)",
    "開始", "キャンセル",
    "時間", "分", "秒",
    "通信モード",
    "このモードでは Wi-Fi 接続でスマホや PC からブラウザで",
    "画像をアップロードしたりフォルダの整理を行うことができます。",
    "① Wi-Fi に接続",
    "② ブラウザで開く",
    "※ この画面では SLEEP になりません。",
    "  使用後は右の「終了」ボタンを押してください。",
    "終了",
    "設定", "言語 / Language", "戻る",
    "MESSAGE"
};
static const Strings STR_EN = {
    "List", "Slide", "Comm", "Cfg",
    "(no image)",
    "Battery: %d%%", "Battery: N/A",
    "SD card not found.",
    "Previous image is inaccessible.",
    "No images found on SD card.",
    "No images in folder",
    "%d / %d  close",
    "%d/%d  Play this folder",
    "Slideshow Interval  (%s)",
    "Under 60s: Light Sleep (tap to respond, 40x saving)",
    "60s or more: Deep Sleep (power btn to respond, 2000x saving)",
    "Start", "Cancel",
    "Hour", "Min", "Sec",
    "Comm Mode",
    "In this mode you can use a smartphone or PC browser via Wi-Fi",
    "to upload images and manage folders.",
    "1. Connect Wi-Fi",
    "2. Open in browser",
    "* SLEEP is disabled on this screen.",
    "  Press Exit when done.",
    "Exit",
    "Settings", "Language", "Back",
    "MESSAGE"
};

static const Strings& S() { return (s_lang == LANG_EN) ? STR_EN : STR_JA; }

struct Item {
    std::string name;   // basename
    bool is_dir;
    ImgKind kind;       // IMG_NONE if is_dir
};

struct Rect { int x, y, w, h; };

// ----- RTC メモリ (deep sleep 間で保持) -----
RTC_DATA_ATTR static char s_saved_dir[160] = "";
RTC_DATA_ATTR static char s_saved_file[96] = "";
RTC_DATA_ATTR static uint32_t s_boot_count = 0;
// 長間隔 (≥60s) の deep-sleep スライドショー継続用
RTC_DATA_ATTR static bool     s_rtc_ss_active      = false;
RTC_DATA_ATTR static uint32_t s_rtc_ss_interval_ms = 0;
RTC_DATA_ATTR static char     s_rtc_ss_dir[160]    = {0};

// ----- CPU 周波数 (案 C: 省電力) -----
static constexpr uint32_t CPU_FREQ_LOW  = 80;    // 通常待機
static constexpr uint32_t CPU_FREQ_HIGH = 240;   // デコード/Wi-Fi
static inline void cpuLow()  { setCpuFrequencyMhz(CPU_FREQ_LOW);  }
static inline void cpuHigh() { setCpuFrequencyMhz(CPU_FREQ_HIGH); }

// ----- スライドショー間隔 -----
// 60 秒以上は deep sleep モード
static constexpr uint32_t DEEP_SLEEP_THRESHOLD_MS = 60000UL;
// 桁インデックス: 0=H10 1=H1 2=M10 3=M1 4=S10 5=S1
static constexpr int PICKER_DIGITS = 6;
static int s_pd[PICKER_DIGITS] = { 0, 0, 0, 0, 1, 0 }; // 既定 10 秒

// ----- 揮発状態 -----
static std::string s_current_dir = ROOT_DIR;
static std::vector<Item> s_items;           // s_current_dir の中身 (フォルダ+画像)
static std::vector<size_t> s_image_idx;     // s_items のうち画像のみの index
static size_t s_pos = 0;                    // s_image_idx 上の位置 (VIEW用)
static AppState s_state = STATE_VIEW;
static uint32_t s_last_activity = 0;

// LIST state
static std::string s_view_dir;     // LIST に入る前の VIEW 用ディレクトリ (close 時復帰用)
static size_t s_view_pos = 0;      // LIST に入る前の s_pos
static int s_list_page = 0;
static int16_t s_touch_start_x = 0;
static int16_t s_touch_start_y = 0;
static uint32_t s_touch_start_ms = 0;

// Slideshow state
static uint32_t s_ss_interval_ms = 5000;
static uint32_t s_ss_last_change = 0;
static std::string s_ss_dir;

// ----- 前方宣言 -----
static bool initSD();
static void scanDir(const char* dir);
static bool getJpgSize(const char* path, int& w, int& h);
static bool getPngSize(const char* path, int& w, int& h);
static void drawFullScreenImg(ImgKind kind, const char* path);
static void showCurrentImage();
static void showMenuOverlay();
static void hideMenuOverlay();
static void showList();
static void showIntervalPicker();
static void redrawPickerDigit(int idx);
static void pickerInc(int idx);
static void pickerDec(int idx);
static uint32_t pickerTotalSec();
static Rect pickerUpRect(int idx);
static Rect pickerDnRect(int idx);
static Rect pickerOkRect();
static Rect pickerCancelRect();
static void startSlideshow(const std::string& dir, uint32_t interval_ms);
static void stopSlideshow();
static void enterSlideshowDeepSleep(uint32_t interval_ms);
static void handleSlideshowResume();
static int getBatteryLevel();
static void startCommMode();
static void stopCommMode();
static void drawCommScreen(const char* ssid, const char* pass, const char* url);
static void drawError(const char* msg);
static void goToDeepSleep();
static void savePosition();
static bool findFirstImage(const char* dir, int depth, std::string& outDir, std::string& outFile);
static void saveLang(uint8_t lang);
static void showSettings();
static Rect settingsLangJaRect();
static Rect settingsLangEnRect();
static Rect settingsBackRect();
static std::string joinPath(const std::string& a, const std::string& b);
static std::string parentOf(const std::string& p);

// ============================================================
// メニューボタンの配置 (画面下部の帯)
// ============================================================
static Rect menuBarRect() {
    int w = M5.Display.width();
    int h = M5.Display.height();
    const int bar_h = 80;
    return { 0, h - bar_h, w, bar_h };
}

// 6 ボタン: [<] [>] [画像リスト] [スライド] [通信] [設定]
static constexpr int MENU_BUTTON_COUNT = 6;
static Rect menuButton(int idx) {
    Rect bar = menuBarRect();
    int bw = bar.w / MENU_BUTTON_COUNT;
    return { bar.x + idx * bw + 6, bar.y + 6, bw - 12, bar.h - 12 };
}

// 上部の電池表示バー (高さ 50px、画面上部に被せる)
static Rect batteryBarRect() {
    return { 0, 0, M5.Display.width(), 50 };
}

// 通信モードの「終了」ボタン (画面右下)
static Rect commExitButtonRect() {
    int W = M5.Display.width();
    int H = M5.Display.height();
    const int bw = 180, bh = 64;
    return { W - bw - 20, H - bh - 16, bw, bh };
}

static bool inRect(const Rect& r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// ============================================================
void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);

    Serial.begin(115200);
    delay(800);
    ++s_boot_count;
    Serial.printf("\n======== BOOT %lu ========\n", (unsigned long)s_boot_count);

    M5.Display.setRotation(1);
    M5.Display.setEpdMode(epd_mode_t::epd_quality);

    // 案 C: デフォルトは 80MHz、必要時に 240MHz へ昇圧
    cpuLow();

    // --- 言語設定を NVS から読み込み ---
    {
        Preferences prefs;
        prefs.begin("viewer", true);   // read-only
        s_lang = prefs.getUChar("lang", LANG_JA);
        prefs.end();
        if (s_lang > LANG_EN) s_lang = LANG_JA;
        Serial.printf("Language: %s\n", s_lang == LANG_EN ? "EN" : "JA");
    }

    // --- SD カードのマウント ---
    if (!initSD()) {
        drawError(S().msg_no_sd);
        delay(3000);                 // 画面を読む時間を確保
        goToDeepSleep();
    }

    // --- スライドショー (deep sleep) 継続判定 ---
    if (s_rtc_ss_active) {
        esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
        Serial.printf("[SS] active, wake cause=%d\n", (int)cause);
        if (cause == ESP_SLEEP_WAKEUP_TIMER) {
            // タイマで起きた → 次画像へ進めて再 deep sleep
            handleSlideshowResume();
            // 通常は戻らない (戻った場合はフォルダ空など)
        } else {
            // ボタン等で起きた → スライドショーを終了
            Serial.println("[SS] stopped by button");
            s_rtc_ss_active = false;
            // 通常復元 (s_saved_file/dir に最後に表示した画像が入っている)
        }
    }

    // --- 前回ファイル位置の復元を試みる ---
    // RTC に保存がなければ NVS から読み込む (電源 OFF 復帰)
    if (s_saved_file[0] == 0) {
        Preferences prefs;
        prefs.begin("viewer", true);
        String nDir  = prefs.getString("dir",  "");
        String nFile = prefs.getString("file", "");
        prefs.end();
        if (nDir.length() > 0) {
            strncpy(s_saved_dir, nDir.c_str(), sizeof(s_saved_dir) - 1);
            s_saved_dir[sizeof(s_saved_dir) - 1] = 0;
        }
        if (nFile.length() > 0) {
            strncpy(s_saved_file, nFile.c_str(), sizeof(s_saved_file) - 1);
            s_saved_file[sizeof(s_saved_file) - 1] = 0;
        }
        Serial.printf("[NVS] dir=%s file=%s\n", s_saved_dir, s_saved_file);
    }

    const bool had_saved = (s_saved_file[0] != 0);
    bool restored = false;
    s_current_dir = s_saved_dir[0] ? s_saved_dir : "/";

    if (had_saved) {
        scanDir(s_current_dir.c_str());
        for (size_t i = 0; i < s_image_idx.size(); ++i) {
            if (s_items[s_image_idx[i]].name == s_saved_file) {
                // 実際にファイルが開けるかを確認
                std::string p = joinPath(s_current_dir, s_items[s_image_idx[i]].name);
                File tf = SD.open(p.c_str(), FILE_READ);
                if (tf) {
                    tf.close();
                    s_pos = i;
                    restored = true;
                }
                break;
            }
        }
        if (!restored) {
            drawError(S().msg_prev_fail);
            delay(2000);
        }
    }

    // --- フォールバック: ルートから再帰的に最初の画像を探す ---
    if (!restored) {
        std::string foundDir, foundFile;
        if (findFirstImage("/", 0, foundDir, foundFile)) {
            s_current_dir = foundDir;
            scanDir(s_current_dir.c_str());
            s_pos = 0;
            // foundFile に一致する画像を探す
            for (size_t i = 0; i < s_image_idx.size(); ++i) {
                if (s_items[s_image_idx[i]].name == foundFile) { s_pos = i; break; }
            }
        } else {
            drawError(S().msg_no_images);
            delay(2000);
            goToDeepSleep();
        }
    }

    s_view_dir = s_current_dir;
    s_view_pos = s_pos;
    s_state = STATE_VIEW;
    showCurrentImage();
    s_last_activity = millis();
}

// ============================================================
void loop() {
    M5.update();
    auto t = M5.Touch.getDetail();

    if (t.wasPressed()) {
        s_touch_start_x = t.x;
        s_touch_start_y = t.y;
        s_touch_start_ms = millis();
    }

    if (t.wasReleased()) {
        int dx = (int)t.x - (int)s_touch_start_x;
        int dy = (int)t.y - (int)s_touch_start_y;
        uint32_t dur = millis() - s_touch_start_ms;
        bool isTap   = (abs(dx) < 20 && abs(dy) < 20);
        bool isFlick = (dur < 500) && (abs(dx) > 40 || abs(dy) > 40);

        Serial.printf("Release dx=%d dy=%d dur=%lu tap=%d flick=%d\n",
                      dx, dy, (unsigned long)dur, isTap, isFlick);

        switch (s_state) {
            case STATE_VIEW:
                if (isTap) { showMenuOverlay(); s_state = STATE_MENU; }
                break;

            case STATE_MENU:
                if (isTap) {
                    int hit = -1;
                    for (int i = 0; i < MENU_BUTTON_COUNT; ++i) {
                        if (inRect(menuButton(i), t.x, t.y)) { hit = i; break; }
                    }
                    Serial.printf("Menu hit = %d\n", hit);
                    if (hit == 0) {                         // <
                        s_pos = (s_pos + s_image_idx.size() - 1) % s_image_idx.size();
                        showCurrentImage();
                        s_state = STATE_VIEW;
                    } else if (hit == 1) {                  // >
                        s_pos = (s_pos + 1) % s_image_idx.size();
                        showCurrentImage();
                        s_state = STATE_VIEW;
                    } else if (hit == 2) {                  // 画像リスト
                        s_view_dir = s_current_dir;
                        s_view_pos = s_pos;
                        s_list_page = 0;
                        scanDir(s_current_dir.c_str());
                        s_state = STATE_LIST;
                        showList();
                    } else if (hit == 3) {                  // スライド
                        s_view_dir = s_current_dir;
                        s_view_pos = s_pos;
                        s_list_page = 0;
                        scanDir(s_current_dir.c_str());
                        s_state = STATE_SS_PICK_FOLDER;
                        showList();
                    } else if (hit == 4) {                  // 通信
                        s_state = STATE_COMM;
                        startCommMode();
                    } else if (hit == 5) {                  // 設定
                        s_state = STATE_SETTINGS;
                        showSettings();
                    } else {
                        // ボタン外タップ → メニューを閉じる
                        hideMenuOverlay();
                        s_state = STATE_VIEW;
                    }
                }
                break;

            case STATE_LIST:
            case STATE_SS_PICK_FOLDER: {
                int npages = (int)((s_items.size() + LIST_PER_PAGE - 1) / LIST_PER_PAGE);
                if (npages < 1) npages = 1;
                if (isFlick && abs(dy) > abs(dx)) {
                    if (dy < 0) s_list_page = (s_list_page + 1) % npages;
                    else        s_list_page = (s_list_page + npages - 1) % npages;
                    showList();
                } else if (isTap) {
                    int w = M5.Display.width();
                    int h = M5.Display.height();
                    int header_h = 60;
                    int footer_h = 50;
                    int area_h  = h - header_h - footer_h;
                    int row_h   = area_h / LIST_PER_PAGE;

                    // フッタタップ (行判定より優先)
                    if (t.y >= h - footer_h) {
                        if (t.x < w / 3) {
                            s_list_page = (s_list_page + npages - 1) % npages;
                            showList();
                        } else if (t.x > w * 2 / 3) {
                            s_list_page = (s_list_page + 1) % npages;
                            showList();
                        } else {
                            if (s_state == STATE_SS_PICK_FOLDER) {
                                // このフォルダで再生 → 間隔選択へ
                                s_ss_dir = s_current_dir;
                                s_state  = STATE_SS_INTERVAL;
                                showIntervalPicker();
                            } else {
                                // LIST を閉じる → 元のビューに戻す
                                s_current_dir = s_view_dir;
                                s_pos = s_view_pos;
                                scanDir(s_current_dir.c_str());
                                // s_pos の妥当性チェック
                                if (s_pos >= s_image_idx.size()) s_pos = 0;
                                s_state = STATE_VIEW;
                                showCurrentImage();
                            }
                        }
                        break;
                    }

                    // 行タップ
                    int row = (t.y - header_h) / row_h;
                    if (row >= 0 && row < LIST_PER_PAGE) {
                        size_t idx = s_list_page * LIST_PER_PAGE + row;
                        if (idx < s_items.size()) {
                            const Item& it = s_items[idx];
                            if (it.is_dir) {
                                if (it.name == "..") s_current_dir = parentOf(s_current_dir);
                                else                  s_current_dir = joinPath(s_current_dir, it.name);
                                scanDir(s_current_dir.c_str());
                                s_list_page = 0;
                                showList();
                            } else if (s_state == STATE_LIST) {
                                // 通常一覧: 画像選択 → 表示
                                for (size_t k = 0; k < s_image_idx.size(); ++k) {
                                    if (s_image_idx[k] == idx) { s_pos = k; break; }
                                }
                                s_state = STATE_VIEW;
                                showCurrentImage();
                            }
                            // SS_PICK_FOLDER で画像タップは無視 (フォルダ選択専用)
                        }
                    }
                }
                break;
            }

            case STATE_SS_INTERVAL: {
                // 桁 ▲/▼ ボタン
                bool hit = false;
                for (int i = 0; i < PICKER_DIGITS; ++i) {
                    if (inRect(pickerUpRect(i), t.x, t.y)) {
                        pickerInc(i);
                        redrawPickerDigit(i);
                        // clamp で他桁も変わっている可能性 → 全桁更新
                        for (int j = 0; j < PICKER_DIGITS; ++j)
                            if (j != i) redrawPickerDigit(j);
                        hit = true; break;
                    }
                    if (inRect(pickerDnRect(i), t.x, t.y)) {
                        pickerDec(i);
                        redrawPickerDigit(i);
                        for (int j = 0; j < PICKER_DIGITS; ++j)
                            if (j != i) redrawPickerDigit(j);
                        hit = true; break;
                    }
                }
                if (hit) break;

                // キャンセル
                if (inRect(pickerCancelRect(), t.x, t.y)) {
                    s_state = STATE_VIEW;
                    showCurrentImage();
                    break;
                }
                // 開始
                if (inRect(pickerOkRect(), t.x, t.y)) {
                    uint32_t sec = pickerTotalSec();
                    Serial.printf("Interval picked: %lu sec\n", (unsigned long)sec);
                    if (sec == 0) {
                        // 0 秒は開始しない
                        break;
                    }
                    startSlideshow(s_ss_dir, sec * 1000UL);
                }
                break;
            }

            case STATE_SLIDESHOW:
                // 任意タップで停止
                stopSlideshow();
                break;

            case STATE_COMM:
                // 「終了」ボタンを押した時のみ終了。それ以外のタップは無視。
                if (isTap && inRect(commExitButtonRect(), t.x, t.y)) {
                    stopCommMode();
                    // 通信中に SD 内容が変わった可能性があるため再スキャン
                    scanDir(s_current_dir.c_str());
                    if (s_pos >= s_image_idx.size()) s_pos = 0;
                    s_state = STATE_VIEW;
                    showCurrentImage();
                }
                break;

            case STATE_SETTINGS:
                if (isTap) {
                    if (inRect(settingsLangJaRect(), t.x, t.y)) {
                        if (s_lang != LANG_JA) { saveLang(LANG_JA); showSettings(); }
                    } else if (inRect(settingsLangEnRect(), t.x, t.y)) {
                        if (s_lang != LANG_EN) { saveLang(LANG_EN); showSettings(); }
                    } else if (inRect(settingsBackRect(), t.x, t.y)) {
                        s_state = STATE_VIEW;
                        showCurrentImage();
                    }
                }
                break;
        }
        s_last_activity = millis();
    }

    // スライドショー自動送り (light sleep モードのみ。deep sleep モードは setup() で完結)
    if (s_state == STATE_SLIDESHOW && s_ss_interval_ms > 0) {
        if ((uint32_t)(millis() - s_ss_last_change) >= s_ss_interval_ms) {
            if (!s_image_idx.empty()) {
                s_pos = (s_pos + 1) % s_image_idx.size();
                showCurrentImage();
            }
            s_ss_last_change = millis();
        }
    }

    // 通信モード中は Web サーバのクライアント処理 & idle タイマを常にリセット
    extern void commHandleClients();
    if (s_state == STATE_COMM) {
        commHandleClients();
        s_last_activity = millis();
    }

    // 通常モードのみ idle 監視 (スライドショー / 通信中はスリープしない)
    if (s_state != STATE_SLIDESHOW && s_state != STATE_COMM) {
        if ((uint32_t)(millis() - s_last_activity) >= IDLE_TIMEOUT_MS) {
            Serial.println("Idle timeout -> deep sleep");
            goToDeepSleep();
        }
    }

    // 案 A: スライドショー中は light sleep でアイドル時間を埋める
    //       タッチ反応性を保つため最大 200ms ずつ
    if (s_state == STATE_SLIDESHOW && s_ss_interval_ms > 0) {
        uint32_t elapsed = millis() - s_ss_last_change;
        uint32_t remain  = (s_ss_interval_ms > elapsed) ? (s_ss_interval_ms - elapsed) : 0;
        uint32_t sleep_ms = remain;
        if (sleep_ms > 200) sleep_ms = 200;
        if (sleep_ms >= 30) {
            esp_sleep_enable_timer_wakeup((uint64_t)sleep_ms * 1000ULL);
            esp_light_sleep_start();
        } else {
            delay(5);
        }
    } else if (s_state == STATE_COMM) {
        // Wi-Fi 動作中は light sleep を入れず軽量 delay
        delay(2);
    } else {
        delay(20);
    }
}

// ============================================================
static bool initSD() {
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    SDSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    const uint32_t freqs[] = { 4000000, 10000000, 20000000 };
    for (auto f : freqs) {
        if (SD.begin(SD_CS, SDSPI, f)) {
            if (SD.cardType() == CARD_NONE) { SD.end(); continue; }
            Serial.printf("SD OK @%luHz\n", (unsigned long)f);
            return true;
        }
    }
    return false;
}

// ============================================================
static bool endsWithCI(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[s.size() - n + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

static std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty() || a == "/") return "/" + b;
    if (!a.empty() && a.back() == '/') return a + b;
    return a + "/" + b;
}

static std::string parentOf(const std::string& p) {
    if (p == "/" || p.empty()) return "/";
    // 末尾スラッシュを除去してから探す
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    size_t slash = s.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return "/";
    return s.substr(0, slash);
}

// ============================================================
// ディレクトリをスキャン: フォルダ(昇順) + 画像(昇順)。".."を先頭に追加 (ルート以外)
static void scanDir(const char* dir) {
    s_items.clear();
    s_image_idx.clear();

    // ".." (ルート "/" 以外で表示)
    if (std::string(dir) != "/") {
        s_items.push_back({"..", true, IMG_NONE});
    }

    File d = SD.open(dir);
    if (!d || !d.isDirectory()) {
        Serial.printf("open(%s) failed\n", dir);
        return;
    }

    std::vector<Item> dirs, imgs;
    File e;
    while ((e = d.openNextFile())) {
        std::string name = e.name();
        size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        bool is_dir = e.isDirectory();
        e.close();
        if (name.empty() || name[0] == '.') continue;

        if (is_dir) {
            dirs.push_back({name, true, IMG_NONE});
        } else {
            ImgKind k = IMG_NONE;
            if (endsWithCI(name, ".jpg") || endsWithCI(name, ".jpeg")) k = IMG_JPG;
            else if (endsWithCI(name, ".png")) k = IMG_PNG;
            if (k != IMG_NONE) imgs.push_back({name, false, k});
        }
    }
    d.close();

    std::sort(dirs.begin(), dirs.end(), [](const Item& a, const Item& b){ return a.name < b.name; });
    std::sort(imgs.begin(), imgs.end(), [](const Item& a, const Item& b){ return a.name < b.name; });

    for (auto& it : dirs) s_items.push_back(it);
    for (auto& it : imgs) s_items.push_back(it);

    for (size_t i = 0; i < s_items.size(); ++i) {
        if (!s_items[i].is_dir) s_image_idx.push_back(i);
    }
    Serial.printf("scanDir %s: %u items (%u images)\n",
                  dir, (unsigned)s_items.size(), (unsigned)s_image_idx.size());
}

// ============================================================
static bool getJpgSize(const char* path, int& w, int& h) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    if (f.read() != 0xFF || f.read() != 0xD8) { f.close(); return false; }
    while (f.available()) {
        if (f.read() != 0xFF) continue;
        uint8_t marker; do { marker = f.read(); } while (marker == 0xFF);
        bool isSOF = (marker >= 0xC0 && marker <= 0xCF)
                  && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (isSOF) {
            f.read(); f.read(); f.read();
            uint8_t hh_hi = f.read(), hh_lo = f.read();
            uint8_t ww_hi = f.read(), ww_lo = f.read();
            h = (hh_hi << 8) | hh_lo;
            w = (ww_hi << 8) | ww_lo;
            f.close();
            return (w > 0 && h > 0);
        }
        uint8_t l_hi = f.read(), l_lo = f.read();
        uint16_t len = (l_hi << 8) | l_lo;
        if (len < 2) break;
        f.seek(f.position() + (len - 2));
    }
    f.close();
    return false;
}

static bool getPngSize(const char* path, int& w, int& h) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint8_t hdr[24];
    if (f.read(hdr, sizeof(hdr)) != sizeof(hdr)) { f.close(); return false; }
    f.close();
    static const uint8_t sig[8] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};
    for (int i = 0; i < 8; ++i) if (hdr[i] != sig[i]) return false;
    w = (hdr[16] << 24) | (hdr[17] << 16) | (hdr[18] << 8) | hdr[19];
    h = (hdr[20] << 24) | (hdr[21] << 16) | (hdr[22] << 8) | hdr[23];
    return (w > 0 && h > 0);
}

// ============================================================
static void drawFullScreenImg(ImgKind kind, const char* path) {
    auto& d = M5.Display;
    d.setEpdMode(epd_mode_t::epd_quality);
    d.fillScreen(TFT_WHITE);

    const int scr_w = d.width();
    const int scr_h = d.height();

    int iw = 0, ih = 0;
    bool sz_ok = (kind == IMG_PNG) ? getPngSize(path, iw, ih)
                                   : getJpgSize(path, iw, ih);
    if (!sz_ok) {
        if (kind == IMG_PNG) d.drawPngFile(SD, path, 0, 0, scr_w, scr_h, 0, 0, 0.0f, middle_center);
        else                 d.drawJpgFile(SD, path, 0, 0, scr_w, scr_h, 0, 0, 0.0f, middle_center);
        d.display();
        return;
    }

    LGFX_Sprite src(&d), dst(&d);
    src.setPsram(true); src.setColorDepth(16);
    dst.setPsram(true); dst.setColorDepth(16);
    if (!src.createSprite(iw, ih) || !dst.createSprite(scr_w, scr_h)) {
        Serial.println("sprite alloc failed, fallback");
        if (kind == IMG_PNG) d.drawPngFile(SD, path, 0, 0, scr_w, scr_h, 0, 0, 0.0f, middle_center);
        else                 d.drawJpgFile(SD, path, 0, 0, scr_w, scr_h, 0, 0, 0.0f, middle_center);
        d.display();
        src.deleteSprite(); dst.deleteSprite();
        return;
    }
    src.fillSprite(TFT_WHITE);
    if (kind == IMG_PNG) src.drawPngFile(SD, path, 0, 0, iw, ih);
    else                 src.drawJpgFile(SD, path, 0, 0, iw, ih);

    dst.fillSprite(TFT_WHITE);

    // 縦長画像は 90° 回転して横長画面にフィットさせる
    //   回転後の見かけ寸法: width=ih, height=iw
    //   それを scr_w × scr_h に埋めるよう zx/zy を決める
    const bool portrait = (iw < ih);
    float angle, zx, zy;
    if (portrait) {
        angle = 90.0f;
        zx = (float)scr_h / (float)iw;   // 回転後の高さ ← 元幅
        zy = (float)scr_w / (float)ih;   // 回転後の幅   ← 元高さ
    } else {
        angle = 0.0f;
        zx = (float)scr_w / (float)iw;
        zy = (float)scr_h / (float)ih;
    }
    src.pushRotateZoomWithAA(&dst, scr_w / 2, scr_h / 2, angle, zx, zy);
    dst.pushSprite(&d, 0, 0);

    src.deleteSprite();
    dst.deleteSprite();
    d.display();
}

// ============================================================
static void showCurrentImage() {
    if (s_image_idx.empty()) return;
    size_t ii = s_image_idx[s_pos];
    const Item& it = s_items[ii];
    std::string full = joinPath(s_current_dir, it.name);
    Serial.printf("[%u/%u] %s\n",
                  (unsigned)(s_pos + 1), (unsigned)s_image_idx.size(), full.c_str());
    // 案 C: デコード中は 240MHz、終わったら 80MHz に戻す
    cpuHigh();
    drawFullScreenImg(it.kind, full.c_str());
    M5.Display.waitDisplay();
    cpuLow();
    // 表示するたびに位置を保存 (電源 OFF でも復帰できる)
    savePosition();
}

// ============================================================
// メニューオーバーレイ (画像は残したまま下部にバー)
// ============================================================
static void drawMenuButton(int idx, const char* label) {
    auto& d = M5.Display;
    Rect r = menuButton(idx);
    d.fillRect(r.x, r.y, r.w, r.h, TFT_BLACK);
    d.drawRect(r.x, r.y, r.w, r.h, TFT_WHITE);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setFont(&fonts::efontJA_24_b);
    d.setTextDatum(middle_center);
    d.drawString(label, r.x + r.w / 2, r.y + r.h / 2);
    d.setTextDatum(top_left);
}

static void showMenuOverlay() {
    auto& d = M5.Display;
    d.setEpdMode(epd_mode_t::epd_fast);

    // 上部: 情報バー (左: 現在のファイル名 / 右: 電池残量)
    Rect b = batteryBarRect();
    d.fillRect(b.x, b.y, b.w, b.h, TFT_BLACK);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setFont(&fonts::efontJA_24_b);

    // 左: [pos/total] filename
    std::string fname = S().no_image;
    if (!s_image_idx.empty() && s_pos < s_image_idx.size()) {
        fname = s_items[s_image_idx[s_pos]].name;
    }
    char lbuf[96];
    if (!s_image_idx.empty()) {
        snprintf(lbuf, sizeof(lbuf), "[%u/%u] %s",
                 (unsigned)(s_pos + 1), (unsigned)s_image_idx.size(), fname.c_str());
    } else {
        snprintf(lbuf, sizeof(lbuf), "%s", fname.c_str());
    }

    // 右: 電池残量 を先に用意 (左の最大幅を計算するため)
    int lvl = getBatteryLevel();
    char bbuf[32];
    if (lvl >= 0) snprintf(bbuf, sizeof(bbuf), S().battery, lvl);
    else          snprintf(bbuf, sizeof(bbuf), "%s", S().battery_na);

    int right_w = d.textWidth(bbuf);
    int max_left_w = b.w - right_w - 16 * 3;   // 左右マージン + 中間余白
    // 左テキストを幅に収まるまで末尾を切り詰め ("..." を付ける)
    std::string left = lbuf;
    if (d.textWidth(left.c_str()) > max_left_w && max_left_w > 0) {
        std::string trimmed = left;
        while (trimmed.size() > 3 && d.textWidth((trimmed + "...").c_str()) > max_left_w) {
            trimmed.pop_back();
        }
        left = trimmed + "...";
    }

    d.setTextDatum(middle_left);
    d.drawString(left.c_str(), 16, b.y + b.h / 2);
    d.setTextDatum(middle_right);
    d.drawString(bbuf, b.w - 16, b.y + b.h / 2);
    d.setTextDatum(top_left);

    // 下部: 6 ボタン [<] [>] [画像リスト] [スライド] [通信] [設定]
    drawMenuButton(0, "<");
    drawMenuButton(1, ">");
    drawMenuButton(2, S().menu_list);
    drawMenuButton(3, S().menu_slide);
    drawMenuButton(4, S().menu_comm);
    drawMenuButton(5, S().menu_cfg);

    d.display();
    d.waitDisplay();
}

static void hideMenuOverlay() {
    // メニュー領域だけ元画像に戻すのは手間なので、画像を再描画 (品質モード)
    showCurrentImage();
}

// ============================================================
// ファイル/フォルダ一覧表示
// ============================================================
static void showList() {
    auto& d = M5.Display;
    d.setEpdMode(epd_mode_t::epd_quality);
    d.fillScreen(TFT_WHITE);
    const int w = d.width();
    const int h = d.height();
    const int header_h = 60;
    const int footer_h = 50;
    const int area_h  = h - header_h - footer_h;
    const int row_h   = area_h / LIST_PER_PAGE;

    // ヘッダ: 現在パス
    d.setTextColor(TFT_BLACK, TFT_WHITE);
    d.setFont(&fonts::efontJA_24_b);
    d.setTextDatum(middle_left);
    d.drawString(s_current_dir.c_str(), 16, header_h / 2);
    d.drawFastHLine(0, header_h, w, TFT_BLACK);

    // ページ計算
    int total  = (int)s_items.size();
    int npages = (total + LIST_PER_PAGE - 1) / LIST_PER_PAGE;
    if (npages < 1) npages = 1;
    if (s_list_page >= npages) s_list_page = 0;

    // 行描画
    d.setFont(&fonts::efontJA_24);
    int start = s_list_page * LIST_PER_PAGE;
    for (int row = 0; row < LIST_PER_PAGE; ++row) {
        int idx = start + row;
        int y = header_h + row * row_h;
        d.drawFastHLine(0, y + row_h, w, TFT_BLACK);
        if (idx >= total) continue;
        const Item& it = s_items[idx];
        std::string label;
        if (it.is_dir) label = (it.name == "..") ? "[ .. ]" : "[" + it.name + "]";
        else           label = it.name;
        d.setTextDatum(middle_left);
        d.drawString(label.c_str(), 20, y + row_h / 2);
        // 種別マーク
        const char* mark = it.is_dir ? "DIR" :
                            (it.kind == IMG_PNG ? "PNG" : "JPG");
        d.setTextDatum(middle_right);
        d.drawString(mark, w - 20, y + row_h / 2);
    }

    // フッタ: [< prev]  [close / ここで再生]  [next >]
    d.drawFastHLine(0, h - footer_h, w, TFT_BLACK);
    d.setFont(&fonts::efontJA_24_b);
    d.setTextDatum(middle_center);
    d.drawString("< PREV", w / 6, h - footer_h / 2);
    char buf[48];
    if (s_state == STATE_SS_PICK_FOLDER) {
        snprintf(buf, sizeof(buf), S().list_play_folder, s_list_page + 1, npages);
    } else {
        snprintf(buf, sizeof(buf), S().list_close, s_list_page + 1, npages);
    }
    d.drawString(buf, w / 2, h - footer_h / 2);
    d.drawString("NEXT >", w * 5 / 6, h - footer_h / 2);
    d.setTextDatum(top_left);

    d.display();
    d.waitDisplay();
}

// ============================================================
static void drawSleepMark() {
    auto& d = M5.Display;
    d.setEpdMode(epd_mode_t::epd_fast);
    const int w = 140, hh = 44;
    const int x = d.width() - w - 10;
    const int y = 10;
    d.fillRect(x, y, w, hh, TFT_BLACK);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setFont(&fonts::FreeSansBold18pt7b);
    d.setTextDatum(middle_center);
    d.drawString("SLEEP", x + w / 2, y + hh / 2);
    d.setTextDatum(top_left);
    d.display();
    d.waitDisplay();
}

// 現在位置を RTC + NVS に保存
static void savePosition() {
    strncpy(s_saved_dir, s_current_dir.c_str(), sizeof(s_saved_dir) - 1);
    s_saved_dir[sizeof(s_saved_dir) - 1] = 0;
    if (!s_image_idx.empty() && s_pos < s_image_idx.size()) {
        const std::string& n = s_items[s_image_idx[s_pos]].name;
        strncpy(s_saved_file, n.c_str(), sizeof(s_saved_file) - 1);
        s_saved_file[sizeof(s_saved_file) - 1] = 0;
    }
    // NVS にも保存 (電源 OFF でも保持)
    Preferences prefs;
    prefs.begin("viewer", false);
    prefs.putString("dir", s_saved_dir);
    prefs.putString("file", s_saved_file);
    prefs.end();
    Serial.printf("[save] dir=%s file=%s\n", s_saved_dir, s_saved_file);
}

// ルートから再帰的に最初の画像を探す (depth制限付き)
static bool findFirstImage(const char* dir, int depth, std::string& outDir, std::string& outFile) {
    if (depth > 5) return false;   // 深すぎる階層は打ち切り
    File d = SD.open(dir);
    if (!d || !d.isDirectory()) return false;

    std::vector<std::string> subdirs;
    std::vector<std::string> images;
    File e;
    while ((e = d.openNextFile())) {
        std::string name = e.name();
        size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        bool is_dir = e.isDirectory();
        e.close();
        if (name.empty() || name[0] == '.') continue;

        if (is_dir) {
            subdirs.push_back(name);
        } else {
            if (endsWithCI(name, ".jpg") || endsWithCI(name, ".jpeg") || endsWithCI(name, ".png")) {
                images.push_back(name);
            }
        }
    }
    d.close();

    // このディレクトリに画像があれば最初の1枚を返す
    if (!images.empty()) {
        std::sort(images.begin(), images.end());
        outDir = dir;
        outFile = images[0];
        return true;
    }

    // サブフォルダを再帰的に探す
    std::sort(subdirs.begin(), subdirs.end());
    for (auto& sub : subdirs) {
        std::string path = std::string(dir);
        if (path.back() != '/') path += '/';
        path += sub;
        if (findFirstImage(path.c_str(), depth + 1, outDir, outFile)) return true;
    }
    return false;
}

static void goToDeepSleep() {
    savePosition();

    // メニュー等のオーバーレイが出ていれば閉じてから SLEEP マークを出す
    if (s_state != STATE_VIEW && !s_image_idx.empty()) {
        Serial.println("Hide overlay before sleep");
        showCurrentImage();
        s_state = STATE_VIEW;
    }

    drawSleepMark();
    SD.end(); SDSPI.end();
    Serial.println("Deep sleep. Press power button to wake.");
    Serial.flush();
    M5.Power.deepSleep();
}

// ============================================================
static void drawError(const char* msg) {
    auto& d = M5.Display;
    d.setEpdMode(epd_mode_t::epd_quality);
    d.fillScreen(TFT_WHITE);
    d.setTextColor(TFT_BLACK, TFT_WHITE);
    // アプリ名
    d.setFont(&fonts::FreeSansBold18pt7b);
    d.setCursor(40, 50); d.print("M5PaperS3-PocketFrame");
    // 区切り線
    d.drawFastHLine(40, 80, d.width() - 80, TFT_BLACK);
    // メッセージ本文
    d.setFont(&fonts::efontJA_24_b);
    d.setCursor(40, 120); d.print(msg);
    d.display(); d.waitDisplay();
    Serial.printf("MSG: %s\n", msg);
}

// ============================================================
// 設定画面
// ============================================================
static Rect settingsLangJaRect() { return { 160, 230, 280, 80 }; }
static Rect settingsLangEnRect() { return { 520, 230, 280, 80 }; }
static Rect settingsBackRect()   { return { 380, 430, 200, 64 }; }

static void saveLang(uint8_t lang) {
    s_lang = lang;
    Preferences prefs;
    prefs.begin("viewer", false);   // read-write
    prefs.putUChar("lang", s_lang);
    prefs.end();
}

static void showSettings() {
    auto& d = M5.Display;
    d.setEpdMode(epd_mode_t::epd_quality);
    d.fillScreen(TFT_WHITE);
    const int W = d.width();

    // アプリ名 + バージョン + 設定タイトル
    d.setTextColor(TFT_BLACK, TFT_WHITE);
    d.setFont(&fonts::FreeSansBold18pt7b);
    d.setTextDatum(middle_center);
    d.drawString("M5PaperS3-PocketFrame", W / 2, 28);
    d.setFont(&fonts::efontJA_24);
    d.drawString("v1.0", W / 2, 60);
    d.setFont(&fonts::efontJA_24_b);
    d.drawString(S().settings_title, W / 2, 90);
    d.drawFastHLine(0, 110, W, TFT_BLACK);

    // 言語選択ラベル
    d.setFont(&fonts::efontJA_24);
    d.drawString(S().settings_lang, W / 2, 160);

    // 日本語ボタン
    Rect ja = settingsLangJaRect();
    if (s_lang == LANG_JA) {
        d.fillRect(ja.x, ja.y, ja.w, ja.h, TFT_BLACK);
        d.setTextColor(TFT_WHITE, TFT_BLACK);
    } else {
        d.drawRect(ja.x, ja.y, ja.w, ja.h, TFT_BLACK);
        d.drawRect(ja.x + 1, ja.y + 1, ja.w - 2, ja.h - 2, TFT_BLACK);
        d.setTextColor(TFT_BLACK, TFT_WHITE);
    }
    d.setFont(&fonts::efontJA_24_b);
    d.drawString("日本語", ja.x + ja.w / 2, ja.y + ja.h / 2);

    // English ボタン
    Rect en = settingsLangEnRect();
    if (s_lang == LANG_EN) {
        d.fillRect(en.x, en.y, en.w, en.h, TFT_BLACK);
        d.setTextColor(TFT_WHITE, TFT_BLACK);
    } else {
        d.drawRect(en.x, en.y, en.w, en.h, TFT_BLACK);
        d.drawRect(en.x + 1, en.y + 1, en.w - 2, en.h - 2, TFT_BLACK);
        d.setTextColor(TFT_BLACK, TFT_WHITE);
    }
    d.drawString("English", en.x + en.w / 2, en.y + en.h / 2);

    // 戻るボタン
    Rect bk = settingsBackRect();
    d.fillRect(bk.x, bk.y, bk.w, bk.h, TFT_BLACK);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString(S().settings_back, bk.x + bk.w / 2, bk.y + bk.h / 2);

    d.setTextColor(TFT_BLACK, TFT_WHITE);
    d.setTextDatum(top_left);
    d.display();
    d.waitDisplay();
}

// ============================================================
// 電池残量取得
// ============================================================
static int getBatteryLevel() {
    int lvl = M5.Power.getBatteryLevel();   // 0..100, 取得失敗時 -1
    if (lvl < 0) return -1;
    if (lvl > 100) lvl = 100;
    return lvl;
}

// ============================================================
// スライドショー間隔選択画面: HH:MM:SS 桁送りピッカー
//   各桁の上下に ▲▼ ボタン。最大 24:00:00。
//   60 秒未満 → Light Sleep, 60 秒以上 → Deep Sleep
// ============================================================
// --- レイアウト定数 (画面 960x540 想定) ---
static constexpr int PICK_DIGIT_W   = 90;
static constexpr int PICK_DIGIT_H   = 110;
static constexpr int PICK_BTN_H     = 80;
static constexpr int PICK_DIGIT_GAP = 8;    // 同グループ内
static constexpr int PICK_GROUP_GAP = 56;   // H-M / M-S 間
static constexpr int PICK_DIGITS_Y  = 210;  // 数字表示の top
static constexpr int PICK_UP_Y      = PICK_DIGITS_Y - PICK_BTN_H - 10;  // 120
static constexpr int PICK_DN_Y      = PICK_DIGITS_Y + PICK_DIGIT_H + 10; // 330
static constexpr int PICK_LABEL_Y   = PICK_DN_Y + PICK_BTN_H + 8;        // 418
static constexpr int PICK_OK_Y      = 472;
static constexpr int PICK_OK_H      = 58;

// 各桁の左上 x を返す
static int pickerDigitX(int idx) {
    // 全体幅: 3グループ * (digit*2 + gap) + 2 * group_gap
    const int group_w = PICK_DIGIT_W * 2 + PICK_DIGIT_GAP;
    const int total_w = group_w * 3 + PICK_GROUP_GAP * 2;
    const int left    = (M5.Display.width() - total_w) / 2;
    int g = idx / 2;          // 0:H 1:M 2:S
    int i = idx % 2;          // 0:tens 1:ones
    int x = left + g * (group_w + PICK_GROUP_GAP) + i * (PICK_DIGIT_W + PICK_DIGIT_GAP);
    return x;
}
static Rect pickerDigitRect(int idx) {
    return { pickerDigitX(idx), PICK_DIGITS_Y, PICK_DIGIT_W, PICK_DIGIT_H };
}
static Rect pickerUpRect(int idx) {
    return { pickerDigitX(idx), PICK_UP_Y, PICK_DIGIT_W, PICK_BTN_H };
}
static Rect pickerDnRect(int idx) {
    return { pickerDigitX(idx), PICK_DN_Y, PICK_DIGIT_W, PICK_BTN_H };
}
// 開始 / キャンセル ボタン
static Rect pickerOkRect() {
    int W = M5.Display.width();
    return { W / 2 - 220, PICK_OK_Y, 200, PICK_OK_H };
}
static Rect pickerCancelRect() {
    int W = M5.Display.width();
    return { W / 2 + 20, PICK_OK_Y, 200, PICK_OK_H };
}

// 現在値 (H,M,S / 合計秒)
static void pickerGetHMS(int& h, int& m, int& s) {
    h = s_pd[0] * 10 + s_pd[1];
    m = s_pd[2] * 10 + s_pd[3];
    s = s_pd[4] * 10 + s_pd[5];
}
static uint32_t pickerTotalSec() {
    int h, m, s; pickerGetHMS(h, m, s);
    return (uint32_t)h * 3600UL + (uint32_t)m * 60UL + (uint32_t)s;
}
// 24:00:00 を超えたら 24:00:00 に丸める
static void pickerClamp() {
    int h, m, s; pickerGetHMS(h, m, s);
    uint32_t total = (uint32_t)h * 3600UL + (uint32_t)m * 60UL + (uint32_t)s;
    if (total > 24UL * 3600UL) {
        s_pd[0] = 2; s_pd[1] = 4;
        s_pd[2] = 0; s_pd[3] = 0;
        s_pd[4] = 0; s_pd[5] = 0;
    }
}
// idx 桁の剰余範囲
static int pickerDigitMod(int idx) {
    switch (idx) {
        case 0: return 3;   // H10: 0-2
        case 2: return 6;   // M10: 0-5
        case 4: return 6;   // S10: 0-5
        default: return 10; // 1 桁
    }
}
static void pickerInc(int idx) {
    int m = pickerDigitMod(idx);
    s_pd[idx] = (s_pd[idx] + 1) % m;
    pickerClamp();
}
static void pickerDec(int idx) {
    int m = pickerDigitMod(idx);
    s_pd[idx] = (s_pd[idx] + m - 1) % m;
    pickerClamp();
}

static void drawTriangleUp(int cx, int cy, int sz, uint16_t color) {
    M5.Display.fillTriangle(cx - sz, cy + sz / 2,
                            cx + sz, cy + sz / 2,
                            cx,      cy - sz / 2, color);
}
static void drawTriangleDown(int cx, int cy, int sz, uint16_t color) {
    M5.Display.fillTriangle(cx - sz, cy - sz / 2,
                            cx + sz, cy - sz / 2,
                            cx,      cy + sz / 2, color);
}

static void showIntervalPicker() {
    auto& d = M5.Display;
    d.setEpdMode(epd_mode_t::epd_quality);
    d.fillScreen(TFT_WHITE);

    const int W = d.width();
    const int title_h = 110;

    // --- タイトル / 説明 ---
    d.setTextColor(TFT_BLACK, TFT_WHITE);
    d.setFont(&fonts::efontJA_24_b);
    d.setTextDatum(middle_center);
    char t[160];
    snprintf(t, sizeof(t), S().picker_title, s_ss_dir.c_str());
    d.drawString(t, W / 2, 24);

    d.setFont(&fonts::efontJA_24);
    d.drawString(S().picker_ls_desc, W / 2, 58);
    d.drawString(S().picker_ds_desc, W / 2, 86);
    d.drawFastHLine(0, title_h, W, TFT_BLACK);

    // --- 桁 (数字 + 上下ボタン) ---
    d.setFont(&fonts::efontJA_24_b);
    for (int i = 0; i < PICKER_DIGITS; ++i) {
        Rect dr = pickerDigitRect(i);
        Rect ur = pickerUpRect(i);
        Rect lr = pickerDnRect(i);

        // 数字枠
        d.drawRect(dr.x, dr.y, dr.w, dr.h, TFT_BLACK);
        d.drawRect(dr.x + 1, dr.y + 1, dr.w - 2, dr.h - 2, TFT_BLACK);

        // 数字
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", s_pd[i]);
        d.setFont(&fonts::Font8);   // 大型数字フォント (7-seg風)
        d.setTextDatum(middle_center);
        d.drawString(buf, dr.x + dr.w / 2, dr.y + dr.h / 2);
        d.setFont(&fonts::efontJA_24_b);

        // 上ボタン
        d.drawRect(ur.x, ur.y, ur.w, ur.h, TFT_BLACK);
        drawTriangleUp(ur.x + ur.w / 2, ur.y + ur.h / 2, 24, TFT_BLACK);

        // 下ボタン
        d.drawRect(lr.x, lr.y, lr.w, lr.h, TFT_BLACK);
        drawTriangleDown(lr.x + lr.w / 2, lr.y + lr.h / 2, 24, TFT_BLACK);
    }

    // --- グループラベル 時間 / 分 / 秒 ---
    d.setFont(&fonts::efontJA_24_b);
    d.setTextDatum(middle_center);
    const char* names[3] = { S().picker_hour, S().picker_min, S().picker_sec };
    for (int g = 0; g < 3; ++g) {
        int xL = pickerDigitX(g * 2);
        int xR = pickerDigitX(g * 2 + 1) + PICK_DIGIT_W;
        int cx = (xL + xR) / 2;
        d.drawString(names[g], cx, PICK_LABEL_Y + 18);
    }

    // --- 開始 / キャンセル ---
    Rect ok = pickerOkRect();
    Rect cn = pickerCancelRect();

    d.fillRect(ok.x, ok.y, ok.w, ok.h, TFT_BLACK);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString(S().picker_start, ok.x + ok.w / 2, ok.y + ok.h / 2);

    d.setTextColor(TFT_BLACK, TFT_WHITE);
    d.drawRect(cn.x, cn.y, cn.w, cn.h, TFT_BLACK);
    d.drawRect(cn.x + 1, cn.y + 1, cn.w - 2, cn.h - 2, TFT_BLACK);
    d.drawString(S().picker_cancel, cn.x + cn.w / 2, cn.y + cn.h / 2);

    d.setTextDatum(top_left);
    d.display();
    d.waitDisplay();
}

// 数字桁だけを再描画 (ボタン押下時)
static void redrawPickerDigit(int idx) {
    auto& d = M5.Display;
    d.setEpdMode(epd_mode_t::epd_fast);
    Rect dr = pickerDigitRect(idx);
    d.fillRect(dr.x + 2, dr.y + 2, dr.w - 4, dr.h - 4, TFT_WHITE);
    d.setTextColor(TFT_BLACK, TFT_WHITE);
    d.setFont(&fonts::Font8);
    d.setTextDatum(middle_center);
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", s_pd[idx]);
    d.drawString(buf, dr.x + dr.w / 2, dr.y + dr.h / 2);
    d.setTextDatum(top_left);
    d.display();
    d.waitDisplay();
    d.setEpdMode(epd_mode_t::epd_quality);
}

// ============================================================
static void startSlideshow(const std::string& dir, uint32_t interval_ms) {
    Serial.printf("Start slideshow dir=%s interval=%lums (deep=%d)\n",
                  dir.c_str(), (unsigned long)interval_ms,
                  interval_ms >= DEEP_SLEEP_THRESHOLD_MS);

    s_current_dir = dir;
    scanDir(s_current_dir.c_str());
    if (s_image_idx.empty()) {
        Serial.println("slideshow: no images, abort");
        drawError(S().msg_no_images_folder);
        delay(1500);
        // 元のビューディレクトリに戻す
        s_current_dir = s_view_dir;
        s_pos = s_view_pos;
        scanDir(s_current_dir.c_str());
        if (s_pos >= s_image_idx.size()) s_pos = 0;
        s_state = STATE_VIEW;
        showCurrentImage();
        return;
    }

    s_ss_interval_ms = interval_ms;
    s_pos = 0;
    s_state = STATE_SLIDESHOW;
    showCurrentImage();

    if (interval_ms >= DEEP_SLEEP_THRESHOLD_MS) {
        // 案 B: deep sleep (画像を表示後すぐ完全電源 OFF)
        enterSlideshowDeepSleep(interval_ms);
        // 戻ってこない
    } else {
        // 案 A: light sleep (loop 内で短時間ずつ寝る)
        s_ss_last_change = millis();
    }
}

// ============================================================
static void stopSlideshow() {
    Serial.println("Stop slideshow");
    s_rtc_ss_active = false;     // deep sleep 継続フラグもクリア
    s_state = STATE_VIEW;
    showCurrentImage();
}

// ============================================================
// Deep sleep スライドショー: 画像表示後ここで完全電源 OFF。
// timer wakeup で setup() が再実行され handleSlideshowResume() に入る。
// ============================================================
static void enterSlideshowDeepSleep(uint32_t interval_ms) {
    // RTC に継続情報を保存
    s_rtc_ss_active      = true;
    s_rtc_ss_interval_ms = interval_ms;
    strncpy(s_rtc_ss_dir, s_current_dir.c_str(), sizeof(s_rtc_ss_dir) - 1);
    s_rtc_ss_dir[sizeof(s_rtc_ss_dir) - 1] = 0;

    // 現在ファイルを s_saved_* にも残す (resume が次の画像を計算するため)
    savePosition();

    Serial.printf("[SS] deep sleep for %lu ms\n", (unsigned long)interval_ms);
    SD.end(); SDSPI.end();
    Serial.flush();
    // 画面はそのまま残す (SLEEP マークは描かない)
    M5.Power.deepSleep((uint64_t)interval_ms * 1000ULL);
}

// ============================================================
// deep sleep 継続中のウェイクアップ: 次画像を表示してまた寝る。
// ============================================================
static void handleSlideshowResume() {
    Serial.println("[SS] resume from deep sleep");

    s_current_dir = s_rtc_ss_dir[0] ? s_rtc_ss_dir : "/";
    scanDir(s_current_dir.c_str());
    if (s_image_idx.empty()) {
        Serial.println("[SS] folder empty, exit slideshow");
        s_rtc_ss_active = false;
        return;
    }
    // 現在位置を s_saved_file から復元 → +1
    s_pos = 0;
    for (size_t i = 0; i < s_image_idx.size(); ++i) {
        if (s_items[s_image_idx[i]].name == s_saved_file) { s_pos = i; break; }
    }
    s_pos = (s_pos + 1) % s_image_idx.size();

    cpuHigh();
    showCurrentImage();
    cpuLow();

    enterSlideshowDeepSleep(s_rtc_ss_interval_ms);
    // 戻らない
}

// ============================================================
// 通信モード: Wi-Fi AP + HTTP ファイル管理サーバ
// ============================================================
static WebServer    g_http(80);
static File         g_up_file;
static bool         g_comm_running = false;
static char         g_ap_ssid[32] = {0};
static const char*  g_ap_pass = "m5paper00";   // 8 文字以上必須 (WPA2)
static char         g_ap_url[32]  = {0};

// ---- JSON ヘルパ ----
static void jsonEscape(const std::string& s, std::string& out) {
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8]; snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
                    out += b;
                } else {
                    out += c;
                }
        }
    }
}

static bool isImgName(const std::string& n) {
    return endsWithCI(n, ".jpg") || endsWithCI(n, ".jpeg") || endsWithCI(n, ".png");
}

// ---- HTTP ハンドラ ----
// Web UI HTML テンプレート — 言語文字列は L オブジェクト (handleRoot で注入) を参照
static const char HTML_INDEX[] PROGMEM = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>M5PaperS3-PocketFrame</title>
<style>
body{font-family:sans-serif;margin:1em;color:#111;max-width:900px}
h1{font-size:1.2em;margin:.2em 0}
table{width:100%;border-collapse:collapse;font-size:.95em}
td,th{border:1px solid #ccc;padding:.4em;text-align:left;vertical-align:middle}
.row{margin:.6em 0}
.bar{display:flex;gap:.5em;align-items:center;flex-wrap:wrap}
button{padding:.45em 1em;font-size:.95em;cursor:pointer}
input[type=text],input[type=number],select{padding:.35em;font-size:.95em}
.dir{color:#06c;font-weight:bold;cursor:pointer}
.pathbar{font-family:monospace;background:#f5f5f5;padding:.4em .7em;border:1px solid #ddd;display:inline-block;min-width:20em}
.log{font-family:monospace;font-size:.85em;color:#444;white-space:pre-wrap;background:#fafafa;padding:.5em;border:1px solid #eee;min-height:3em;margin-top:.8em}
.opt{background:#eef7ff;padding:.4em .7em;border:1px solid #cde;border-radius:4px}
.bigbtn{display:inline-block;padding:.9em 1.8em;font-size:1.1em;background:#2a66d8;color:#fff;border:none;border-radius:6px;cursor:pointer}
.bigbtn:hover{background:#1e4da0}
.bigbtn.green{background:#2a9f3a}
.bigbtn.green:hover{background:#1d7727}
.preview{display:flex;flex-wrap:wrap;gap:10px;margin:.6em 0;min-height:4em;padding:.4em;background:#fafafa;border:1px dashed #ccc;border-radius:4px}
.preview .item{font-size:.8em;text-align:center;max-width:170px}
.preview img{display:block;max-width:160px;max-height:110px;border:1px solid #bbb;object-fit:contain;background:#fff}
.preview .fname{word-break:break-all;color:#555;margin-top:2px}
</style></head><body>
<h1>M5PaperS3-PocketFrame</h1>

<div class="row bar">
 <button onclick="up()" id="btnUp"></button>
 <button onclick="reload()" id="btnReload"></button>
</div>
<div class="row">
 Path: <span id="path" class="pathbar">/</span>
</div>

<div class="row opt">
 <label><input type="checkbox" id="resize" checked> <span id="lblResize"></span></label>
</div>
<div class="row opt">
 <span id="lblOrient"></span>
 <select id="orient">
  <option value="auto" selected id="optAuto"></option>
  <option value="landscape" id="optLand"></option>
  <option value="portrait" id="optPort"></option>
 </select>
</div>

<div class="row">
 <label for="files" class="bigbtn" id="lblChoose"></label>
 <input type="file" id="files" multiple accept="image/*,.jpg,.jpeg,.png" style="display:none" onchange="showPreview()">
 <span id="fcount" style="margin-left:.7em;color:#666"></span>
</div>

<div id="preview" class="preview"><span id="pvEmpty" style="color:#999;font-size:.85em"></span></div>

<div class="row">
 <button onclick="upload()" class="bigbtn green" id="btnUpload"></button>
</div>

<div class="row bar">
 <input type="text" id="newdir" style="flex:1;min-width:12em">
</div>
<div class="row">
 <button onclick="mkdir()" id="btnMkdir"></button>
</div>

<table id="tbl"><thead><tr><th id="thType"></th><th id="thName"></th><th id="thSize"></th><th id="thAction"></th></tr></thead><tbody></tbody></table>
<div class="log" id="log">ready.</div>
<script>
// L is injected by the server before this script (see handleRoot)
// Apply labels on load
document.getElementById('btnUp').textContent=L.up;
document.getElementById('btnReload').textContent=L.rl;
document.getElementById('lblResize').textContent=L.resize;
document.getElementById('lblOrient').textContent=L.orient;
document.getElementById('optAuto').textContent=L.oAuto;
document.getElementById('optLand').textContent=L.oLand;
document.getElementById('optPort').textContent=L.oPort;
document.getElementById('lblChoose').textContent=L.choose;
document.getElementById('pvEmpty').textContent=L.pvEmpty;
document.getElementById('btnUpload').textContent=L.upload;
document.getElementById('newdir').placeholder=L.newdir;
document.getElementById('btnMkdir').textContent=L.mkdir;
document.getElementById('thType').textContent=L.thType;
document.getElementById('thName').textContent=L.thName;
document.getElementById('thSize').textContent=L.thSize;
document.getElementById('thAction').textContent=L.thAct;

let cwd='/gallery';
const MAX_W=960, MAX_H=540;
const q=encodeURIComponent;
const log=(s)=>{document.getElementById('log').textContent=s};
const addlog=(s)=>{const el=document.getElementById('log'); el.textContent += '\n'+s};
const fmtKB=(n)=>Math.round(n/1024)+' KB';

async function reload(){
 document.getElementById('path').textContent=cwd;
 try{
  const r=await fetch('/api/list?path='+q(cwd));
  const j=await r.json();
  const tb=document.querySelector('#tbl tbody');
  tb.innerHTML='';
  for(const it of j.items){
   const tr=document.createElement('tr');
   const tdT=document.createElement('td'); tdT.textContent=it.is_dir?'DIR':(it.kind||'FILE'); tr.appendChild(tdT);
   const tdN=document.createElement('td');
   if(it.is_dir){const a=document.createElement('span');a.className='dir';a.textContent='['+it.name+']';a.onclick=()=>enter(it.name);tdN.appendChild(a);} else {tdN.textContent=it.name;}
   tr.appendChild(tdN);
   const tdS=document.createElement('td'); tdS.textContent=it.is_dir?'':it.size; tr.appendChild(tdS);
   const tdA=document.createElement('td');
   const bDel=document.createElement('button'); bDel.textContent=L.del; bDel.onclick=()=>del(it.name,it.is_dir); tdA.appendChild(bDel);
   const bMv=document.createElement('button'); bMv.textContent=L.mv; bMv.onclick=()=>mv(it.name); tdA.appendChild(bMv);
   tr.appendChild(tdA);
   tb.appendChild(tr);
  }
 }catch(e){log('list error: '+e)}
}
function enter(n){cwd=(cwd.endsWith('/')?cwd:cwd+'/')+n;reload();}
function up(){let p=cwd.replace(/\/[^\/]*\/?$/,'');if(!p)p='/';cwd=p;reload();}

// --- 選択画像のプレビュー ---
function showPreview(){
 const files=document.getElementById('files').files;
 const pv=document.getElementById('preview');
 pv.innerHTML='';
 document.getElementById('fcount').textContent = files.length ? (files.length+L.selSuffix) : '';
 if(!files.length){
  pv.innerHTML='<span style="color:#999;font-size:.85em">'+L.pvEmpty+'</span>';
  return;
 }
 for(const f of files){
  const div=document.createElement('div');
  div.className='item';
  if(f.type.startsWith('image/')){
   const img=document.createElement('img');
   const url=URL.createObjectURL(f);
   img.src=url;
   img.onload=()=>URL.revokeObjectURL(url);
   img.onerror=()=>URL.revokeObjectURL(url);
   div.appendChild(img);
  } else {
   const ph=document.createElement('div');
   ph.textContent=L.unkFmt;
   ph.style.cssText='width:160px;height:110px;display:flex;align-items:center;justify-content:center;border:1px solid #bbb;background:#fff';
   div.appendChild(ph);
  }
  const cap=document.createElement('div');
  cap.className='fname';
  cap.textContent=f.name+' ('+fmtKB(f.size)+')';
  div.appendChild(cap);
  pv.appendChild(div);
 }
}
function clearPreview(){
 document.getElementById('files').value='';
 showPreview();
}

// --- クライアント側リサイズ (PNG で保存) ---
async function resizeToPng(file, orient){
 return new Promise((resolve, reject)=>{
  const url=URL.createObjectURL(file);
  const img=new Image();
  img.onload=()=>{
   URL.revokeObjectURL(url);
   const iw=img.naturalWidth, ih=img.naturalHeight;
   const srcPortrait = (ih > iw);

   let wantPortrait;
   if (orient === 'portrait')      wantPortrait = true;
   else if (orient === 'landscape') wantPortrait = false;
   else                              wantPortrait = srcPortrait;

   const rotate = (wantPortrait !== srcPortrait);

   const maxW = wantPortrait ? MAX_H : MAX_W;
   const maxH = wantPortrait ? MAX_W : MAX_H;

   const effW = rotate ? ih : iw;
   const effH = rotate ? iw : ih;
   const scale = Math.min(maxW/effW, maxH/effH, 1.0);
   const tw = Math.max(1, Math.round(effW * scale));
   const th = Math.max(1, Math.round(effH * scale));

   const c=document.createElement('canvas');
   c.width=tw; c.height=th;
   const ctx=c.getContext('2d');
   ctx.imageSmoothingEnabled=true;
   ctx.imageSmoothingQuality='high';

   if (rotate) {
    ctx.translate(tw/2, th/2);
    ctx.rotate(Math.PI/2);
    const drawW = iw * scale;
    const drawH = ih * scale;
    ctx.drawImage(img, -drawW/2, -drawH/2, drawW, drawH);
   } else {
    ctx.drawImage(img, 0, 0, tw, th);
   }

   c.toBlob((blob)=>{
    if(!blob){ reject(new Error('toBlob failed')); return; }
    resolve({blob, iw, ih, tw, th, scale, rotated: rotate, wantPortrait});
   }, 'image/png');
  };
  img.onerror=()=>{ URL.revokeObjectURL(url); reject(new Error('image decode failed')); };
  img.src=url;
 });
}

async function upload(){
 const files=document.getElementById('files').files;
 if(!files.length){log(L.noFile);return;}
 const doResize=document.getElementById('resize').checked;
 const orient = document.getElementById('orient').value;
 log('uploading '+files.length+' file(s) (orient='+orient+') ...');
 for(let f of files){
  let out=f, note='';
  if(doResize){
   try{
    const r=await resizeToPng(f, orient);
    const base = f.name.replace(/\.(png|jpg|jpeg|heic|webp|gif|bmp)$/i,'');
    out = new File([r.blob], base+'.png', {type:'image/png'});
    const rotNote = r.rotated ? ' +90\u00b0' : '';
    if(r.scale<1){
     note=' ('+r.iw+'x'+r.ih+rotNote+' \u2192 '+r.tw+'x'+r.th+', '+fmtKB(f.size)+' \u2192 '+fmtKB(out.size)+')';
    } else {
     note=' (no resize: '+r.iw+'x'+r.ih+rotNote+', '+fmtKB(f.size)+' \u2192 '+fmtKB(out.size)+')';
    }
   }catch(e){ addlog('resize err '+f.name+': '+e.message+' \u2014 '+L.fallback); out=f; }
  }
  addlog('\u2192 '+out.name+note);
  const fd=new FormData(); fd.append('file',out,out.name);
  const r=await fetch('/api/upload?path='+q(cwd),{method:'POST',body:fd});
  if(!r.ok){ addlog('upload fail: '+out.name+' ('+r.status+')'); break; }
 }
 addlog('done.');
 clearPreview();
 reload();
}

async function mkdir(){
 const n=document.getElementById('newdir').value.trim(); if(!n)return;
 const r=await fetch('/api/mkdir?path='+q(cwd)+'&name='+q(n),{method:'POST'});
 if(!r.ok)log('mkdir fail');
 document.getElementById('newdir').value='';
 reload();
}
async function del(n,is_dir){
 if(!confirm((is_dir?L.folder:L.file)+L.delConfirm+n+' ?'))return;
 const p=(cwd.endsWith('/')?cwd:cwd+'/')+n;
 const r=await fetch('/api/delete?path='+q(p)+'&is_dir='+(is_dir?1:0),{method:'POST'});
 if(!r.ok)log('delete fail');
 reload();
}
async function mv(n){
 const src=(cwd.endsWith('/')?cwd:cwd+'/')+n;
 const dst=prompt(L.mvPrompt,src);
 if(!dst||dst===src)return;
 const r=await fetch('/api/move?src='+q(src)+'&dst='+q(dst),{method:'POST'});
 if(!r.ok)log('move fail');
 reload();
}
reload();
</script></body></html>
)HTML";

// Web UI 言語 JSON (handleRoot で <script> 前に注入)
static String getWebLangScript() {
    String s = F("<script>const L=");
    if (s_lang == LANG_EN) {
        s += F("{"
            "up:'\u2b06 Up',"
            "rl:'\u21bb Reload',"
            "resize:'Resize to screen (960\\u00d7540) and save as PNG',"
            "orient:'Orient: ',"
            "oAuto:'Auto (match source)',"
            "oLand:'Landscape (960\\u00d7540)',"
            "oPort:'Portrait (540\\u00d7960)',"
            "choose:'Choose files',"
            "pvEmpty:'(Selected image preview appears here)',"
            "upload:'\u2b06 Upload',"
            "newdir:'New folder name',"
            "mkdir:'+ Create folder',"
            "thType:'Type',thName:'Name',thSize:'Size',thAct:'Action',"
            "del:'Delete',mv:'Move/Rename',"
            "selSuffix:' selected',"
            "unkFmt:'(unknown format)',"
            "noFile:'No file selected',"
            "fallback:'sending original',"
            "folder:'Folder',file:'File',"
            "delConfirm:' delete: ',"
            "mvPrompt:'Destination full path (or rename)'"
            "}");
    } else {
        s += F("{"
            "up:'\u2b06 \u4e0a\u306e\u968e\u5c64\u3078',"
            "rl:'\u21bb \u518d\u8aad\u307f\u8fbc\u307f',"
            "resize:'\u753b\u9762\u30b5\u30a4\u30ba(960\\u00d7540)\u306b\u7e2e\u5c0f\u3057\u3066PNG\u4fdd\u5b58\u3057\u307e\u3059',"
            "orient:'\u5411\u304d: ',"
            "oAuto:'\u81ea\u52d5 (\u5143\u753b\u50cf\u306b\u5408\u308f\u305b\u308b)',"
            "oLand:'\u6a2a (960\\u00d7540)',"
            "oPort:'\u7e26 (540\\u00d7960)',"
            "choose:'\u30d5\u30a1\u30a4\u30eb\u3092\u9078\u629e',"
            "pvEmpty:'(\u3053\u3053\u306b\u9078\u629e\u3057\u305f\u753b\u50cf\u306e\u30d7\u30ec\u30d3\u30e5\u30fc\u304c\u51fa\u307e\u3059)',"
            "upload:'\u2b06 \u30a2\u30c3\u30d7\u30ed\u30fc\u30c9',"
            "newdir:'\u65b0\u898f\u30d5\u30a9\u30eb\u30c0\u540d',"
            "mkdir:'+ \u30d5\u30a9\u30eb\u30c0\u4f5c\u6210',"
            "thType:'\u7a2e\u5225',thName:'\u540d\u524d',thSize:'\u30b5\u30a4\u30ba',thAct:'\u64cd\u4f5c',"
            "del:'\u524a\u9664',mv:'\u79fb\u52d5/\u6539\u540d',"
            "selSuffix:' \u4ef6\u9078\u629e\u4e2d',"
            "unkFmt:'(\u4e0d\u660e\u306a\u5f62\u5f0f)',"
            "noFile:'\u30d5\u30a1\u30a4\u30eb\u672a\u9078\u629e',"
            "fallback:'\u539f\u672c\u3092\u9001\u4fe1',"
            "folder:'\u30d5\u30a9\u30eb\u30c0',file:'\u30d5\u30a1\u30a4\u30eb',"
            "delConfirm:'\u524a\u9664: ',"
            "mvPrompt:'\u79fb\u52d5\u5148\u30d5\u30eb\u30d1\u30b9\uff08\u307e\u305f\u306f\u6539\u540d\uff09'"
            "}");
    }
    s += F(";</script>\n");
    return s;
}

static void handleRoot() {
    String langScript = getWebLangScript();
    g_http.setContentLength(CONTENT_LENGTH_UNKNOWN);
    g_http.send(200, "text/html; charset=utf-8", "");
    g_http.sendContent(langScript);
    g_http.sendContent_P(HTML_INDEX);
    g_http.sendContent("");  // end chunked
}

static void handleApiList() {
    String path = g_http.arg("path");
    if (path.length() == 0) path = "/";
    File d = SD.open(path);
    if (!d || !d.isDirectory()) {
        g_http.send(404, "application/json", "{\"ok\":false,\"error\":\"not a directory\"}");
        return;
    }
    std::vector<std::pair<std::string, std::pair<bool, uint32_t>>> entries;
    File e;
    while ((e = d.openNextFile())) {
        std::string name = e.name();
        size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        bool is_dir = e.isDirectory();
        uint32_t sz = is_dir ? 0 : (uint32_t)e.size();
        e.close();
        if (name.empty() || name[0] == '.') continue;
        entries.push_back({name, {is_dir, sz}});
    }
    d.close();
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b){
        if (a.second.first != b.second.first) return a.second.first > b.second.first; // dirs first
        return a.first < b.first;
    });
    std::string out = "{\"ok\":true,\"path\":\"";
    jsonEscape(path.c_str(), out);
    out += "\",\"items\":[";
    bool first = true;
    for (auto& it : entries) {
        if (!first) out += ",";
        first = false;
        out += "{\"name\":\"";
        jsonEscape(it.first, out);
        out += "\",\"is_dir\":";
        out += (it.second.first ? "true" : "false");
        out += ",\"size\":";
        char nb[16]; snprintf(nb, sizeof(nb), "%lu", (unsigned long)it.second.second);
        out += nb;
        out += ",\"kind\":\"";
        if (it.second.first) out += "DIR";
        else if (endsWithCI(it.first, ".png")) out += "PNG";
        else if (endsWithCI(it.first, ".jpg") || endsWithCI(it.first, ".jpeg")) out += "JPG";
        else out += "FILE";
        out += "\"}";
    }
    out += "]}";
    g_http.send(200, "application/json", out.c_str());
}

static void handleApiUploadDone() {
    g_http.send(200, "application/json", "{\"ok\":true}");
}

static void handleApiUploadChunk() {
    HTTPUpload& u = g_http.upload();
    if (u.status == UPLOAD_FILE_START) {
        String dir = g_http.arg("path");
        if (dir.length() == 0) dir = "/";
        String full = dir;
        if (!full.endsWith("/")) full += "/";
        full += u.filename;
        Serial.printf("HTTP upload start: %s\n", full.c_str());
        if (SD.exists(full)) SD.remove(full);
        g_up_file = SD.open(full, FILE_WRITE);
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (g_up_file) g_up_file.write(u.buf, u.currentSize);
    } else if (u.status == UPLOAD_FILE_END) {
        if (g_up_file) g_up_file.close();
        Serial.printf("HTTP upload end: %u bytes\n", (unsigned)u.totalSize);
    } else if (u.status == UPLOAD_FILE_ABORTED) {
        if (g_up_file) g_up_file.close();
        Serial.println("HTTP upload aborted");
    }
}

static void handleApiMkdir() {
    String dir  = g_http.arg("path");
    String name = g_http.arg("name");
    if (name.length() == 0) { g_http.send(400, "application/json", "{\"ok\":false}"); return; }
    String full = dir;
    if (!full.endsWith("/")) full += "/";
    full += name;
    bool ok = SD.mkdir(full);
    Serial.printf("mkdir %s -> %d\n", full.c_str(), ok);
    g_http.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handleApiDelete() {
    String path   = g_http.arg("path");
    bool   is_dir = (g_http.arg("is_dir") == "1");
    bool ok = is_dir ? SD.rmdir(path) : SD.remove(path);
    Serial.printf("delete %s (dir=%d) -> %d\n", path.c_str(), is_dir, ok);
    g_http.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handleApiMove() {
    String src = g_http.arg("src");
    String dst = g_http.arg("dst");
    if (src.length() == 0 || dst.length() == 0) {
        g_http.send(400, "application/json", "{\"ok\":false}");
        return;
    }
    bool ok = SD.rename(src, dst);
    Serial.printf("rename %s -> %s : %d\n", src.c_str(), dst.c_str(), ok);
    g_http.send(ok ? 200 : 500, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// ---- 画面: QR + SSID/PW/URL + 終了ボタン ----
static void drawCommScreen(const char* ssid, const char* pass, const char* url) {
    auto& d = M5.Display;
    d.setEpdMode(epd_mode_t::epd_quality);
    d.fillScreen(TFT_WHITE);
    const int W = d.width();
    const int H = d.height();

    // ヘッダ
    d.setTextColor(TFT_BLACK, TFT_WHITE);
    d.setFont(&fonts::efontJA_24_b);
    d.setTextDatum(middle_center);
    d.drawString(S().comm_title, W / 2, 22);

    // 説明文 (2 行)
    d.setFont(&fonts::efontJA_24);
    d.drawString(S().comm_desc1, W / 2, 52);
    d.drawString(S().comm_desc2, W / 2, 80);
    d.drawFastHLine(0, 100, W, TFT_BLACK);

    const int qr_size = 220;
    const int qr_y    = 110;
    const int left_qr_x  = 60;
    const int right_qr_x = W - 60 - qr_size;

    // 左: Wi-Fi 接続 QR (WIFI: 形式)
    char wifi_qr[128];
    snprintf(wifi_qr, sizeof(wifi_qr), "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
    d.qrcode(wifi_qr, left_qr_x, qr_y, qr_size, 6);
    d.setFont(&fonts::efontJA_24_b);
    d.setTextDatum(top_center);
    d.drawString(S().comm_wifi_step, left_qr_x + qr_size / 2, qr_y + qr_size + 6);
    d.setFont(&fonts::efontJA_24);
    char l1[64], l2[64];
    snprintf(l1, sizeof(l1), "SSID: %s", ssid);
    snprintf(l2, sizeof(l2), "PASS: %s", pass);
    d.drawString(l1, left_qr_x + qr_size / 2, qr_y + qr_size + 38);
    d.drawString(l2, left_qr_x + qr_size / 2, qr_y + qr_size + 66);

    // 右: URL QR
    d.qrcode(url, right_qr_x, qr_y, qr_size, 4);
    d.setFont(&fonts::efontJA_24_b);
    d.drawString(S().comm_browser_step, right_qr_x + qr_size / 2, qr_y + qr_size + 6);
    d.setFont(&fonts::efontJA_24);
    d.drawString(url, right_qr_x + qr_size / 2, qr_y + qr_size + 38);

    // 注意書き (左下 / 終了ボタンの左)
    Rect exb = commExitButtonRect();
    d.setTextDatum(middle_left);
    d.setFont(&fonts::efontJA_24_b);
    d.drawString(S().comm_warn1, 20, H - 54);
    d.setFont(&fonts::efontJA_24);
    d.drawString(S().comm_warn2, 20, H - 22);

    // 終了ボタン
    d.fillRect(exb.x, exb.y, exb.w, exb.h, TFT_BLACK);
    d.drawRect(exb.x - 2, exb.y - 2, exb.w + 4, exb.h + 4, TFT_BLACK);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setFont(&fonts::efontJA_24_b);
    d.setTextDatum(middle_center);
    d.drawString(S().comm_exit, exb.x + exb.w / 2, exb.y + exb.h / 2);

    d.setTextColor(TFT_BLACK, TFT_WHITE);
    d.setTextDatum(top_left);
    d.display();
    d.waitDisplay();
}

// ---- 起動 / 停止 / tick ----
static void startCommMode() {
    Serial.println("Start comm mode");

    // Wi-Fi は 80MHz では正しく動作しないので 240MHz に上げる
    cpuHigh();

    // SSID は MAC 下位 3 バイトで一意化
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(g_ap_ssid, sizeof(g_ap_ssid), "PocketFrame-%02X%02X%02X", mac[3], mac[4], mac[5]);

    WiFi.mode(WIFI_AP);
    bool ok = WiFi.softAP(g_ap_ssid, g_ap_pass);
    Serial.printf("softAP %s -> %d\n", g_ap_ssid, ok);
    delay(200);
    IPAddress ip = WiFi.softAPIP();
    snprintf(g_ap_url, sizeof(g_ap_url), "http://%s/", ip.toString().c_str());
    Serial.printf("AP IP: %s\n", g_ap_url);

    g_http.on("/",            HTTP_GET,  handleRoot);
    g_http.on("/api/list",    HTTP_GET,  handleApiList);
    g_http.on("/api/upload",  HTTP_POST, handleApiUploadDone, handleApiUploadChunk);
    g_http.on("/api/mkdir",   HTTP_POST, handleApiMkdir);
    g_http.on("/api/delete",  HTTP_POST, handleApiDelete);
    g_http.on("/api/move",    HTTP_POST, handleApiMove);
    g_http.onNotFound([](){ g_http.send(404, "text/plain", "not found"); });
    g_http.begin();
    g_comm_running = true;

    drawCommScreen(g_ap_ssid, g_ap_pass, g_ap_url);
}

static void stopCommMode() {
    Serial.println("Stop comm mode");
    if (g_comm_running) {
        g_http.stop();
        g_comm_running = false;
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);

    // 通信終了後は省電力モードへ戻す
    cpuLow();
}

// loop() から呼ばれる
void commHandleClients() {
    if (g_comm_running) g_http.handleClient();
}
