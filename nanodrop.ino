/*
 * DIY Nanodrop (UV) - ESP32 + 2.8" ST7789 LCD  [TOUCH UI + WIFI/CSV]
 *
 * 対象ボード: ideaspark ESP32 2.8" IPS LCD タッチボード (240x320, ST7789, XPT2046)
 *   https://www.amazon.co.jp/dp/B0HHSHFZ8Q
 *
 * 265nm + 280nm UVC LED を切替駆動し、UVセンサで検出。
 * 吸光度 A = -log10(I/I0) と純度比 A260/A280 を算出。
 *
 * ---- センサ選択 (下の SENSOR_AS7331 で切替) ----
 *   SENSOR_AS7331=0 : アナログUVフォトダイオード + TIA  ← 既定
 *                     SG01S-C18 (SiC, sglux) を TIA で電圧に変換 → ADS1115 でAD変換
 *     PD_USE_ADS1115=1 : 外付けADC ADS1115 (16-bit I2C) を使用【推奨】
 *     PD_USE_ADS1115=0 : ESP32 内蔵ADC (GPIO33) を使用（簡易・精度低）
 *     ※ GUVA-S12SD 等の他のアナログUV素子でも同じ経路が使える。
 *   SENSOR_AS7331=1 : AS7331 (I2C デジタル 3ch)  ← 入手できれば選択可
 *
 *   ※ 吸光度は比 I/I0 なので TIA ゲインは式上キャンセルされ、絶対校正は不要。
 *     ただし TIA 出力は ADS1115 のフルスケール(±4.096V)内で正に振れること。
 *   ※ ESP32 内蔵ADCはノイズ・非直線性が大きいため外付け ADS1115 を推奨。
 *
 * ---- 画面構成 ----
 *   ホーム: [Calibration] [Measuring] [Settings] の大きなボタン + Download data
 *   Calibration: EEPROM の校正データ有無を確認 → 無ければ案内付きで校正開始
 *   Measuring  : Blank → Sample を毎回ペアで測定し、結果をメモリ保持
 *   Settings   : 画面の明るさ / WiFi 設定(SSID一覧→WPS) / WiFi初期化 / タッチ校正
 *
 * ---- WiFi ----
 *   記憶した SSID/pass があれば STA 接続。無ければ AP モード:
 *     SSID=mynanodrop / pass=12345678 / IP=192.168.5.1
 *   Web サーバを常時起動: / に案内、 /data.csv で測定データを CSV ダウンロード。
 *
 * ---- ピン ----
 *   LCD  : CS=15 DC=2 RST=4 BL=32 / VSPI SCK=18 MISO=19 MOSI=23
 *   Touch: CS=14 IRQ=27
 *   LED_265=GPIO16  LED_280=GPIO17
 *   SG01S-C18 + TIA -> ADS1115 AIN0
 *   AS7331 / ADS1115: SDA=GPIO21 SCL=GPIO22 (3.3V直結)
 *   TIA出力 (内蔵ADC使用時): GPIO33 (ADC1_CH5)
 */

#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <WebServer.h>
#include <stddef.h>

// ============================================================
// センサ選択
// ============================================================
// AS7331 を使うなら 1、アナログUVフォトダイオード(SG01S-C18 + TIA)なら 0（既定）。
// PlatformIO 等から -DSENSOR_AS7331=1 で上書き可能。
#ifndef SENSOR_AS7331
#define SENSOR_AS7331 0
#endif

// アナログ素子の読み取り方法 (SENSOR_AS7331=0 のとき有効)
//   SG01S-C18 (SiC) を TIA で増幅 → ADS1115 でAD変換
//   1 = 外付けADC ADS1115 (16-bit I2C) 【推奨】 / 0 = ESP32 内蔵ADC
#ifndef PD_USE_ADS1115
#define PD_USE_ADS1115 1
#endif

// ============================================================
// ピン / 画面
// ============================================================
#define TFT_CS    15
#define TFT_DC     2
#define TFT_RST    4
#define TFT_BL    32
#define TS_CS     14
#define TS_IRQ    27
#define VSPI_SCK  18
#define VSPI_MISO 19
#define VSPI_MOSI 23

#define LED_265_PIN 16
#define LED_280_PIN 17
#define I2C_SDA     21
#define I2C_SCL     22

#define SCREEN_W 320
#define SCREEN_H 240

Adafruit_ST7789 display(TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(TS_CS, TS_IRQ);
WebServer server(80);

// ---- 色 (RGB565) ----
#define COL_BG      ST77XX_BLACK
#define COL_FG      ST77XX_WHITE
#define COL_BTN     0x2B7E  // 青系
#define COL_BTN2    0x4A69  // 灰青
#define COL_ACCENT  ST77XX_CYAN
#define COL_OK      ST77XX_GREEN
#define COL_WARN    ST77XX_YELLOW
#define COL_ERR     ST77XX_RED

// ---- WiFi AP 既定値 ----
#define AP_SSID "mynanodrop"
#define AP_PASS "12345678"

// デバッグ: タッチ生値を Serial に出す
#define TOUCH_DEBUG 0

// ============================================================
// 永続化データ (ESP32 EEPROM エミュレーション)
// ============================================================
#define EEPROM_SIZE 256
#define CFG_MAGIC   0x4E414E4F  // "NANO"
#define CFG_VERSION 2

struct StoredData {
  uint32_t magic;
  uint8_t  version;
  uint8_t  calibValid;   // 校正データ有効
  uint8_t  wifiValid;    // WiFi 認証情報有効
  uint8_t  brightness;   // 0-255
  float    k260, b260, k280, b280;
  int16_t  tsMinX, tsMaxX, tsMinY, tsMaxY; // タッチ校正
  char     ssid[33];
  char     pass[65];
  uint32_t checksum;
} __attribute__((packed));

StoredData cfg;

static uint32_t cfgChecksum(const StoredData& d) {
  const uint8_t* p = (const uint8_t*)&d;
  size_t n = offsetof(StoredData, checksum);
  uint32_t s = 0;
  for (size_t i = 0; i < n; i++) s += p[i];
  return s;
}

void cfgDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = CFG_MAGIC;
  cfg.version = CFG_VERSION;
  cfg.calibValid = 0;
  cfg.wifiValid = 0;
  cfg.brightness = 200;
  cfg.k260 = 1.0f; cfg.b260 = 0.0f;
  cfg.k280 = 1.0f; cfg.b280 = 0.0f;
  cfg.tsMinX = 200; cfg.tsMaxX = 3700;
  cfg.tsMinY = 240; cfg.tsMaxY = 3800;
  cfg.ssid[0] = 0; cfg.pass[0] = 0;
}

void loadConfig() {
  EEPROM.get(0, cfg);
  if (cfg.magic != CFG_MAGIC || cfg.version != CFG_VERSION ||
      cfgChecksum(cfg) != cfg.checksum) {
    cfgDefaults();
  }
}

