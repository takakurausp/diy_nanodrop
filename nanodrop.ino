/*
 * DIY Nanodrop (UV) - LGT8F328P Nano  [STANDALONE + CALIBRATION]
 *
 * 265nm + 280nm UVC LED を切替駆動し、GUVA-S12SD で検出。
 * 吸光度 A = -log10(I / I0) と純度比 A260/A280 を算出。
 * OLED表示（オプション）＋ Serial出力。校正モード（固定濃度・armステップ式）。
 *
 * USE_OLED=1 で OLED 表示有効、USE_OLED=0 で OLED なし（Serial のみ）。
 *   (OLEDには Adafruit_GFX + Adafruit_SSD1306 ライブラリが必要)
 *
 * ---- ピン配置 (LGT8F328P Nano) ----
 *   LED_265 PWM  -> D3  (PD3, PWM対応)
 *   LED_280 PWM  -> D5  (PD5, PWM対応)
 *   SENSOR       -> A0  (PC0, ADC0)
 *   OLED I2C     -> SDA=D18(PC4), SCL=D19(PC5)  [ハードウェア固定]
 *   CALIB BUTTON -> D7  (PB7)  校正モード切替（プルダウン＋ボタン→VCC）
 *   ARM SWITCH   -> D8  (PB0)  アーム連動スイッチ
 *       ARM_USE_HALL=0 : マクロスイッチ（プルアップ、押下=LOW、FALLING）
 *       ARM_USE_HALL=1 : ホールセンサー（プルダウン外付け、磁石でHIGH、RISING）
 *
 * ---- 校正モデル ----
 *   A_true = k_w * A_meas + b_w   (w = 260 or 280)
 *   Purity = (k260/k280) * (A260_true / A280_true)
 *
 *   k/b は校正モードで既知標準から算出し、コードの定数に反映する。
 */

#include <Arduino.h>
#include <Wire.h>
#include <EEPROM.h>

// USE_OLED はビルド時に -DUSE_OLED=0/1 で設定（デフォルト: OLED なし）
#if USE_OLED
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET  -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
#endif

// ---- ピン定義 ----
#define LED_265_PIN   D3    // PWM: 265nm UVC LED (A260)
#define LED_280_PIN   D5    // PWM: 280nm UVC LED (A280)
#define SENSOR_PIN    A0    // GUVA-S12SD analog output (ADC0 / PC0)
#define CALIB_BTN_PIN D7    // 校正モード切替（プルダウン＋ボタン→VCC）
#define ARM_SW_PIN    D8    // アーム連動スイッチ
#define ARM_USE_HALL  0    // 0:マクロスイッチ / 1:ホールセンサー（磁石）（プルアップ、押下=LOW）

// ---- パラメータ ----
#define LED_PWM       255   // 全点灯 (0-255)。出力不足時は下げる
#define AVG_SAMPLES   64    // 各測定の平均サンプル数
#define CALIB_MS      300   // LED点灯後の安定待ち(ms)
#define ARM_DEBounce  15    // アームスイッチデバウンス(ms)

const float REF_CONCENTRATION = 50.0; // dsDNA [ug/mL] @ A260=1.0 (1cm pathlength)

// ---- 校正用固定真値（濃度固定、ユーザ入力なし）----
const float DNA_TRUE_A260 = 1.0f;   // 50ug/mL DNA
const float DNA_TRUE_A280 = 0.47f;
const float PROT_TRUE_A260 = 0.074f; // 1% (w/v) protein
const float PROT_TRUE_A280 = 0.13f;

// ============================================================
// 校正係数（校正モードで既知標準から算出し、ここに反映する）
//   デフォルト: k=1, b=0 （未校正）。実測後に上書き。
// ============================================================
const float K260 = 1.0f; // A260 スケール因子（デフォルト/未校正）
const float B260 = 0.0f; // A260 オフセット
const float K280 = 1.0f; // A280 スケール因子
const float B280 = 0.0f; // A280 オフセット

// ---- 実行時校正係数（EEPROM読み込みで上書き。runMeasure はこれを使う）----
float gK260 = 1.0f, gB260 = 0.0f, gK280 = 1.0f, gB280 = 0.0f;

