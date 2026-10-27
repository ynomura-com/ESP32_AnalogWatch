// GC9A01 (240x240 円形) + ESP32 MiniKit (MH-ET LIVE) アナログ時計
// 診断ログ付き版 / 帯(ストリップ)スプライトで描画してメモリ不足を回避

#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <sys/time.h>
#include <time.h>
#include <esp_system.h>
#include <esp_heap_caps.h>

// ================= 設定 =================
static const char* WIFI_SSID = "YOUR_SSID";      // 2.4GHz の SSID のみ接続可能
static const char* WIFI_PASS = "YOUR_PASSWORD";
static const char* TZ_JST    = "JST-9";
static const bool  SMOOTH_SECOND = true;         // true: 秒針が滑らか / false: 1秒ごと
static const char* LOGO_TEXT = "ESP";
static const int   NUM_DY = 2;                   // 数字の縦位置の微調整(px)
static const uint32_t HEARTBEAT_MS = 10000;      // 状態ログの出力間隔

// 秒針の 12 時停止(鉄道時計風)。SMOOTH_SECOND = true のときだけ有効
static const bool  STOP_AT_12_RAILWAY = true;    // 鉄道時計風の文字盤
static const bool  STOP_AT_12_BLUE    = false;   // 青い文字盤
static const float SWEEP_SECONDS      = 58.5f;   // 1周にかける秒数(残りの時間は12時で停止)

// ================= ログ =================
#define LOG(tag, fmt, ...) Serial.printf("[%8lu] [" tag "] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)

// ================= 色 =================
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
constexpr uint16_t COL_BG   = 0x0000;
constexpr uint16_t COL_NUM  = rgb565(150, 205, 255);
constexpr uint16_t COL_TICK = rgb565(190, 200, 215);
constexpr uint16_t COL_HAND = rgb565(240, 244, 255);
constexpr uint16_t COL_HUB  = rgb565(40, 100, 230);
constexpr uint16_t COL_SEC  = rgb565(255, 40, 40);    // 秒針(赤)

constexpr int CX = 120;
constexpr int CY = 120;

TFT_eSPI tft;
TFT_eSprite spr(&tft);
static int stripH = 120;   // 帯の高さ(確保できた大きさに応じて自動決定)
static int gYOff  = 0;     // 現在描画中の帯の画面上Y座標

static bool ntpOk = false;
static uint32_t frames = 0;

// ================= ヘルパー(ログ文字列) =================
static const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXTERNAL_PIN";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC (crash)";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (power supply problem)";
    default:                return "OTHER";
  }
}

static const char* wifiStatusStr(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "UNKNOWN";
  }
}

static const char* disconnectReasonStr(uint8_t r) {
  switch (r) {
    case 2:   return "AUTH_EXPIRE";
    case 8:   return "ASSOC_LEAVE";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT (wrong password?)";
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND (wrong SSID / 5GHz only / out of range)";
    case 202: return "AUTH_FAIL (wrong password?)";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    default:  return "OTHER";
  }
}

static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      LOG("WIFI", "event: STA started");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      LOG("WIFI", "event: associated with AP (channel %d)", info.wifi_sta_connected.channel);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      LOG("WIFI", "event: got IP %s", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      LOG("WIFI", "event: disconnected, reason=%d %s",
          info.wifi_sta_disconnected.reason,
          disconnectReasonStr(info.wifi_sta_disconnected.reason));
      break;
    default:
      break;
  }
}

static void logPins() {
#ifdef TFT_BL
  const int bl = TFT_BL;
#else
  const int bl = -1;
#endif
  LOG("TFT", "pins: MOSI=%d SCLK=%d CS=%d DC=%d RST=%d BL=%d, SPI_FREQUENCY=%lu",
      (int)TFT_MOSI, (int)TFT_SCLK, (int)TFT_CS, (int)TFT_DC, (int)TFT_RST, bl,
      (unsigned long)SPI_FREQUENCY);
}