void saveConfig() {
  cfg.magic = CFG_MAGIC;
  cfg.version = CFG_VERSION;
  cfg.checksum = cfgChecksum(cfg);
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

void applyBrightness() {
  analogWrite(TFT_BL, cfg.brightness);
}

// ============================================================
// AS7331 UVセンサドライバ (I2C)
// ============================================================
#define AS7331_ADDR        0x74

#define AS7331_GAIN_RAW    10   // GAIN_2 -> 2^(11-10)=2
#define AS7331_TIME_RAW    6    // 64ms
#define AS7331_CCLK_RAW    0    // 1.024MHz
#define AS7331_GAIN_X      (1 << (11 - AS7331_GAIN_RAW))
#define AS7331_CONV_MS     (1 << AS7331_TIME_RAW)
#define AS7331_CCLK_HZ     (1024.0f * (1 << AS7331_CCLK_RAW))

#define AS7331_FSR_UVA     348160.0f
#define AS7331_FSR_UVB     387072.0f
#define AS7331_FSR_UVC     169984.0f
#define AS7331_MEAS_CMD    1

#define AS7331_REG_CFG_OSR     0x00
#define AS7331_REG_CFG_AGEN    0x02
#define AS7331_REG_CFG_CREG1   0x06
#define AS7331_REG_CFG_CREG2   0x07
#define AS7331_REG_CFG_CREG3   0x08
#define AS7331_REG_CFG_BREAK   0x09
#define AS7331_REG_CFG_EDGES   0x0A
#define AS7331_REG_CFG_OPTREG  0x0B
#define AS7331_REG_MRES1       0x02

#define AS7331_OPMODE_CFG      0x02
#define AS7331_OPMODE_MEAS     0x03

#define LED_PWM 255
#define CALIB_MS 300

#if SENSOR_AS7331
static bool as7331WriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(AS7331_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool as7331ReadRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(AS7331_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)AS7331_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static float as7331CountsToUwCm2(uint16_t raw, float fsr) {
  float factor = fsr / ((float)AS7331_GAIN_X * (float)AS7331_CONV_MS * AS7331_CCLK_HZ);
  return (float)raw * factor;
}

static bool as7331Begin() {
  as7331WriteReg(AS7331_REG_CFG_OSR, AS7331_OPMODE_CFG | (1 << 6));
  delay(1);
  uint8_t osr = 0;
  if (!as7331ReadRegs(AS7331_REG_CFG_AGEN, &osr, 1)) return false;
  if ((osr & 0xF0) != 0x20) return false;
  if (!as7331WriteReg(AS7331_REG_CFG_OSR, AS7331_OPMODE_CFG | (1 << 6) | 0x08)) return false;
  delay(10);
  if (!as7331WriteReg(AS7331_REG_CFG_OSR, AS7331_OPMODE_CFG | (1 << 6))) return false;
  delay(1);

  uint8_t creg1 = (uint8_t)((AS7331_GAIN_RAW << 4) | (AS7331_TIME_RAW & 0x0F));
  uint8_t creg2 = 0x00;
  uint8_t creg3 = (uint8_t)((AS7331_MEAS_CMD << 6) | (AS7331_CCLK_RAW & 0x03));

  if (!as7331WriteReg(AS7331_REG_CFG_CREG1, creg1)) return false;
  if (!as7331WriteReg(AS7331_REG_CFG_CREG2, creg2)) return false;
  if (!as7331WriteReg(AS7331_REG_CFG_CREG3, creg3)) return false;
  if (!as7331WriteReg(AS7331_REG_CFG_BREAK, 25)) return false;
  if (!as7331WriteReg(AS7331_REG_CFG_EDGES, 1)) return false;
  if (!as7331WriteReg(AS7331_REG_CFG_OPTREG, 0x01)) return false;
  if (!as7331WriteReg(AS7331_REG_CFG_OSR, AS7331_OPMODE_MEAS)) return false;
  return true;
}

static bool as7331Start() {
  uint8_t osr = 0;
  if (!as7331ReadRegs(AS7331_REG_CFG_OSR, &osr, 1)) return false;
  osr |= (1 << 7);
  return as7331WriteReg(AS7331_REG_CFG_OSR, osr);
}

static bool as7331ReadUV(uint16_t& uva, uint16_t& uvb, uint16_t& uvc) {
  uint8_t b[6];
  if (!as7331ReadRegs(AS7331_REG_MRES1, b, sizeof(b))) return false;
  uva = ((uint16_t)b[1] << 8) | b[0];
  uvb = ((uint16_t)b[3] << 8) | b[2];
  uvc = ((uint16_t)b[5] << 8) | b[4];
  return true;
}
#endif  // SENSOR_AS7331

void ledOn(int pin)  { analogWrite(pin, LED_PWM); }
void ledOff(int pin) { analogWrite(pin, 0); }

#if !SENSOR_AS7331
// ============================================================
// アナログUVフォトダイオード + TIA + 外付けADC ADS1115 ドライバ
//   例: SG01S-C18 (SiC) の光電流を TIA で電圧に変換し ADS1115 AIN0 へ。
//   単一チャンネルなので 265/280 どちらのLEDでも同じ信号を読む。
//   吸光度は比 I/I0 なので TIA ゲイン・絶対値はキャンセルされ生カウントで良い。
// ============================================================
#define SENSOR_PIN 33    // 内蔵ADC使用時の TIA出力 (GPIO33 = ADC1_CH5)
#define PD_AVG     8     // 1測定あたりの平均サンプル数

#if PD_USE_ADS1115
#define ADS1115_ADDR      0x48  // ADDRピン=GND
#define ADS1115_REG_CONV  0x00
#define ADS1115_REG_CFG   0x01
// OS=1, MUX=AIN0-GND, PGA=±4.096V, single-shot, 128SPS, comparator off
#define ADS1115_CFG_START 0xC383

static void adsWrite16(uint8_t reg, uint16_t v) {
  Wire.beginTransmission(ADS1115_ADDR);
  Wire.write(reg);
  Wire.write((uint8_t)(v >> 8));
  Wire.write((uint8_t)(v & 0xFF));
  Wire.endTransmission();
}

static uint16_t adsRead16(uint8_t reg) {
  Wire.beginTransmission(ADS1115_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  if (Wire.requestFrom((uint8_t)ADS1115_ADDR, (uint8_t)2) != 2) return 0;
  uint16_t v = (uint16_t)Wire.read() << 8;
  v |= (uint16_t)Wire.read();
  return v;
}

static bool ads1115Begin() {
  Wire.beginTransmission(ADS1115_ADDR);
  return Wire.endTransmission() == 0;
}

// 単発変換して生カウント(符号付16bit)を返す
static float ads1115Read() {
  adsWrite16(ADS1115_REG_CFG, ADS1115_CFG_START);  // 変換開始
  uint32_t t0 = millis();
  while (millis() - t0 < 25) {
    if (adsRead16(ADS1115_REG_CFG) & 0x8000) break; // OS=1: 完了
    delay(1);
  }
  return (float)(int16_t)adsRead16(ADS1115_REG_CONV);
}
#endif  // PD_USE_ADS1115
#endif  // !SENSOR_AS7331

// センサ初期化（成功で true）
bool sensorBegin() {
#if SENSOR_AS7331
  return as7331Begin();
#elif PD_USE_ADS1115
  return ads1115Begin();
#else
  analogReadResolution(12);
  analogSetPinAttenuation(SENSOR_PIN, ADC_11db);  // 0-3.3V レンジ
  pinMode(SENSOR_PIN, INPUT);
  return true;
#endif
}

// 指定LEDを点灯し、安定後に1回測定して強度を返す（エラー時 -1.0f）。
//   AS7331: 265nm->UVC / 280nm->UVB を µW/cm² に換算
//   アナログ: TIA出力を ADC で読む（比 I/I0 用なので単位は任意）
float measureUV(int ledPin) {
  ledOn(ledPin);
  delay(CALIB_MS);
#if SENSOR_AS7331
  if (!as7331Start()) { ledOff(ledPin); return -1.0f; }
  delay(AS7331_CONV_MS + 2);
  uint16_t uva, uvb, uvc;
  bool ok = as7331ReadUV(uva, uvb, uvc);
  ledOff(ledPin);
  if (!ok) return -1.0f;
  if (ledPin == LED_265_PIN) return as7331CountsToUwCm2(uvc, AS7331_FSR_UVC);
  return as7331CountsToUwCm2(uvb, AS7331_FSR_UVB);
#elif PD_USE_ADS1115
  float sum = 0;
  for (int i = 0; i < PD_AVG; i++) sum += ads1115Read();
  ledOff(ledPin);
  return sum / PD_AVG;
#else
  long sum = 0;
  for (int i = 0; i < PD_AVG; i++) sum += analogRead(SENSOR_PIN);
  ledOff(ledPin);
  return (float)sum / PD_AVG;
#endif
}

// ブランク信号の下限（これ未満は汚れ/未設置/結線ミス等）
#if SENSOR_AS7331
#define MIN_SIGNAL 0.5f
#elif PD_USE_ADS1115
#define MIN_SIGNAL 50.0f      // ADS1115 カウント (PGA ±4.096V, 約6mV)
#else
#define MIN_SIGNAL 300.0f     // ESP32 内蔵ADC カウント
#endif

float computeAbsorbance(float i, float i0) {
  if (i0 <= MIN_SIGNAL) return 9.99f;
  if (i < 0.0f) return 9.99f;
  float ratio = i / i0;
  if (ratio <= 0.0001f) return 4.0f;
  return -10.0f * log10(ratio);
}

float calibrate(float a, float k, float b) { return k * a + b; }

// ============================================================
// 測定データ (メモリ保持) + CSV
// ============================================================
#define MAX_RECORDS 50
struct Record { float a260, a280, ratio, conc; };
Record records[MAX_RECORDS];
int recordCount = 0;

String buildCsv() {
  String csv = "index,A260,A280,Purity,Conc_ug_ml\n";
  for (int i = 0; i < recordCount; i++) {
    csv += String(i + 1) + ",";
    csv += String(records[i].a260, 3) + ",";
    csv += String(records[i].a280, 3) + ",";
    csv += String(records[i].ratio, 3) + ",";
    csv += String(records[i].conc, 2) + "\n";
  }
  return csv;
}

// ============================================================
// Web サーバ
// ============================================================
String currentIPString() {
  if (WiFi.getMode() & WIFI_MODE_AP) return WiFi.softAPIP().toString();
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return String("0.0.0.0");
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>DIY Nanodrop</title></head><body style='font-family:sans-serif;margin:24px'>";
  html += "<h1>DIY Nanodrop</h1>";
  html += "<p>Records: <b>" + String(recordCount) + "</b></p>";
  html += "<p><a href='/data.csv' style='font-size:20px'>Download data.csv</a></p>";
  html += "<p style='color:#888'>Keep this page open over the device WiFi.</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleCsv() {
  String csv = buildCsv();
  server.sendHeader("Content-Disposition", "attachment; filename=data.csv");
  server.send(200, "text/csv", csv);
}

void setupServer() {
  server.on("/", handleRoot);
  server.on("/data.csv", handleCsv);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

// ============================================================
// WiFi
// ============================================================
void startAP() {
  WiFi.mode(WIFI_AP);
  IPAddress ip(192, 168, 5, 1), gw(192, 168, 5, 1), sn(255, 255, 255, 0);
  WiFi.softAPConfig(ip, gw, sn);
  WiFi.softAP(AP_SSID, AP_PASS);
}

void wifiBoot() {
  if (cfg.wifiValid && cfg.ssid[0]) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid, cfg.pass);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(100);
    if (WiFi.status() == WL_CONNECTED) return;
  }
  startAP();
}

// ============================================================
// UI 基盤
// ============================================================
enum Screen {
  SCR_HOME,
  SCR_CAL_CHECK_OK,
  SCR_CAL_ASK,
  SCR_CAL_INTRO,
  SCR_CAL_BLANK,
  SCR_CAL_DNA,
  SCR_CAL_PROT,
  SCR_CAL_DONE,
  SCR_MEAS_BLANK,
  SCR_MEAS_SAMPLE,
  SCR_MEAS_RESULT,
  SCR_SETTINGS,
  SCR_BRIGHT,
  SCR_WIFI,
  SCR_WIFI_SCAN,
  SCR_WIFI_LIST,
  SCR_WIFI_WPS,
  SCR_WIFI_DONE,
  SCR_TOUCHCAL,
  SCR_LED_TUNE,
  SCR_DOWNLOAD,
  SCR_MSG
};

Screen screen = SCR_HOME;
Screen msgReturn = SCR_HOME;
Screen screenReturn = SCR_HOME;   // Download の戻り先
char msg1[40] = {0}, msg2[40] = {0};

int wifiCount = 0;
int wifiSel = -1;
int touchCalStep = 0;

void wifiStartScan();
void wifiRunWps();

void clearScreen() { display.fillScreen(COL_BG); }

void centerText(const char* s, int y, uint8_t size, uint16_t color) {
  display.setTextSize(size);
  display.setTextColor(color);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_W - (int16_t)w) / 2 - x1, y);
  display.print(s);
}