// ============================================================
// EEPROM保存用（LGT8F328P内蔵実物EEPROM、約1KB使用可）
//   アップロード毎に消去されるため、有効性判定用のマジック番号を持つ。
// ============================================================
#define CALIB_EEPROM_ADDR 0
#define CALIB_MAGIC       0x4E414E4F // "NANO"
#define CALIB_VERSION     1

struct CalibData {
  uint32_t magic;   // マジック番号（有効性判定）
  uint8_t  version; // バージョン
  float    k260, b260, k280, b280; // 校正係数
  uint32_t checksum; // 簡易チェックサム
} __attribute__((packed));

void saveCalibration(float k260, float b260, float k280, float b280);
bool loadCalibration(float& k260, float& b260, float& k280, float& b280);

// ============================================================
// 前方宣言
// ============================================================
void printScreen(const char* line1, const char* line2);
void showCalibResult(float k260, float b260, float k280, float b280);
void showResult(float a260, float a280, float ratio, float conc);
void runMeasure();
void runCalibration();
long measureReference(int ledPin);
long measureSample(int ledPin);

volatile bool armPressed = false;   // アームスイッチ割込フラグ
unsigned long lastArmTime = 0;      // デバウンス用

// ---- 通常測定の状態機械（指示付き：ブランク → サンプルの2段階）----
//   アームスイッチを1回押すたびに1ステップ進行する。
//     1) "PUT BLANK 1/2" でブランク(I0)を測定し、アームを押す
//     2) "SWAP SAMPLE 2/2" でサンプルに換えてアームを押す → 結果表示
//       size1: A260/A280 + Purity, size2: Conc=xx.x（大きく）, size1: "next: set BLANK"
//   結果はアーム押下までOLEDに保持。次のアームで自動的に次のブランク測定へ。
enum MeasureState { M_DO_I0, M_DO_I };
static MeasureState mState = M_DO_I0;   // 起動時はすぐにブランク測定可能
static bool measuring = false;          // 実行中の追加割込防止
static long g_i0_265, g_i0_280;         // ブランク（I0）を保持

// ---- アームスイッチ割込 ----
// マクロ: FALLING（押した時）/ ホール: RISING（磁石が近づいた時）に1回だけ測定
void armISR() {
  unsigned long now = millis();
  if (now - lastArmTime > ARM_DEBounce) {
    armPressed = true;
    lastArmTime = now;
  }
}

// ---- LED制御 ----
void ledOn(int pin, int pwm) { analogWrite(pin, pwm); }
void ledOff(int pin)         { analogWrite(pin, 0); }

long readAverage(int pin) {
  long sum = 0;
  for (int i = 0; i < AVG_SAMPLES; i++) sum += analogRead(pin);
  return sum / AVG_SAMPLES;
}

// ブランク（I0）測定
long measureReference(int ledPin) {
  ledOn(ledPin, LED_PWM); delay(CALIB_MS);
  long i0 = readAverage(SENSOR_PIN);
  ledOff(ledPin);
  return i0;
}

// サンプル（I）測定
long measureSample(int ledPin) {
  ledOn(ledPin, LED_PWM); delay(CALIB_MS);
  long i = readAverage(SENSOR_PIN);
  ledOff(ledPin);
  return i;
}

// 生吸光度 A_meas = -log10(I/I0)
float computeAbsorbance(long i, long i0) {
  if (i0 <= 1) return 9.99f;
  float ratio = (float)i / (float)i0;
  if (ratio <= 0.0001f) return 4.0f;
  return -10.0f * log10(ratio);
}

// 校正適用: A_true = k*A_meas + b
float calibrate(float a, float k, float b) {
  return k * a + b;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000) {}   // USBシリアル待ち

  pinMode(LED_265_PIN, OUTPUT);
  pinMode(LED_280_PIN, OUTPUT);
  pinMode(CALIB_BTN_PIN, INPUT);          // プルダウン（ボタン=VCC）
#if ARM_USE_HALL
  pinMode(ARM_SW_PIN, INPUT);             // ホール: 出力をそのまま読む（外付けプルダウン）
#else
  pinMode(ARM_SW_PIN, INPUT_PULLUP);      // マクロスイッチ: プルアップ、押下=LOW
#endif

#if ARM_USE_HALL
  attachInterrupt(digitalPinToInterrupt(ARM_SW_PIN), armISR, RISING);   // 磁石でHIGH