// ================= 画面への状態表示 =================
static void showStatus(const char* l1, const char* l2 = nullptr) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(COL_NUM, TFT_BLACK);
  tft.drawCentreString(l1, CX, l2 ? 100 : 112, 2);
  if (l2) tft.drawCentreString(l2, CX, 124, 2);
}

// 色ベタ塗りテスト(WiFi・スプライトと無関係に表示系だけ確認する)
static void displayTest() {
  const uint16_t cols[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
  const char* names[]   = {"RED", "GREEN", "BLUE", "WHITE"};
  for (int i = 0; i < 4; i++) {
    LOG("TFT", "color test: fill %s", names[i]);
    tft.fillScreen(cols[i]);
    delay(500);
  }
  tft.fillScreen(TFT_BLACK);
  LOG("TFT", "color test done (if the panel stayed black, check wiring/power/BL/RST)");
}

// ================= 描画 =================
// 画面座標Yを現在の帯内の座標へ変換
static inline int Y(int y) { return y - gYOff; }

// 角度(度, 12時=0°, 時計回り)と半径 → 帯内座標
static inline void polar(float deg, float r, float &x, float &y) {
  float a = deg * DEG_TO_RAD;
  x = CX + r * sinf(a);
  y = CY - r * cosf(a) - gYOff;
}

// ESP32-C3 には FPU(浮動小数点演算器)が無く、float 演算がソフトウェア実装で非常に遅い。
// TFT_eSPI の drawWedgeLine / fillSmoothCircle は 1 ピクセルごとに float を多用するため、
// 針の描画だけでフレーム時間の大半を使ってしまう。
// そこで、動く針(毎フレーム描く部分)だけ整数演算(固定小数点)で描く関数に置き換える。
// 仕様は drawWedgeLine と同じ: 両端が丸い太さ可変の線(ra/rb は端の半径)。
// 先端と根元が同じ点なら円になる。座標は現在の帯(スプライト)内の座標。
// ---- 整数(固定小数点 Q8)で描く高速アンチエイリアス付き線 ----
static inline uint16_t blend565(uint16_t bg, uint16_t fg, uint32_t a) {   // a: 0..256
  uint32_t inv = 256 - a;
  uint32_t r = (((bg >> 11) & 0x1F) * inv + ((fg >> 11) & 0x1F) * a) >> 8;
  uint32_t g = (((bg >> 5)  & 0x3F) * inv + ((fg >> 5)  & 0x3F) * a) >> 8;
  uint32_t b = (( bg        & 0x1F) * inv + ( fg        & 0x1F) * a) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void fastWedge(float ax, float ay, float bx, float by, float ra, float rb, uint16_t color) {
  uint16_t* buf = (uint16_t*)spr.getPointer();
  if (!buf) return;
  const int W = 240, H = stripH;

  const float maxr = ra > rb ? ra : rb;
  int x0 = (int)floorf((ax < bx ? ax : bx) - maxr - 1.0f);
  int x1 = (int)ceilf ((ax > bx ? ax : bx) + maxr + 1.0f);
  int y0 = (int)floorf((ay < by ? ay : by) - maxr - 1.0f);
  int y1 = (int)ceilf ((ay > by ? ay : by) + maxr + 1.0f);
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > W - 1) x1 = W - 1;
  if (y1 > H - 1) y1 = H - 1;
  if (x0 > x1 || y0 > y1) return;

  const int32_t Ax = (int32_t)lroundf(ax * 256.0f), Ay = (int32_t)lroundf(ay * 256.0f);
  const int32_t Bx = (int32_t)lroundf(bx * 256.0f), By = (int32_t)lroundf(by * 256.0f);
  const int32_t Ra = (int32_t)lroundf(ra * 256.0f), Rb = (int32_t)lroundf(rb * 256.0f);
  const int32_t bax = Bx - Ax, bay = By - Ay, dR = Rb - Ra;
  const int64_t ba2 = (int64_t)bax * bax + (int64_t)bay * bay;                  // Q16
  const int64_t inv = ba2 > 0 ? (((int64_t)1 << 46) / ba2) : 0;
  const uint16_t cs = __builtin_bswap16(color);                                 // スプライトはバイト入替で保持

  for (int y = y0; y <= y1; y++) {
    const int32_t pay = y * 256 - Ay;
    uint16_t* row = buf + y * W;
    for (int x = x0; x <= x1; x++) {
      const int32_t pax = x * 256 - Ax;
      const int64_t dot = (int64_t)pax * bax + (int64_t)pay * bay;              // Q16
      int32_t h = 0;                                                            // Q16: 0..65536
      if (dot > 0) {
        int64_t hh = (dot * inv) >> 30;
        h = hh > 65536 ? 65536 : (int32_t)hh;
      }
      const int32_t dx = pax - (int32_t)(((int64_t)bax * h) >> 16);
      const int32_t dy = pay - (int32_t)(((int64_t)bay * h) >> 16);
      const int32_t r  = Ra + (int32_t)(((int64_t)dR * h) >> 16);               // Q8
      const int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy;                   // Q16

      const int32_t ro = r + 128;
      if (d2 >= (int64_t)ro * ro) continue;                                     // 完全に外側
      const int32_t ri = r - 128;
      if (ri > 0 && d2 <= (int64_t)ri * ri) { row[x] = cs; continue; }          // 完全に内側
      // 境界のピクセルだけ距離を計算してアンチエイリアス
      const int32_t d = (int32_t)sqrtf((float)d2);                              // Q8
      int32_t a = r + 128 - d;
      if (a <= 0) continue;
      if (a > 256) a = 256;
      row[x] = __builtin_bswap16(blend565(__builtin_bswap16(row[x]), color, (uint32_t)a));
    }
  }
}

// 秒針の角度(度)。stopAt12 が true なら SWEEP_SECONDS で1周し、12時で次の分まで停止する
static float secondHandDeg(float second, bool stopAt12) {
  if (!stopAt12 || !SMOOTH_SECOND) return second * 6.0f;
  if (second >= SWEEP_SECONDS) return 360.0f;          // 12時で停止
  return second * (360.0f / SWEEP_SECONDS);
}

static void drawFaceBlue() {
  spr.fillSprite(COL_BG);
  float x0, y0, x1, y1;

  // --- 目盛り ---
  for (int i = 0; i < 60; i++) {
    float deg = i * 6.0f;
    if (i % 5 == 0) {
      if (i == 0) continue;
      polar(deg, 87, x0, y0);
      polar(deg, 98, x1, y1);
      spr.drawWedgeLine(x0, y0, x1, y1, 1.75f, 1.75f, COL_NUM, COL_BG);   // 半径 = 幅3.5px の半分
    } else {
      polar(deg, 94, x0, y0);
      spr.fillCircle((int)roundf(x0), (int)roundf(y0), 1, COL_TICK);
    }
  }

  // 12時位置の▼マーク
  spr.fillTriangle(CX - 6, Y(CY - 100), CX + 6, Y(CY - 100), CX, Y(CY - 88), COL_NUM);

  spr.setTextColor(COL_NUM);
  spr.setTextDatum(MC_DATUM);

  // --- 時の数字 ---
  spr.setFreeFont(&FreeSansBold18pt7b);
  for (int h = 1; h <= 12; h++) {
    polar(h * 30.0f, 66, x0, y0);
    spr.drawNumber(h, (int)roundf(x0), (int)roundf(y0) + NUM_DY);
  }

  // --- 外周の分数字 ---
  spr.setFreeFont(&FreeSans9pt7b);
  for (int m = 5; m <= 60; m += 5) {
    polar(m * 6.0f, 108, x0, y0);
    spr.drawNumber(m, (int)roundf(x0), (int)roundf(y0) + 1);
  }

  // --- ロゴ枠 ---
  spr.setFreeFont(NULL);
  spr.setTextFont(1);
  spr.setTextSize(1);
  spr.drawRoundRect(CX - 15, Y(CY + 28), 30, 15, 3, COL_NUM);
  spr.drawString(LOGO_TEXT, CX, Y(CY + 36));
}

static void drawHand(float deg, float len, float tail, float wBase, float wTip, uint16_t col) {
  float x0, y0, x1, y1;
  polar(deg + 180.0f, tail, x0, y0);
  polar(deg, len, x1, y1);
  // drawWedgeLine の太さ引数は半径なので、幅の半分を渡す
  fastWedge(x0, y0, x1, y1, wBase * 0.5f, wTip * 0.5f, col);
}

static void drawHandsBlue(int hour, int minute, float second) {

  float hDeg = ((hour % 12) + minute / 60.0f + second / 3600.0f) * 30.0f;
  float mDeg = (minute + second / 60.0f) * 6.0f;
  float sDeg = secondHandDeg(second, STOP_AT_12_BLUE);

  // 時針・分針: 先端が針のように尖る(先端幅 TIP_W を小さくするほど鋭くなる)
  const float TIP_W = 0.8f;
  drawHand(hDeg, 52, 10, 9.0f, TIP_W, COL_HAND);
  drawHand(mDeg, 86, 12, 7.0f, TIP_W, COL_HAND);

  // 秒針: 赤い細線 + 先端に小さな赤い円
  const float SEC_TIP_R = 90.0f;   // 円の中心の位置(中心からの距離)
  drawHand(sDeg, SEC_TIP_R, 26, 2.0f, 2.0f, COL_SEC);
  float sx, sy;
  polar(sDeg, SEC_TIP_R, sx, sy);
  fastWedge(sx, sy, sx, sy, 4.0f, 4.0f, COL_SEC);

  fastWedge(CX, Y(CY), CX, Y(CY), 6.0f, 6.0f, COL_HUB);
  spr.fillCircle(CX, Y(CY), 1, COL_HAND);
}

// ====================================================================
//  Face 1: 鉄道時計風(白文字盤・黒い太バー・黒の太い針・赤い秒針)
// ====================================================================
#ifndef FACE_STYLE
#define FACE_STYLE 0          // 0: ブルー文字盤 / 1: 鉄道時計風 (platformio.ini の env で切り替え)
#endif
constexpr int FACE = FACE_STYLE;

constexpr uint16_t RW_WHITE = 0xFFFF;
constexpr uint16_t RW_BLACK = 0x0000;
constexpr uint16_t RW_RED   = rgb565(235, 25, 25);

// 針の向きに沿った長方形(r0〜r1, 幅w)を塗りつぶす
static void fillBarRect(float deg, float r0, float r1, float w, uint16_t col) {
  float a = deg * DEG_TO_RAD;
  float dx = sinf(a), dy = -cosf(a);   // 針の向き(画面座標)
  float px = -dy,     py = dx;         // 垂直方向
  float hw = w * 0.5f;
  const float rr[4] = {r0, r0, r1, r1};
  const float ss[4] = {hw, -hw, -hw, hw};
  int x[4], y[4];
  for (int i = 0; i < 4; i++) {
    x[i] = (int)roundf(CX + dx * rr[i] + px * ss[i]);
    y[i] = (int)roundf(CY + dy * rr[i] + py * ss[i]) - gYOff;
  }
  spr.fillTriangle(x[0], y[0], x[1], y[1], x[2], y[2], col);
  spr.fillTriangle(x[0], y[0], x[2], y[2], x[3], y[3], col);
}

// 端が平らなバー。側面はアンチエイリアス、両端は長方形で平らに仕上げる
// r0 に負の値を指定すると中心より反対側(針の尾)まで伸びる
static void drawBar(float deg, float r0, float r1, float w, uint16_t fg, uint16_t bg) {
  float hw = w * 0.5f;
  float ax, ay, bx, by;
  polar(deg, r0 + hw, ax, ay);
  polar(deg, r1 - hw, bx, by);
  // 注意: drawWedgeLine の太さ引数は「半径」(幅の半分)
  (void)bg;
  fastWedge(ax, ay, bx, by, hw, hw, fg);
  fillBarRect(deg, r0, r0 + hw, w, fg);
  fillBarRect(deg, r1 - hw, r1, w, fg);
}

static void drawFaceRailway() {
  spr.fillSprite(RW_WHITE);
  for (int i = 0; i < 60; i++) {
    float deg = i * 6.0f;
    if (i % 5 == 0) {
      drawBar(deg, 80, 114, 7.0f, RW_BLACK, RW_WHITE);    // 時の太いバー
    } else {
      drawBar(deg, 105, 114, 2.0f, RW_BLACK, RW_WHITE);   // 分の細い目盛り
    }
  }
}

static void drawHandsRailway(int hour, int minute, float second) {

  float hDeg = ((hour % 12) + minute / 60.0f + second / 3600.0f) * 30.0f;
  float mDeg = (minute + second / 60.0f) * 6.0f;
  float sDeg = secondHandDeg(second, STOP_AT_12_RAILWAY);

  // 時針・分針: 太い黒の棒(短い尾付き)
  drawBar(hDeg, -20, 74, 10.0f, RW_BLACK, RW_WHITE);
  drawBar(mDeg, -22, 106, 7.0f, RW_BLACK, RW_WHITE);

  // 秒針: 赤い細線(尾付き)+ 先端の赤い大きな円
  const float SEC_DISC_POS = 84.0f;   // 円の中心位置(中心からの距離)
  const int   SEC_DISC_R   = 9;       // 円の半径
  const float SEC_TAIL     = 34.0f;   // 反対側の尾の長さ
  float ax, ay, bx, by;
  polar(sDeg + 180.0f, SEC_TAIL, ax, ay);
  polar(sDeg, SEC_DISC_POS, bx, by);
  fastWedge(ax, ay, bx, by, 1.25f, 1.25f, RW_RED);   // 半径1.25 = 幅2.5px
  fastWedge(bx, by, bx, by, (float)SEC_DISC_R, (float)SEC_DISC_R, RW_RED);

  // 中心: 黒いハブ + 赤い芯
  fastWedge(CX, Y(CY), CX, Y(CY), 6.0f, 6.0f, RW_BLACK);
  fastWedge(CX, Y(CY), CX, Y(CY), 3.0f, 3.0f, RW_RED);
}

// 文字盤・針の切り替え
static void drawFace() {
  if (FACE == 1) drawFaceRailway();
  else           drawFaceBlue();
}

static void drawHands(int hour, int minute, float second) {
  if (FACE == 1) drawHandsRailway(hour, minute, second);
  else           drawHandsBlue(hour, minute, second);
}

// ---- 文字盤キャッシュ ----------------------------------------------------
// 動かない文字盤(目盛り・数字)は最初に1回だけ描いて RAM に保存し、
// 毎フレームは「コピー + 針だけ描画」にする(描画時間が大幅に減る)。
// 帯ごとに確保するので、メモリが足りない帯だけ従来どおり毎回描画する。
static uint16_t* faceCache[8] = {nullptr};

static void buildFaceCache() {
  const size_t stripBytes = (size_t)240 * stripH * 2;
  const int n = 240 / stripH;
  int ok = 0;
  for (int i = 0; i < n && i < 8; i++) {
    uint16_t* buf = (uint16_t*)heap_caps_malloc(stripBytes, MALLOC_CAP_8BIT);
    if (!buf) {
      LOG("CACHE", "strip %d/%d: alloc FAILED (%u bytes), will redraw live", i + 1, n, (unsigned)stripBytes);
      continue;
    }
    gYOff = i * stripH;
    drawFace();
    memcpy(buf, spr.getPointer(), stripBytes);
    faceCache[i] = buf;
    ok++;
  }
  LOG("CACHE", "face cache: %d/%d strips cached, free heap=%lu, largest block=%lu",
      ok, n, (unsigned long)ESP.getFreeHeap(),
      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// ================= 初期化処理 =================
static bool createStripSprite() {
  spr.setColorDepth(16);
  const int heights[] = {120, 80, 60, 40, 30};   // 240 を割り切れる高さ
  for (int h : heights) {
    LOG("SPR", "trying 240x%d (%d bytes), largest free block=%lu",
        h, 240 * h * 2, (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (spr.createSprite(240, h)) {
      stripH = h;
      LOG("SPR", "OK: strip height=%d, passes per frame=%d, free heap=%lu",
          stripH, 240 / stripH, (unsigned long)ESP.getFreeHeap());
      return true;
    }
    LOG("SPR", "createSprite FAILED for 240x%d", h);
  }
  return false;
}

static void connectWiFi() {
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  LOG("WIFI", "scanning networks...");
  int n = WiFi.scanNetworks();
  bool found = false;
  if (n <= 0) {
    LOG("WIFI", "scan result: no networks found (n=%d)", n);
  } else {
    LOG("WIFI", "scan result: %d network(s)", n);
    for (int i = 0; i < n; i++) {
      bool match = (WiFi.SSID(i) == WIFI_SSID);
      if (match) found = true;
      LOG("WIFI", "  %2d: %-32s ch%-2d %4d dBm %s%s", i + 1, WiFi.SSID(i).c_str(),
          WiFi.channel(i), WiFi.RSSI(i),
          WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured",
          match ? "  <== target" : "");
    }
  }
  WiFi.scanDelete();
  if (!found) {
    LOG("WIFI", "WARNING: target SSID \"%s\" not found (ESP32 supports 2.4GHz only)", WIFI_SSID);
  }

  LOG("WIFI", "connecting to \"%s\" ...", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#ifdef WIFI_LOW_TX_POWER
  // 一部の ESP32-C3 Super Mini はアンテナ特性の都合で送信出力を下げると安定する
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  LOG("WIFI", "TX power limited to 8.5 dBm");
#endif

  const uint32_t timeoutMs = 20000;
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(1000);
    LOG("WIFI", "waiting... status=%s (%lus)", wifiStatusStr(WiFi.status()),
        (unsigned long)((millis() - t0) / 1000));
  }

  if (WiFi.status() == WL_CONNECTED) {
    LOG("WIFI", "CONNECTED: IP=%s GW=%s RSSI=%d dBm ch=%d",
        WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
        WiFi.RSSI(), WiFi.channel());
  } else {
    LOG("WIFI", "connect FAILED (status=%s). Will keep retrying in background.",
        wifiStatusStr(WiFi.status()));
  }
}

static void syncTime() {
  LOG("NTP", "configTzTime(%s) ntp.nict.jp / time.google.com", TZ_JST);
  configTzTime(TZ_JST, "ntp.nict.jp", "time.google.com");
  for (int i = 1; i <= 15; i++) {
    time_t now = time(nullptr);
    if (now > 1700000000) {
      struct tm t;
      localtime_r(&now, &t);
      char buf[48];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &t);
      LOG("NTP", "synced: %s", buf);
      ntpOk = true;
      return;
    }
    LOG("NTP", "waiting for sync (%d/15)", i);
    delay(1000);
  }
  LOG("NTP", "NOT synced yet. Clock will show wrong time until NTP succeeds.");
}

// ================= setup / loop =================
void setup() {
  Serial.begin(115200);
  // USB CDC(ESP32-C3/S3 のネイティブUSB)ではPC側が接続するまで待つ(最大3秒)
  for (uint32_t t0 = millis(); !Serial && millis() - t0 < 3000;) delay(10);
  delay(1000);
  Serial.println();
  Serial.println("============ Analog Clock boot ============");

  LOG("BOOT", "reset reason: %s", resetReasonStr(esp_reset_reason()));
  LOG("BOOT", "chip: %s rev%d, %d core(s), %lu MHz",
      ESP.getChipModel(), (int)ESP.getChipRevision(), (int)ESP.getChipCores(),
      (unsigned long)ESP.getCpuFreqMHz());
  LOG("BOOT", "flash: %lu KB, SDK: %s", (unsigned long)(ESP.getFlashChipSize() / 1024), ESP.getSdkVersion());
  LOG("BOOT", "heap free=%lu, largest block=%lu, PSRAM=%lu",
      (unsigned long)ESP.getFreeHeap(),
      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
      (unsigned long)ESP.getPsramSize());

  LOG("BOOT", "watch face: %s", FACE == 1 ? "railway (white)" : "blue (digits)");

  // --- TFT ---
  logPins();
  LOG("TFT", "tft.init() ...");
  tft.init();
  tft.setRotation(0);
  LOG("TFT", "tft.init() done (%dx%d)", tft.width(), tft.height());
  displayTest();

  // --- スプライト(WiFi より先に確保) ---
  showStatus("Allocating buffer...");
  if (!createStripSprite()) {
    LOG("SPR", "FATAL: could not allocate any sprite. Halting.");
    showStatus("Sprite alloc failed", "see serial log");
    while (true) {
      LOG("SPR", "halted (heap free=%lu, largest=%lu)", (unsigned long)ESP.getFreeHeap(),
          (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      delay(5000);
    }
  }

  // --- 文字盤キャッシュ(WiFi より先に確保) ---
  buildFaceCache();

  // --- WiFi / NTP ---
  showStatus("Connecting WiFi...", WIFI_SSID);
  connectWiFi();

  showStatus("Syncing time...");
  syncTime();

  LOG("RUN", "entering main loop");
}

void loop() {
  static int lastSec = -1;
  static uint32_t lastBeat = 0;
  static uint32_t beatFrames = 0;
  static uint32_t accRenderUs = 0, accPushUs = 0;   // 1フレームあたりの描画/転送時間の集計

  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm t;
  localtime_r(&tv.tv_sec, &t);

  if (!SMOOTH_SECOND && t.tm_sec == lastSec) {
    delay(5);
  } else {
    lastSec = t.tm_sec;
    float sec = t.tm_sec + (SMOOTH_SECOND ? tv.tv_usec / 1e6f : 0.0f);
    for (int y = 0; y < 240; y += stripH) {
      gYOff = y;
      uint32_t tA = micros();
      uint16_t* cache = faceCache[y / stripH];
      if (cache) memcpy(spr.getPointer(), cache, (size_t)240 * stripH * 2);
      else       drawFace();
      drawHands(t.tm_hour, t.tm_min, sec);
      uint32_t tB = micros();
      spr.pushSprite(0, y);
      uint32_t tC = micros();
      accRenderUs += tB - tA;
      accPushUs   += tC - tB;
    }
    frames++;
    beatFrames++;
  }

  // --- 定期ステータスログ ---
  uint32_t now = millis();
  if (now - lastBeat >= HEARTBEAT_MS) {
    float fps = beatFrames * 1000.0f / (now - lastBeat);
    if (!ntpOk && time(nullptr) > 1700000000) {
      ntpOk = true;
      LOG("NTP", "synced (late)");
    }
    wl_status_t ws = WiFi.status();
    float renderMs = beatFrames ? accRenderUs / 1000.0f / beatFrames : 0.0f;
    float pushMs   = beatFrames ? accPushUs   / 1000.0f / beatFrames : 0.0f;
    LOG("RUN", "%02d:%02d:%02d | WiFi=%s RSSI=%d | NTP=%s | heap=%lu | %.1f fps (render %.1f ms + push %.1f ms) | frames=%lu",
        t.tm_hour, t.tm_min, t.tm_sec, wifiStatusStr(ws),
        ws == WL_CONNECTED ? WiFi.RSSI() : 0, ntpOk ? "OK" : "NG",
        (unsigned long)ESP.getFreeHeap(), fps, renderMs, pushMs, (unsigned long)frames);
    accRenderUs = 0;
    accPushUs = 0;
    lastBeat = now;
    beatFrames = 0;
  }
}