void drawButton(int x, int y, int w, int h, const char* label,
                uint16_t bg = COL_BTN, uint8_t textSize = 2) {
  display.fillRoundRect(x, y, w, h, 8, bg);
  display.drawRoundRect(x, y, w, h, 8, COL_FG);
  display.setTextSize(textSize);
  display.setTextColor(COL_FG);
  int16_t x1, y1; uint16_t ww, hh;
  display.getTextBounds(label, 0, 0, &x1, &y1, &ww, &hh);
  display.setCursor(x + (w - (int16_t)ww) / 2 - x1, y + (h - (int16_t)hh) / 2 - y1);
  display.print(label);
}

bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

// ---- ホーム ----
#define HOME_X 30
#define HOME_W 260
#define HOME_H 46
#define HOME_CAL_Y 50
#define HOME_MEA_Y 102
#define HOME_SET_Y 154
#define DL_X 8
#define DL_Y 210
#define DL_W 304
#define DL_H 24

void renderHome() {
  clearScreen();
  centerText("DIY Nanodrop", 6, 3, COL_ACCENT);
  drawButton(HOME_X, HOME_CAL_Y, HOME_W, HOME_H, "Calibration", COL_BTN, 3);
  drawButton(HOME_X, HOME_MEA_Y, HOME_W, HOME_H, "Measuring", COL_BTN, 3);
  drawButton(HOME_X, HOME_SET_Y, HOME_W, HOME_H, "Settings", COL_BTN2, 3);
  drawButton(DL_X, DL_Y, DL_W, DL_H, "Download data", 0x4208, 2);
}