#else
  attachInterrupt(digitalPinToInterrupt(ARM_SW_PIN), armISR, FALLING);  // 押下でLOW
#endif

  analogReadResolution(12);               // 12-bit ADC (0-4095)
  analogReference(DEFAULT);               // AVCC基準

  digitalWrite(LED_265_PIN, LOW);
  digitalWrite(LED_280_PIN, LOW);

  Wire.begin();
#if USE_OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found");   // OLED無しでも測定は続行
  } else {
    display.display();
    delay(200);
  }
#endif

  printScreen("PUT BLANK 1/2", "press arm...");
  Serial.println("=== DIY Nanodrop (LGT8F328P) ready ===");

  // ---- EEPROMから校正係数を読み込む（未保存ならデフォルト1.0/0.0）----
  if (loadCalibration(gK260, gB260, gK280, gB280)) {
    Serial.println("Loaded calibration from EEPROM:");
    Serial.print("  K260="); Serial.print(gK260, 6);
    Serial.print(" B260="); Serial.print(gB260, 6);
    Serial.print(" | K280="); Serial.print(gK280, 6);
    Serial.print(" B280="); Serial.println(gB280, 6);
  } else {
    Serial.println("No valid calibration in EEPROM (using defaults).");
  }

  Serial.print("Calibration: K260="); Serial.print(gK260, 4);
  Serial.print(" B260="); Serial.print(gB260, 4);
  Serial.print(" | K280="); Serial.print(gK280, 4);
  Serial.print(" B280="); Serial.println(gB280, 4);
}

void printScreen(const char* line1, const char* line2) {
#if USE_OLED
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10);
  display.println(line1);
  if (line2) { display.setCursor(0, 38); display.println(line2); }
  display.display();
#endif
}

// 測定結果表示（保持中も見やすく、Concを大きく、ヒントを表示）
void showResult(float a260, float a280, float ratio, float conc) {
#if USE_OLED
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // size1: A260/A280 (1行まとめ)
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("A260="); display.print(a260, 2);
  display.print(" A280="); display.println(a280, 2);

  // size1: Purity
  display.setCursor(0, 10);
  display.print("Purity="); display.println(ratio, 2);

  // size2: Conc を大きく（小数1位）
  display.setTextSize(2);
  display.setCursor(0, 22);
  display.print("Conc="); display.println(conc, 1);

  // size1: ヒント
  display.setTextSize(1);
  display.setCursor(0, 48);
  display.println("next: set BLANK");

  display.display();
#endif
}

// ---- 校正結果をOLEDへ多行表示（K/B係数を表示）----
void showCalibResult(float k260, float b260, float k280, float b280) {
#if USE_OLED
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("CAL OK");
  display.println(String("K260=") + String(k260, 4));
  display.println(String("B260=") + String(b260, 4));
  display.println(String("K280=") + String(k280, 4));
  display.println(String("B280=") + String(b280, 4));
  display.display();
#endif
}

// ---- 通常測定（状態機械：指示付き ブランク → サンプル）----
//   アームスイッチを1回押すたびに1ステップ進行する。
//     ステップ1: "PUT BLANK 1/2" でブランク(I0)を両波長で測定 → "SWAP SAMPLE 2/2"
//     ステップ2: サンプルに換えてアームを押す → 結果を表示（アーム押下まで保持）
//       size1: A260/A280 + Purity
//       size2: Conc=xx.x （大きく）
//       size1: next: set BLANK
//     次のアームでブランクへ
void runMeasure() {
  if (measuring) return;   // 実行中に追加割込が入らないよう保護

  if (mState == M_DO_I0) {
    // ---- ステップ1: ブランク(I0)を両波長で測定 ----
    measuring = true;
    printScreen("PUT BLANK 1/2", "press arm...");
    Serial.println("--- MEASURE: BLANK ---");

    long i0_265 = measureReference(LED_265_PIN);
    long i0_280 = measureReference(LED_280_PIN);
    g_i0_265 = i0_265;
    g_i0_280 = i0_280;

    // 両ブランクが成立しているか確認（異常時はエラー表示）
    if (i0_265 <= 1 || i0_280 <= 1) {
      printScreen("MEAS ERR", "check BLANK");
      Serial.println("BLANK too low — check cuvette/window");
      mState = M_DO_I0;
      measuring = false;
      return;
    }

    // 次のステップへ：サンプルに交換するよう指示
    mState = M_DO_I;
    printScreen("SWAP SAMPLE 2/2", "press arm...");
    Serial.println("--- MEASURE: SAMPLE (swap cuvette) ---");
  } else {
    // ---- ステップ2: サンプルを両波長で測定し結果を表示 ----
    measuring = true;
    printScreen("MEASURING", "please wait...");
    Serial.println("--- MEASURE: SAMPLE ---");

    long i_265 = measureSample(LED_265_PIN);
    long i_280 = measureSample(LED_280_PIN);

    float a260_meas = computeAbsorbance(i_265, g_i0_265);
    float a280_meas = computeAbsorbance(i_280, g_i0_280);

    // 校正適用（EEPROM読み込み済みの実行時係数を使用）
    float a260 = calibrate(a260_meas, gK260, gB260);
    float a280 = calibrate(a280_meas, gK280, gB280);

    // 純度比（kの比も掛ける）
    float ratio = (a280 > 0.001f) ? a260 / a280 : 99.9f;
    float conc  = a260 * REF_CONCENTRATION; // dsDNA ug/mL

    showResult(a260, a280, ratio, conc);

    Serial.print("A260=");  Serial.print(a260, 3);
    Serial.print(" A280="); Serial.print(a280, 3);
    Serial.print(" Ratio=");Serial.print(ratio, 3);
    Serial.print(" Conc=");Serial.println(conc, 1);

    // 結果画面を保持（次のアーム押下まで）。アームを押すと次の測定（ブランク）へ進む。
    mState = M_DO_I0;
    measuring = false;
    Serial.println("--- RESULT HELD (press arm to continue) ---");
  }
}

// ---- 校正モード（固定濃度・arm駆動）----
//   順序: BLANK → 50ug/mL DNA → 1% protein
//   真値は固定（濃度固定）。Serial入力不要。
void runCalibration() {
  Serial.println("=== CALIBRATION (fixed standards) ===");
  armPressed = false;
  mState = M_DO_I0;  // 安全のため測定状態をリセット

  printScreen("CAL MODE", "arm to step");
  delay(600);

  // Step 1: BLANK
  printScreen("CAL 1/3 BLANK", "blank + arm");
  {
    unsigned long t0 = millis();
    while (!armPressed && (millis() - t0 < 60000)) delay(10);
  }
  if (!armPressed) { printScreen("CAL ERR", "timeout"); return; }
  armPressed = false;

  printScreen("CAL 1/3 BLANK", "measuring...");
  Serial.println("--- CALIB: BLANK ---");
  long i0_265 = measureReference(LED_265_PIN);
  long i0_280 = measureReference(LED_280_PIN);

  if (i0_265 <= 1 || i0_280 <= 1) {
    printScreen("CAL ERR", "check BLANK");
    Serial.println("BLANK too low — check cuvette/window");
    return;
  }

  // Step 2: DNA 50ug/mL
  printScreen("CAL 2/3 DNA", "DNA + arm");
  {
    unsigned long t0 = millis();
    while (!armPressed && (millis() - t0 < 60000)) delay(10);
  }
  if (!armPressed) { printScreen("CAL ERR", "timeout"); return; }
  armPressed = false;

  printScreen("CAL 2/3 DNA", "measuring...");
  Serial.println("--- CALIB: DNA ---");
  long d_265 = measureSample(LED_265_PIN);
  long d_280 = measureSample(LED_280_PIN);
  float dna_meas_260 = computeAbsorbance(d_265, i0_265);
  float dna_meas_280 = computeAbsorbance(d_280, i0_280);

  // Step 3: PROTEIN 1%
  printScreen("CAL 3/3 PROT", "PROT + arm");
  {
    unsigned long t0 = millis();
    while (!armPressed && (millis() - t0 < 60000)) delay(10);
  }
  if (!armPressed) { printScreen("CAL ERR", "timeout"); return; }
  armPressed = false;

  printScreen("CAL 3/3 PROT", "measuring...");
  Serial.println("--- CALIB: PROT ---");
  long p_265 = measureSample(LED_265_PIN);
  long p_280 = measureSample(LED_280_PIN);
  float prot_meas_260 = computeAbsorbance(p_265, i0_265);
  float prot_meas_280 = computeAbsorbance(p_280, i0_280);

  // 較正計算（固定真値使用）
  printScreen("CALC...", "please wait");
  float denom260 = (dna_meas_260 - prot_meas_260);
  float denom280 = (dna_meas_280 - prot_meas_280);

  if (abs(denom260) < 1e-4f || abs(denom280) < 1e-4f) {
    Serial.println("Calib error: standards too close");
    printScreen("CAL ERR", "check standards");
    return;
  }

  float nk260 = (PROT_TRUE_A260 - DNA_TRUE_A260) / denom260;
  float nb260 = DNA_TRUE_A260 - nk260 * dna_meas_260;
  float nk280 = (PROT_TRUE_A280 - DNA_TRUE_A280) / denom280;
  float nb280 = DNA_TRUE_A280 - nk280 * dna_meas_280;

  // 結果表示（4-5秒）
  showCalibResult(nk260, nb260, nk280, nb280);
  Serial.print("RESULT K260="); Serial.print(nk260, 4);
  Serial.print(" B260="); Serial.print(nb260, 4);
  Serial.print(" | K280="); Serial.print(nk280, 4);
  Serial.print(" B280="); Serial.println(nb280, 4);

  // 参考: ハードコード用（Serialで確認可）
  Serial.println("=== (reference) HARD-CODE VALUES ===");
  Serial.print("  const float K260 = "); Serial.print(nk260, 6); Serial.println(";");
  Serial.print("  const float B260 = "); Serial.print(nb260, 6); Serial.println(";");
  Serial.print("  const float K280 = "); Serial.print(nk280, 6); Serial.println(";");
  Serial.print("  const float B280 = "); Serial.print(nb280, 6); Serial.println(";");

  // EEPROM保存
  saveCalibration(nk260, nb260, nk280, nb280);

  // 結果（CAL OK + K/B）を4.5秒表示してからSAVED
  delay(4500);
  printScreen("CAL SAVED", "to EEPROM OK");
  Serial.println("Saved to EEPROM (applied on next boot).");
  delay(600);

  // 通常測定に戻る（状態リセット）
  mState = M_DO_I0;
  printScreen("PUT BLANK 1/2", "press arm...");
}