void renderDownload() {
  clearScreen();
  centerText("Download data", 6, 3, COL_ACCENT);
  centerText("Connect to this WiFi and", 54, 2, COL_FG);
  String ip = currentIPString();
  String url = "http://" + ip + "/data.csv";
  centerText(url.c_str(), 84, 2, COL_OK);
  centerText("Records stored:", 120, 2, COL_FG);
  centerText(String(recordCount).c_str(), 144, 3, COL_FG);
  if (WiFi.getMode() & WIFI_MODE_AP) {
    centerText("AP:", 180, 1, COL_WARN);
    centerText((String(AP_SSID) + " / " + AP_PASS).c_str(), 194, 1, COL_WARN);
  }
  drawButton(110, 210, 100, 24, "Back", COL_BTN2, 2);
}

void showDownload(Screen from) {
  screenReturn = from;
  screen = SCR_DOWNLOAD;
  renderDownload();
}

void showMessage(const char* l1, const char* l2, Screen ret) {
  msgReturn = ret;
  strncpy(msg1, l1 ? l1 : "", sizeof(msg1) - 1); msg1[sizeof(msg1) - 1] = 0;
  strncpy(msg2, l2 ? l2 : "", sizeof(msg2) - 1); msg2[sizeof(msg2) - 1] = 0;
  screen = SCR_MSG;
  clearScreen();
  if (l1) centerText(l1, 70, 2, COL_FG);
  if (l2) centerText(l2, 110, 2, COL_FG);
  drawButton(110, 170, 100, 44, "OK", COL_BTN, 3);
}

// ============================================================
// 校正フロー
// ============================================================
const float DNA_TRUE_A260 = 1.0f;
const float DNA_TRUE_A280 = 0.47f;
const float PROT_TRUE_A260 = 0.074f;
const float PROT_TRUE_A280 = 0.13f;

float cal_i0_265 = 0, cal_i0_280 = 0;
float cal_dna260 = 0, cal_dna280 = 0;
float cal_prot260 = 0, cal_prot280 = 0;

void renderCalCheckOk() {
  clearScreen();
  centerText("Calibration", 6, 3, COL_ACCENT);
  centerText("Calibration data found", 54, 2, COL_OK);
  char b[40];
  snprintf(b, sizeof(b), "K260=%.4f B260=%.4f", cfg.k260, cfg.b260);
  centerText(b, 86, 1, COL_FG);
  snprintf(b, sizeof(b), "K280=%.4f B280=%.4f", cfg.k280, cfg.b280);
  centerText(b, 102, 1, COL_FG);
  drawButton(30, 150, 130, 50, "Re-calibrate", COL_BTN, 2);
  drawButton(175, 150, 115, 50, "Back", COL_BTN2, 2);
}

void renderCalAsk() {
  clearScreen();
  centerText("Calibration", 6, 3, COL_ACCENT);
  centerText("No calibration data", 60, 2, COL_WARN);
  centerText("Configure now?", 96, 2, COL_FG);
  drawButton(40, 150, 100, 54, "Yes", COL_OK, 3);
  drawButton(180, 150, 100, 54, "No", COL_BTN2, 3);
}

void renderCalIntro() {
  clearScreen();
  centerText("Calibration", 6, 3, COL_ACCENT);
  centerText("Standards needed:", 50, 2, COL_FG);
  centerText("DNA: 50 ug/mL  (A260)", 82, 2, COL_ACCENT);
  centerText("Protein: 1% w/v  (A280)", 108, 2, COL_ACCENT);
  centerText("Follow the steps shown", 140, 1, COL_FG);
  centerText("on screen. Keep blank ready.", 154, 1, COL_FG);
  drawButton(90, 186, 140, 44, "Start", COL_OK, 3);
}

void renderCalStep(const char* title, const char* label, const char* hint) {
  clearScreen();
  centerText("Calibration", 6, 2, COL_ACCENT);
  centerText(title, 44, 3, COL_FG);
  if (hint) centerText(hint, 100, 2, COL_WARN);
  centerText(label, 140, 2, COL_FG);
  drawButton(70, 180, 180, 48, "Measure", COL_OK, 3);
}

void renderCalDone() {
  clearScreen();
  centerText("Calibration", 6, 3, COL_ACCENT);
  centerText("Saved to EEPROM", 50, 2, COL_OK);
  char b[40];
  snprintf(b, sizeof(b), "K260=%.4f", cfg.k260); centerText(b, 88, 2, COL_FG);
  snprintf(b, sizeof(b), "B260=%.4f", cfg.b260); centerText(b, 110, 2, COL_FG);
  snprintf(b, sizeof(b), "K280=%.4f", cfg.k280); centerText(b, 132, 2, COL_FG);
  snprintf(b, sizeof(b), "B280=%.4f", cfg.b280); centerText(b, 154, 2, COL_FG);
  drawButton(110, 190, 100, 40, "OK", COL_BTN, 3);
}

void runCalBlank() {
  showMessage("Measuring BLANK", "please wait...", SCR_CAL_DNA);
  // 画面更新後に測定
  cal_i0_265 = measureUV(LED_265_PIN);
  cal_i0_280 = measureUV(LED_280_PIN);
  if (cal_i0_265 < MIN_SIGNAL || cal_i0_280 < MIN_SIGNAL) {
    showMessage("BLANK too low", "check cell/wiring", SCR_CAL_BLANK);
    return;
  }
  screen = SCR_CAL_DNA;
  renderCalStep("Step 2/3  DNA", "Apply 50 ug/mL DNA", "then tap Measure");
}