// ---- EEPROMへ校正係数を保存（LGT8F328P内蔵EEPROM）----
static uint32_t calcChecksum(const CalibData& d) {
  // 簡易チェックサム（k/bの4float分のみ）
  uint32_t s = 0;
  const uint8_t *p = (const uint8_t*)&d.k260;
  for (size_t i = 0; i < sizeof(float) * 4; i++) s += p[i];
  return s;
}

void saveCalibration(float k260, float b260, float k280, float b280) {
  CalibData d;
  d.magic = CALIB_MAGIC;
  d.version = CALIB_VERSION;
  d.k260 = k260; d.b260 = b260;
  d.k280 = k280; d.b280 = b280;
  d.checksum = calcChecksum(d);

  // put() は変更時のみ書き込み（E2PROM実装に依存）
  EEPROM.put(CALIB_EEPROM_ADDR, d);
}

// ---- EEPROMから校正係数を読み込む。有効ならtrue、失敗ならfalse----
bool loadCalibration(float& k260, float& b260, float& k280, float& b280) {
  CalibData d;
  EEPROM.get(CALIB_EEPROM_ADDR, d);

  if (d.magic != CALIB_MAGIC) return false;   // 未保存/消去済み
  if (d.version != CALIB_VERSION) return false;
  if (calcChecksum(d) != d.checksum) return false; // データ破損

  k260 = d.k260; b260 = d.b260;
  k280 = d.k280; b280 = d.b280;
  return true;
}

void loop() {
  // アームスイッチが押されたら測定実行
  if (armPressed) {
    armPressed = false;
    runMeasure();
  }

  // 校正ボタンが長押し（2秒）で校正モード
  static unsigned long calibStart = 0;
  if (!digitalRead(CALIB_BTN_PIN)) {
    if (calibStart == 0) calibStart = millis();
    else if (millis() - calibStart > 2000) {
      calibStart = 0;
      runCalibration();
    }
  } else {
    calibStart = 0;
  }
}