void runCalDna() {
  showMessage("Measuring DNA", "please wait...", SCR_CAL_PROT);
  float d265 = measureUV(LED_265_PIN);
  float d280 = measureUV(LED_280_PIN);
  cal_dna260 = computeAbsorbance(d265, cal_i0_265);
  cal_dna280 = computeAbsorbance(d280, cal_i0_280);
  screen = SCR_CAL_PROT;
  renderCalStep("Step 3/3  Protein", "Apply 1% w/v protein", "then tap Measure");
}

void runCalProt() {
  showMessage("Measuring Protein", "please wait...", SCR_CAL_DONE);
  float p265 = measureUV(LED_265_PIN);
  float p280 = measureUV(LED_280_PIN);
  cal_prot260 = computeAbsorbance(p265, cal_i0_265);
  cal_prot280 = computeAbsorbance(p280, cal_i0_280);

  float denom260 = cal_dna260 - cal_prot260;
  float denom280 = cal_dna280 - cal_prot280;
  if (fabsf(denom260) < 1e-4f || fabsf(denom280) < 1e-4f) {
    showMessage("Calibration failed", "standards too close", SCR_HOME);
    return;
  }
  cfg.k260 = (PROT_TRUE_A260 - DNA_TRUE_A260) / denom260;
  cfg.b260 = DNA_TRUE_A260 - cfg.k260 * cal_dna260;
  cfg.k280 = (PROT_TRUE_A280 - DNA_TRUE_A280) / denom280;
  cfg.b280 = DNA_TRUE_A280 - cfg.k280 * cal_dna280;
  cfg.calibValid = 1;
  saveConfig();

  Serial.printf("CAL RESULT K260=%.6f B260=%.6f K280=%.6f B280=%.6f\n",
                cfg.k260, cfg.b260, cfg.k280, cfg.b280);
  screen = SCR_CAL_DONE;
  renderCalDone();
}

// ============================================================
// 測定フロー
// ============================================================
float meas_i0_265 = 0, meas_i0_280 = 0;

void renderMeasBlank() {
  clearScreen();
  centerText("Measuring", 4, 2, COL_ACCENT);
  centerText("Apply BLANK", 40, 3, COL_FG);
  centerText("(pure solvent)", 84, 2, COL_FG);
  centerText("then tap Measure", 116, 2, COL_WARN);
  drawButton(70, 160, 180, 48, "Measure BLANK", COL_OK, 2);
  drawButton(DL_X, DL_Y, DL_W, DL_H, "Download data", 0x4208, 2);
}

void renderMeasSample() {
  clearScreen();
  centerText("Measuring", 4, 2, COL_ACCENT);
  centerText("Apply SAMPLE", 40, 3, COL_FG);
  centerText("then tap Measure", 100, 2, COL_WARN);
  drawButton(70, 160, 180, 48, "Measure SAMPLE", COL_OK, 2);
  drawButton(DL_X, DL_Y, DL_W, DL_H, "Download data", 0x4208, 2);
}

void renderMeasResult() {
  clearScreen();
  centerText("Result", 4, 2, COL_ACCENT);
  if (recordCount == 0) { centerText("no data", 100, 2, COL_FG); return; }
  Record& r = records[recordCount - 1];
  char b[44];
  snprintf(b, sizeof(b), "A260=%.3f  A280=%.3f", r.a260, r.a280);
  centerText(b, 34, 2, COL_FG);
  snprintf(b, sizeof(b), "Purity=%.2f", r.ratio);
  centerText(b, 58, 2, COL_FG);
  snprintf(b, sizeof(b), "Conc=%.1f ug/mL", r.conc);
  centerText(b, 88, 3, COL_OK);
  centerText("Before next sample:", 130, 1, COL_FG);
  centerText("apply BLANK again", 144, 1, COL_WARN);
  drawButton(10, 170, 150, 34, "Next: BLANK", COL_BTN, 2);
  drawButton(168, 170, 142, 34, "Download data", 0x4208, 2);
}

void runMeasBlank() {
  showMessage("Measuring BLANK", "please wait...", SCR_MEAS_SAMPLE);
  meas_i0_265 = measureUV(LED_265_PIN);
  meas_i0_280 = measureUV(LED_280_PIN);
  if (meas_i0_265 < MIN_SIGNAL || meas_i0_280 < MIN_SIGNAL) {
    showMessage("BLANK too low", "check cell/wiring", SCR_MEAS_BLANK);
    return;
  }
  screen = SCR_MEAS_SAMPLE;
  renderMeasSample();
}

void runMeasSample() {
  showMessage("Measuring SAMPLE", "please wait...", SCR_MEAS_RESULT);
  float i265 = measureUV(LED_265_PIN);
  float i280 = measureUV(LED_280_PIN);
  float a260m = computeAbsorbance(i265, meas_i0_265);
  float a280m = computeAbsorbance(i280, meas_i0_280);
  float a260 = calibrate(a260m, cfg.k260, cfg.b260);
  float a280 = calibrate(a280m, cfg.k280, cfg.b280);
  float ratio = (a280 > 0.001f) ? a260 / a280 : 99.9f;
  float conc = a260 * 50.0f;  // dsDNA ug/mL

  if (recordCount < MAX_RECORDS) {
    records[recordCount++] = { a260, a280, ratio, conc };
  }
  Serial.printf("SAMPLE A260=%.3f A280=%.3f Ratio=%.3f Conc=%.1f\n",
                a260, a280, ratio, conc);
  screen = SCR_MEAS_RESULT;
  renderMeasResult();
}

// ============================================================
// 設定
// ============================================================
void renderSettings() {
  clearScreen();
  centerText("Settings", 4, 3, COL_ACCENT);
  drawButton(10, 40, 300, 32, "Brightness", COL_BTN, 2);
  drawButton(10, 76, 300, 32, "LED tune", COL_BTN, 2);
  drawButton(10, 112, 300, 32, "WiFi settings", COL_BTN, 2);
  drawButton(10, 148, 300, 32, "Touch calibration", COL_BTN2, 2);
  drawButton(10, 184, 210, 32, "Initialize WiFi", COL_ERR, 2);
  drawButton(228, 184, 82, 32, "Back", COL_BTN2, 2);
}

#define SET_BRIGHT_Y 40
#define SET_LED_Y    76
#define SET_WIFI_Y   112
#define SET_TOUCH_Y  148
#define SET_INIT_Y   184

void renderBrightness() {
  clearScreen();
  centerText("Brightness", 6, 3, COL_ACCENT);
  char b[24];
  snprintf(b, sizeof(b), "%d", cfg.brightness);
  centerText(b, 80, 4, COL_FG);
  drawButton(40, 150, 80, 50, "-", COL_BTN, 3);
  drawButton(200, 150, 80, 50, "+", COL_BTN, 3);
  drawButton(120, 150, 80, 50, "OK", COL_OK, 3);
}

void renderWifiInfo() {
  clearScreen();
  centerText("WiFi", 6, 3, COL_ACCENT);
  if (cfg.wifiValid && WiFi.status() == WL_CONNECTED) {
    centerText("Connected (STA)", 48, 2, COL_OK);
    centerText(cfg.ssid, 82, 2, COL_FG);
    centerText(WiFi.localIP().toString().c_str(), 110, 2, COL_FG);
  } else if (WiFi.getMode() & WIFI_MODE_AP) {
    centerText("AP mode", 48, 2, COL_WARN);
    centerText((String("SSID: ") + AP_SSID).c_str(), 80, 2, COL_FG);
    centerText((String("pass: ") + AP_PASS).c_str(), 106, 2, COL_FG);
    centerText("IP: 192.168.5.1", 132, 2, COL_FG);
  } else {
    centerText("Not connected", 60, 2, COL_WARN);
  }
  drawButton(20, 176, 170, 48, "Scan & WPS", COL_BTN, 2);
  drawButton(200, 176, 100, 48, "Back", COL_BTN2, 2);
}

void renderWifiScan() {
  clearScreen();
  centerText("WiFi", 6, 3, COL_ACCENT);
  centerText("Scanning...", 90, 3, COL_FG);
}

void renderWifiList() {
  clearScreen();
  centerText("Select SSID", 4, 2, COL_ACCENT);
  int total = 0;
  int shown = 0;
  // WiFi.SSID は scan 後有効
  for (int i = 0; i < wifiCount && shown < 6; i++, shown++) {
    int y = 30 + shown * 30;
    String s = WiFi.SSID(i);
    if (s.length() > 26) s = s.substring(0, 26);
    char label[40];
    snprintf(label, sizeof(label), "%s", s.c_str());
    drawButton(8, y, 304, 26, label, (i == wifiSel ? COL_OK : COL_BTN),
               s.length() > 16 ? 1 : 2);
    total++;
  }
  drawButton(8, 210, 100, 24, "Back", COL_BTN2, 2);
}

void renderWifiWps() {
  clearScreen();
  centerText("WPS", 6, 3, COL_ACCENT);
  centerText("Press the WPS button", 70, 2, COL_FG);
  centerText("on your router now.", 96, 2, COL_FG);
  centerText("Connecting automatically...", 140, 2, COL_WARN);
}

void renderWifiDone(bool ok) {
  clearScreen();
  centerText("WiFi", 6, 3, COL_ACCENT);
  if (ok) {
    centerText("Connected!", 60, 3, COL_OK);
    centerText(cfg.ssid, 106, 2, COL_FG);
    centerText(WiFi.localIP().toString().c_str(), 132, 2, COL_FG);
  } else {
    centerText("WPS failed", 70, 3, COL_ERR);
    centerText("Check router WPS", 116, 2, COL_FG);
  }
  drawButton(110, 180, 100, 44, "OK", COL_BTN, 3);
}

void renderTouchCal(int step) {
  clearScreen();
  centerText("Touch calibration", 6, 2, COL_ACCENT);
  if (step == 1) centerText("Tap the TOP-LEFT target", 70, 2, COL_FG);
  else centerText("Tap the BOTTOM-RIGHT target", 70, 2, COL_FG);
  int tx = (step == 1) ? 20 : SCREEN_W - 20;
  int ty = (step == 1) ? 20 : SCREEN_H - 20;
  display.drawLine(tx - 12, ty, tx + 12, ty, COL_ERR);
  display.drawLine(tx, ty - 12, tx, ty + 12, COL_ERR);
  display.drawCircle(tx, ty, 8, COL_ERR);
}

// ============================================================
// LED 出力調整モード
//   2つのLEDを交互に点灯し、センサ生値と電圧換算値を連続表示する。
//   ノイズ対策として指数移動平均 (EMA) を適用:
//       X = a * Xnow + (1 - a) * X
//   (X: 前回のフィルタ値, a: 0<a<=1, 小さいほど平滑)
// ============================================================
#define TUNE_ALPHA     0.20f   // EMA係数 a
#define TUNE_STEP_MS   200     // LED切替周期
#define TUNE_SETTLE_MS 40      // LED点灯後の安定待ち
#define TUNE_AVG       4       // 1回あたりの平均サンプル数
#if !SENSOR_AS7331 && PD_USE_ADS1115
#define ADS1115_LSB_V  (4.096f / 32768.0f)  // PGA ±4.096V の1LSB
#endif

static int   tuneIdx = 0;                 // 次に測るLED: 0=265nm, 1=280nm
static float tuneFilt[2] = {0, 0};        // フィルタ済み生値
static bool  tuneHas[2]  = {false, false};
static unsigned long tuneNext = 0;

// 指定LEDを点灯して1回のセンサ生値を返す（エラー時 -1.0f）
static float tuneReadOnce(int pin) {
  ledOn(pin);
  delay(TUNE_SETTLE_MS);
  float raw;
#if SENSOR_AS7331
  if (!as7331Start()) { ledOff(pin); return -1.0f; }
  delay(AS7331_CONV_MS + 2);
  uint16_t uva, uvb, uvc;
  bool ok = as7331ReadUV(uva, uvb, uvc);
  raw = ok ? (float)((pin == LED_265_PIN) ? uvc : uvb) : -1.0f;
#elif PD_USE_ADS1115
  float sum = 0;
  for (int i = 0; i < TUNE_AVG; i++) sum += ads1115Read();
  raw = sum / TUNE_AVG;
#else
  long sum = 0;
  for (int i = 0; i < TUNE_AVG; i++) sum += analogRead(SENSOR_PIN);
  raw = (float)sum / TUNE_AVG;
#endif
  ledOff(pin);
  return raw;
}

void drawLedTuneRow(int y, const char* name, int idx) {
  bool active = (tuneIdx == idx);
  display.fillRoundRect(6, y, 308, 46, 8, active ? 0x1A6A : COL_BTN2);
  display.drawRoundRect(6, y, 308, 46, 8, active ? COL_OK : COL_FG);
  display.setTextSize(2);
  display.setTextColor(COL_FG);
  display.setCursor(16, y + 14);
  display.print(name);
  char b[24];
  if (tuneHas[idx]) snprintf(b, sizeof(b), "%7.0f", tuneFilt[idx]);
  else snprintf(b, sizeof(b), "%7s", "--");
  display.setCursor(100, y + 14);
  display.print(b);
#if !SENSOR_AS7331 && PD_USE_ADS1115
  if (tuneHas[idx]) snprintf(b, sizeof(b), "%.4fV", tuneFilt[idx] * ADS1115_LSB_V);
  else snprintf(b, sizeof(b), "%.4fV", 0.0f);
  display.setCursor(196, y + 14);
  display.print(b);
#endif
}

void drawLedTuneValues() {
  drawLedTuneRow(40, "LED265", 0);
  drawLedTuneRow(92, "LED280", 1);
}

void renderLedTune() {
  clearScreen();
  centerText("LED tune", 4, 3, COL_ACCENT);
  drawLedTuneValues();
  centerText("X = a*Xnow + (1-a)*X", 150, 1, COL_FG);
  char b[32];
  snprintf(b, sizeof(b), "a=%.2f  dwell=%dms", TUNE_ALPHA, TUNE_STEP_MS);
  centerText(b, 166, 1, COL_WARN);
  drawButton(110, 192, 100, 36, "Back", COL_BTN2, 2);
}

void showLedTune() {
  tuneIdx = 0;
  tuneHas[0] = tuneHas[1] = false;
  tuneNext = millis();
  screen = SCR_LED_TUNE;
  renderLedTune();
}

// loop() から毎回呼ぶ。LEDを交互に測定して表示を更新する。
void ledTuneStep() {
  unsigned long now = millis();
  if ((long)(now - tuneNext) < 0) return;
  tuneNext = now + TUNE_STEP_MS;

  int pin = (tuneIdx == 0) ? LED_265_PIN : LED_280_PIN;
  float raw = tuneReadOnce(pin);
  if (raw >= 0.0f) {
    if (!tuneHas[tuneIdx]) { tuneFilt[tuneIdx] = raw; tuneHas[tuneIdx] = true; }
    else tuneFilt[tuneIdx] = TUNE_ALPHA * raw + (1.0f - TUNE_ALPHA) * tuneFilt[tuneIdx];
  }

  drawLedTuneValues();
  tuneIdx ^= 1;
}

// ============================================================
// タッチ割当 (画面座標)
// ============================================================
void handleTouchCalRaw(int rx, int ry) {
  static int rawX1, rawY1;
  if (touchCalStep == 1) {
    rawX1 = rx; rawY1 = ry;
    touchCalStep = 2;
    renderTouchCal(2);
  } else if (touchCalStep == 2) {
    cfg.tsMinX = (int16_t)rawX1; cfg.tsMaxX = (int16_t)rx;
    cfg.tsMinY = (int16_t)rawY1; cfg.tsMaxY = (int16_t)ry;
    if (cfg.tsMinX == cfg.tsMaxX) cfg.tsMaxX += 1;
    if (cfg.tsMinY == cfg.tsMaxY) cfg.tsMaxY += 1;
    saveConfig();
    touchCalStep = 0;
    showMessage("Touch calibrated", "saved", SCR_HOME);
  }
}

void onTouch(int x, int y) {
  switch (screen) {
    case SCR_HOME:
      if (inRect(x, y, DL_X, DL_Y, DL_W, DL_H)) { showDownload(SCR_HOME); }
      else if (inRect(x, y, HOME_X, HOME_CAL_Y, HOME_W, HOME_H)) {
        if (cfg.calibValid) { screen = SCR_CAL_CHECK_OK; renderCalCheckOk(); }
        else { screen = SCR_CAL_ASK; renderCalAsk(); }
      } else if (inRect(x, y, HOME_X, HOME_MEA_Y, HOME_W, HOME_H)) {
        screen = SCR_MEAS_BLANK; renderMeasBlank();
      } else if (inRect(x, y, HOME_X, HOME_SET_Y, HOME_W, HOME_H)) {
        screen = SCR_SETTINGS; renderSettings();
      }
      break;

    case SCR_CAL_CHECK_OK:
      if (inRect(x, y, 30, 150, 130, 50)) { screen = SCR_CAL_INTRO; renderCalIntro(); }
      else if (inRect(x, y, 175, 150, 115, 50)) { screen = SCR_HOME; renderHome(); }
      break;

    case SCR_CAL_ASK:
      if (inRect(x, y, 40, 150, 100, 54)) { screen = SCR_CAL_INTRO; renderCalIntro(); }
      else if (inRect(x, y, 180, 150, 100, 54)) { screen = SCR_HOME; renderHome(); }
      break;

    case SCR_CAL_INTRO:
      if (inRect(x, y, 90, 186, 140, 44)) {
        screen = SCR_CAL_BLANK;
        renderCalStep("Step 1/3  BLANK", "Apply BLANK (pure solvent)", "then tap Measure");
      }
      break;

    case SCR_CAL_BLANK:
      if (inRect(x, y, 70, 180, 180, 48)) runCalBlank();
      break;
    case SCR_CAL_DNA:
      if (inRect(x, y, 70, 180, 180, 48)) runCalDna();
      break;
    case SCR_CAL_PROT:
      if (inRect(x, y, 70, 180, 180, 48)) runCalProt();
      break;
    case SCR_CAL_DONE:
      if (inRect(x, y, 110, 190, 100, 40)) { screen = SCR_HOME; renderHome(); }
      break;

    case SCR_MEAS_BLANK:
      if (inRect(x, y, DL_X, DL_Y, DL_W, DL_H)) { showDownload(SCR_MEAS_BLANK); }
      else if (inRect(x, y, 70, 160, 180, 48)) runMeasBlank();
      break;
    case SCR_MEAS_SAMPLE:
      if (inRect(x, y, DL_X, DL_Y, DL_W, DL_H)) { showDownload(SCR_MEAS_SAMPLE); }
      else if (inRect(x, y, 70, 160, 180, 48)) runMeasSample();
      break;
    case SCR_MEAS_RESULT:
      if (inRect(x, y, 10, 170, 150, 34)) { screen = SCR_MEAS_BLANK; renderMeasBlank(); }
      else if (inRect(x, y, 168, 170, 142, 34)) { showDownload(SCR_MEAS_RESULT); }
      break;

    case SCR_SETTINGS:
      if (inRect(x, y, 10, SET_BRIGHT_Y, 300, 32)) { screen = SCR_BRIGHT; renderBrightness(); }
      else if (inRect(x, y, 10, SET_LED_Y, 300, 32)) { showLedTune(); }
      else if (inRect(x, y, 10, SET_WIFI_Y, 300, 32)) { screen = SCR_WIFI; renderWifiInfo(); }
      else if (inRect(x, y, 10, SET_TOUCH_Y, 300, 32)) { touchCalStep = 1; screen = SCR_TOUCHCAL; renderTouchCal(1); }
      else if (inRect(x, y, 10, SET_INIT_Y, 210, 32)) {
        cfg.wifiValid = 0; cfg.ssid[0] = 0; cfg.pass[0] = 0; saveConfig();
        WiFi.disconnect(true, true);
        startAP();
        showMessage("WiFi settings cleared", "AP: mynanodrop", SCR_WIFI);
      } else if (inRect(x, y, 228, SET_INIT_Y, 82, 32)) { screen = SCR_HOME; renderHome(); }
      break;

    case SCR_LED_TUNE:
      if (inRect(x, y, 110, 192, 100, 36)) { ledOff(LED_265_PIN); ledOff(LED_280_PIN); screen = SCR_SETTINGS; renderSettings(); }
      break;

    case SCR_BRIGHT:
      if (inRect(x, y, 40, 150, 80, 50)) {
        cfg.brightness = (cfg.brightness > 20) ? cfg.brightness - 20 : 0;
        applyBrightness(); renderBrightness();
      } else if (inRect(x, y, 200, 150, 80, 50)) {
        int v = cfg.brightness + 20; cfg.brightness = (v > 255) ? 255 : v;
        applyBrightness(); renderBrightness();
      } else if (inRect(x, y, 120, 150, 80, 50)) {
        saveConfig(); screen = SCR_SETTINGS; renderSettings();
      }
      break;

    case SCR_WIFI:
      if (inRect(x, y, 20, 176, 170, 48)) { screen = SCR_WIFI_SCAN; renderWifiScan(); wifiStartScan(); }
      else if (inRect(x, y, 200, 176, 100, 48)) { screen = SCR_SETTINGS; renderSettings(); }
      break;

    case SCR_WIFI_LIST:
      if (inRect(x, y, 8, 210, 100, 24)) { screen = SCR_WIFI; renderWifiInfo(); break; }
      for (int i = 0; i < wifiCount && i < 6; i++) {
        int yy = 30 + i * 30;
        if (inRect(x, y, 8, yy, 304, 26)) { wifiSel = i; wifiRunWps(); break; }
      }
      break;

    case SCR_WIFI_DONE:
      if (inRect(x, y, 110, 180, 100, 44)) { screen = SCR_WIFI; renderWifiInfo(); }
      break;

    case SCR_DOWNLOAD:
      if (inRect(x, y, 110, 210, 100, 24)) {
        screen = screenReturn;
        if (screen == SCR_MEAS_BLANK) renderMeasBlank();
        else if (screen == SCR_MEAS_SAMPLE) renderMeasSample();
        else if (screen == SCR_MEAS_RESULT) renderMeasResult();
        else { screen = SCR_HOME; renderHome(); }
      }
      break;

    case SCR_MSG:
      screen = msgReturn;
      if (screen == SCR_HOME) renderHome();
      else if (screen == SCR_SETTINGS) renderSettings();
      else if (screen == SCR_WIFI) renderWifiInfo();
      else if (screen == SCR_MEAS_BLANK) renderMeasBlank();
      else if (screen == SCR_CAL_BLANK) renderCalStep("Step 1/3  BLANK", "Apply BLANK (pure solvent)", "then tap Measure");
      else renderHome();
      break;

    default: break;
  }
}
// ============================================================
// WiFi 操作
// ============================================================
void wifiStartScan() {
  if (WiFi.getMode() & WIFI_MODE_AP) WiFi.mode(WIFI_AP_STA);  // AP中でもスキャン可能に
  WiFi.scanDelete();
  wifiCount = WiFi.scanNetworks();
  if (wifiCount < 0) wifiCount = 0;
  wifiSel = -1;
  screen = SCR_WIFI_LIST;
  renderWifiList();
}

void wifiRunWps() {
  screen = SCR_WIFI_WPS;
  renderWifiWps();
  WiFi.mode(WIFI_AP_STA);   // AP を維持したまま WPS
  bool ok = WiFi.beginWPSConfig();
  if (ok) {
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
    ok = (WiFi.status() == WL_CONNECTED);
  }
  if (ok) {
    String s = WiFi.SSID();
    String p = WiFi.psk();
    strncpy(cfg.ssid, s.c_str(), sizeof(cfg.ssid) - 1);
    cfg.ssid[sizeof(cfg.ssid) - 1] = 0;
    strncpy(cfg.pass, p.c_str(), sizeof(cfg.pass) - 1);
    cfg.pass[sizeof(cfg.pass) - 1] = 0;
    cfg.wifiValid = 1;
    saveConfig();
    Serial.printf("WPS OK ssid=%s\n", cfg.ssid);
  } else {
    Serial.println("WPS failed");
  }
  screen = SCR_WIFI_DONE;
  renderWifiDone(ok);
}

// ============================================================
// タッチ入力
// ============================================================
void pollTouch() {
  static bool lastDown = false;
  static unsigned long lastMs = 0;
  bool down = ts.touched();
  if (down && !lastDown && millis() - lastMs > 200) {
    lastMs = millis();
    TS_Point p = ts.getPoint();
#if TOUCH_DEBUG
    Serial.printf("TOUCH raw x=%d y=%d z=%d\n", p.x, p.y, p.z);
#endif
    if (screen == SCR_TOUCHCAL) {
      handleTouchCalRaw(p.x, p.y);
    } else {
      int sx = map(p.x, cfg.tsMinX, cfg.tsMaxX, 0, SCREEN_W);
      int sy = map(p.y, cfg.tsMinY, cfg.tsMaxY, 0, SCREEN_H);
      sx = constrain(sx, 0, SCREEN_W - 1);
      sy = constrain(sy, 0, SCREEN_H - 1);
      onTouch(sx, sy);
    }
  }
  lastDown = down;
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(TFT_BL, OUTPUT);
  analogWrite(TFT_BL, 200);

  SPI.begin(VSPI_SCK, VSPI_MISO, VSPI_MOSI, TFT_CS);
  display.init(240, 320);
  display.setRotation(1);
  display.fillScreen(COL_BG);
  ts.begin();
  ts.setRotation(1);

  EEPROM.begin(EEPROM_SIZE);
  loadConfig();
  applyBrightness();

  pinMode(LED_265_PIN, OUTPUT);
  pinMode(LED_280_PIN, OUTPUT);
  digitalWrite(LED_265_PIN, LOW);
  digitalWrite(LED_280_PIN, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);

  // WiFi + Web サーバ
  wifiBoot();
  setupServer();

  // センサ初期化
  bool sensorOk = sensorBegin();
  if (!sensorOk) {
#if SENSOR_AS7331
    Serial.println("AS7331 not found (0x74)");
    showMessage("AS7331 not found", "check I2C wiring", SCR_HOME);
#else
    Serial.println("ADS1115 not found (0x48)");
    showMessage("ADS1115 not found", "check I2C wiring", SCR_HOME);
#endif
  } else {
#if SENSOR_AS7331
    Serial.println("Sensor: AS7331 ready");
#elif PD_USE_ADS1115
    Serial.println("Sensor: SG01S-C18 (TIA) via ADS1115 ready");
#else
    Serial.println("Sensor: SG01S-C18 (TIA) via ESP32 ADC ready");
#endif
  }

  Serial.print("WiFi IP: "); Serial.println(currentIPString());
  Serial.printf("Calib valid=%d  brightness=%d\n", cfg.calibValid, cfg.brightness);

  if (sensorOk) {
    screen = SCR_HOME;
    renderHome();
  }
}

void loop() {
  server.handleClient();
  pollTouch();
  if (screen == SCR_LED_TUNE) ledTuneStep();
  delay(5);
}
