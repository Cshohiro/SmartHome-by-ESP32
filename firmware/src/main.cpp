#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

/* ===== 基本設定 =====
 * WiFi（固定IP）と WebServer の基本設定
 */
const char* ssid = "F2.....EXT"; //省略
const char* pass = "....";　//省略

IPAddress local_IP(192,168,1,240);
IPAddress gateway(192,168,1,1);
IPAddress subnet(255,255,255,0);

WebServer server(80);

/* ===== IR 共通設定 =====
 * 送信ピンは AC / 照明で共通利用
 */
const uint16_t kIrLed = 27;
IRsend irsend(kIrLed);

/* ===== AC（Daikin）: 送信タイミング =====
 * 39byte（2フレーム）の生データを sendRaw で送信するためのタイミング定義
 */
#define HDR_MARK   3400
#define HDR_SPACE  1750
#define BIT_MARK   430
#define ONE_SPACE  1300
#define ZERO_SPACE 430
#define GAP        35000

static uint16_t rawBuffer[800];
static int rawIdx = 0;

/* ===== AC（Daikin）: テンプレート =====
 * Byte[26]=温度(×2), Byte[28]=風量
 * 変更後にチェックサム(Byte[19], Byte[38])を再計算して送信
 */
static const uint8_t COOL_TEMPLATE[39] = {
  0x11,0xDA,0x27,0x00,0x02,0x00,0x00,0x00,0x00,0x07,0x00,0x00,0x10,0x00,0x80,0x00,0x00,0x00,0x00,0xAB,
  0x11,0xDA,0x27,0x00,0x00,0x39,0x30,0x00,0xA0,0x00,0x00,0x06,0x60,0x00,0x00,0xC3,0x00,0x00,0x44
};

static const uint8_t HEAT_TEMPLATE[39] = {
  0x11,0xDA,0x27,0x00,0x02,0x00,0x00,0x00,0x00,0x03,0x00,0x00,0x30,0x00,0x80,0x00,0x00,0x00,0x00,0xC7,
  0x11,0xDA,0x27,0x00,0x00,0x49,0x35,0x00,0xA0,0x00,0x00,0x06,0x60,0x00,0x00,0xC3,0x00,0x00,0x59
};

static const uint8_t OFF_TEMPLATE[39] = {
  0x11,0xDA,0x27,0x00,0x02,0x00,0x00,0x00,0x00,0x02,0x00,0x80,0x10,0x00,0x80,0x00,0x00,0x00,0x00,0x26,
  0x11,0xDA,0x27,0x00,0x00,0x38,0x30,0x00,0x50,0x00,0x00,0x06,0x60,0x00,0x00,0xC3,0x00,0x00,0xF3
};

enum ACMode : uint8_t { MODE_HEAT = 0, MODE_COOL = 1 };

/* ===== AC（3F）状態 =====
 * UI表示と送信内容をこの状態に集約
 */
struct ACState {
  bool powerOn;
  int tempX2;     // 温度 * 2（0.5℃刻み）
  ACMode mode;
  uint8_t fanHex; // テンプレート Byte[28]
};

static ACState ac3F = { false, 55, MODE_HEAT, 0xA0 };

/* ===== 風量マップ =====
 * 実機検証した値（Byte[28]）
 */
static const uint8_t FAN_AUTO    = 0xA0;
static const uint8_t FAN_SHIZUKA = 0xB0;
static const uint8_t FAN_LV1     = 0x30;
static const uint8_t FAN_LV2     = 0x40;
static const uint8_t FAN_LV3     = 0x50;
static const uint8_t FAN_LV4     = 0x60;
static const uint8_t FAN_LV5     = 0x70;

/* ===== オフタイマー =====
 * timerMin で期限を設定し、loop() で期限超過を監視して停止送信
 */
static volatile uint64_t offDeadlineMs = 0;

/* ===== AC: バイト→生波形変換（LSB-first） ===== */
static void addByteToBuffer(uint8_t data) {
  for (int i = 0; i < 8; i++) {
    rawBuffer[rawIdx++] = BIT_MARK;
    rawBuffer[rawIdx++] = ((data >> i) & 1) ? ONE_SPACE : ZERO_SPACE;
  }
}

/* ===== AC: チェックサム計算 ===== */
static uint8_t sum8(const uint8_t* p, int len) {
  uint16_t s = 0;
  for (int i = 0; i < len; i++) s += p[i];
  return (uint8_t)(s & 0xFF);
}

static void fixDaikinChecksums(uint8_t data[39]) {
  data[19] = sum8(data, 19);
  data[38] = sum8(data + 20, 18);
}

/* ===== AC: 39byte（2フレーム）を一回送信 ===== */
static void send39BytesRawOnce(const uint8_t data[39]) {
  rawIdx = 0;

  // frame1: bytes[0..19]
  rawBuffer[rawIdx++] = HDR_MARK; rawBuffer[rawIdx++] = HDR_SPACE;
  for (int i = 0; i < 20; i++) addByteToBuffer(data[i]);
  rawBuffer[rawIdx++] = BIT_MARK; rawBuffer[rawIdx++] = GAP;

  // frame2: bytes[20..38]
  rawBuffer[rawIdx++] = HDR_MARK; rawBuffer[rawIdx++] = HDR_SPACE;
  for (int i = 20; i < 39; i++) addByteToBuffer(data[i]);
  rawBuffer[rawIdx++] = BIT_MARK; rawBuffer[rawIdx++] = 0;

  irsend.sendRaw(rawBuffer, rawIdx, 38);
}

/* ===== AC: 表示用風量名（日本語） ===== */
static String fanName(uint8_t h) {
  if (h == FAN_AUTO) return "自動";
  if (h == FAN_SHIZUKA) return "静音";
  if (h == FAN_LV1) return "1";
  if (h == FAN_LV2) return "2";
  if (h == FAN_LV3) return "3";
  if (h == FAN_LV4) return "4";
  if (h == FAN_LV5) return "5";
  return "不明";
}

/* ===== AC: 温度範囲クランプ（18.0〜32.0） ===== */
static void clampTemp(ACState& st) {
  if (st.tempX2 < 36) st.tempX2 = 36;
  if (st.tempX2 > 64) st.tempX2 = 64;
}

/* ===== AC: 状態から送信用39byteを生成して送信 ===== */
static void buildAndSendRun(const ACState& st) {
  if (!st.powerOn) return;

  uint8_t data[39];
  memcpy(data, (st.mode == MODE_COOL) ? COOL_TEMPLATE : HEAT_TEMPLATE, 39);

  int tX2 = st.tempX2;
  if (tX2 < 36) tX2 = 36;
  if (tX2 > 64) tX2 = 64;
  data[26] = (uint8_t)tX2;
  data[28] = st.fanHex;

  fixDaikinChecksums(data);
  send39BytesRawOnce(data);
}

/* ===== AC: 即時停止（OFFテンプレ送信） ===== */
static void sendOffNow() {
  send39BytesRawOnce(OFF_TEMPLATE);
  ac3F.powerOn = false;
  offDeadlineMs = 0;
}

/* ===== 照明（Panasonic HK9493）設定 =====
 * 複数フレームを繰り返し送信して成功率を上げる
 */
static const uint8_t  kFreqKHz = 38;
static const int kInterFrameDelayMs = 40;
static const int kRepeatCount = 6;
static const int kRepeatGapMs  = 100;

// ON (3 frames)
static uint16_t ON_f1[] = {3522,1718,446,418,448,418,448,1288,448,1288,446,420,448,1288,446,418,448,418,448,418,448,1288,446,418,446,420,448,1290,446,418,448,1288,448,418,448,1288,448,418,448,418,448,1288,446,420,446,418,448,418,448,418,448,418,446,420,448,1288,448,1290,448,418,448,1288,448,418,448,418,448,1288,446,420,448,1288,448,418,448,418,448,1290,448,418,446,420,448};
static uint16_t ON_f2[] = {444,420,446,420,446,420,446,418,446,1290,446,1292,446,420,446,1290,446,420,446,420,446,1292,446,420,446,1290,446,420,446,420,446,1290,446,420,446,422,446};
static uint16_t ON_f3[] = {444,420,446,1292,446,420,446,420,446,420,446,1292,446,420,446,420,446,1292,446,420,446,1290,446,420,444,1292,446,420,446,420,446,1292,446,420,446,420,446,420,446,420,446,420,446,420,446,420,446,1292,446,1290,446,422,446,1292,446,420,446,420,446,1292,446,420,444,1292,446,422,446,420,446,1292,446,422,444,422,446};

// NIGHT (2 frames)
static uint16_t NIGHT_f1[] = {3526,1716,448,418,448,418,448,1290,446,1290,446,418,448,1290,446,418,448,418,446,420,446,1290,448,418,446,420,448,1288,448,418,448,1290,446,418,448,1290,448,418,446,420,446,1290,446,418,448,418,448,418,448,418,446,420,446,1290,446,1290,446,1290,448,418,448,1290,446,420,448,420,446,1290,446,1290,446,1290,446,420,446,420,448,1290,446,420,446,420,448};
static uint16_t NIGHT_f2[] = {420,446,420,446,420,446,420,444,1292,446,1292,446,1290,446,420,444,1292,446,420,446,420,446,1290,446,1292,446,1290,446,420,446,420,444,1292,444,422,446,422,444};

// OFF (2 frames)
static uint16_t OFF_f1[] = {3526,1716,448,418,446,418,448,1290,446,1290,448,418,448,1290,446,418,448,418,448,418,448,1288,448,418,446,420,446,1290,446,418,448,1288,448,418,446,1290,446,420,446,420,446,1290,446,418,446,418,448,418,448,418,448,1288,448,1288,448,1290,446,1288,448,418,446,1290,446,420,446,420,446,418,448,1288,446,1290,448,418,446,420,446,1290,446,418,448,420,448};
static uint16_t OFF_f2[] = {418,446,420,446,420,446,1290,446,1292,446,1290,446,1292,446,420,446,1292,446,420,446,420,446,420,446,1290,446,1290,444,420,446,420,446,1290,446,420,446,422,446};

/* ===== 照明: フレーム送信ユーティリティ ===== */
static inline void sendRawArr(const uint16_t* arr, size_t len) {
  irsend.sendRaw(arr, len, kFreqKHz);
}

static void sendFrames(const uint16_t* f1, size_t n1,
                       const uint16_t* f2, size_t n2,
                       const uint16_t* f3, size_t n3,
                       int repeats) {
  delay(30);
  for (int r = 0; r < repeats; r++) {
    sendRawArr(f1, n1);
    if (f2 && n2) { delay(kInterFrameDelayMs); sendRawArr(f2, n2); }
    if (f3 && n3) { delay(kInterFrameDelayMs); sendRawArr(f3, n3); }
    delay(kRepeatGapMs);
  }
  delay(50);
}

static void light_on()    { sendFrames(ON_f1, sizeof(ON_f1)/2, ON_f2, sizeof(ON_f2)/2, ON_f3, sizeof(ON_f3)/2, kRepeatCount); }
static void light_night() { sendFrames(NIGHT_f1, sizeof(NIGHT_f1)/2, NIGHT_f2, sizeof(NIGHT_f2)/2, nullptr, 0, kRepeatCount); }
static void light_off()   { sendFrames(OFF_f1, sizeof(OFF_f1)/2, OFF_f2, sizeof(OFF_f2)/2, nullptr, 0, kRepeatCount); }

/* ===== LittleFS: 静的ファイル配信 =====
 * index.html / style.css / app.js などを ESP32 から配信
 */
static String contentType(const String& path) {
  if (path.endsWith(".html")) return "text/html; charset=utf-8";
  if (path.endsWith(".css"))  return "text/css; charset=utf-8";
  if (path.endsWith(".js"))   return "application/javascript; charset=utf-8";
  if (path.endsWith(".png"))  return "image/png";
  if (path.endsWith(".jpg"))  return "image/jpeg";
  if (path.endsWith(".svg"))  return "image/svg+xml";
  return "text/plain; charset=utf-8";
}

static bool serveFile(String path) {
  if (path == "/") path = "/index.html";
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  server.streamFile(f, contentType(path));
  f.close();
  return true;
}

/* ===== HTTP API: 状態取得（/api/ac3_state） ===== */
static void handleAC3State() {
  String modeStr = (ac3F.mode == MODE_HEAT) ? "heat" : "cool";
  String tempStr = String(ac3F.tempX2 / 2) + ((ac3F.tempX2 % 2) ? ".5" : ".0");
  String fanStr  = fanName(ac3F.fanHex);

  String left = "--";
  if (offDeadlineMs != 0) {
    uint64_t now = millis();
    left = (offDeadlineMs > now) ? String((offDeadlineMs - now) / 1000) + "s" : "0s";
  }

  String json = "{";
  json += "\"power\":" + String(ac3F.powerOn ? "true" : "false") + ",";
  json += "\"mode\":\"" + modeStr + "\",";
  json += "\"temp\":\"" + tempStr + "\",";
  json += "\"fan\":\"" + fanStr + "\",";
  json += "\"timer_left\":\"" + left + "\"";
  json += "}";
  server.send(200, "application/json; charset=utf-8", json);
}

/* ===== HTTP API: 操作（/api/ac3） ===== */
static void handleAC3Api() {
  if (server.hasArg("mode")) {
    String m = server.arg("mode");
    if (m == "heat") { ac3F.mode = MODE_HEAT; ac3F.tempX2 = 55; }
    else if (m == "cool") { ac3F.mode = MODE_COOL; ac3F.tempX2 = 48; }

    ac3F.powerOn = true;
    if (ac3F.fanHex == 0) ac3F.fanHex = FAN_AUTO;

    clampTemp(ac3F);
    buildAndSendRun(ac3F);
    server.send(200, "text/plain; charset=utf-8", "送信完了：モード");
    return;
  }

  if (server.hasArg("tempStep")) {
    float step = server.arg("tempStep").toFloat();
    ac3F.tempX2 += (step > 0) ? 1 : -1;
    clampTemp(ac3F);
    if (ac3F.powerOn) buildAndSendRun(ac3F);
    server.send(200, "text/plain; charset=utf-8", "送信完了：温度");
    return;
  }

  if (server.hasArg("fan")) {
    String f = server.arg("fan");
    if      (f == "auto")  ac3F.fanHex = FAN_AUTO;
    else if (f == "quiet") ac3F.fanHex = FAN_SHIZUKA;
    else if (f == "1")     ac3F.fanHex = FAN_LV1;
    else if (f == "2")     ac3F.fanHex = FAN_LV2;
    else if (f == "3")     ac3F.fanHex = FAN_LV3;
    else if (f == "4")     ac3F.fanHex = FAN_LV4;
    else if (f == "5")     ac3F.fanHex = FAN_LV5;

    if (ac3F.powerOn) buildAndSendRun(ac3F);
    server.send(200, "text/plain; charset=utf-8", "送信完了：風量");
    return;
  }

  if (server.hasArg("power")) {
    int p = server.arg("power").toInt();
    if (p == 0) {
      sendOffNow();
      server.send(200, "text/plain; charset=utf-8", "送信完了：停止");
      return;
    }
  }

  if (server.hasArg("timerMin")) {
    int mins = server.arg("timerMin").toInt();
    if (mins <= 0) {
      server.send(400, "text/plain; charset=utf-8", "分の値が不正です");
      return;
    }
    offDeadlineMs = (uint64_t)millis() + (uint64_t)mins * 60ULL * 1000ULL;
    server.send(200, "text/plain; charset=utf-8", "設定完了：オフタイマー");
    return;
  }

  if (server.hasArg("timerCancel")) {
    offDeadlineMs = 0;
    server.send(200, "text/plain; charset=utf-8", "取消完了：オフタイマー");
    return;
  }

  server.send(400, "text/plain; charset=utf-8", "Bad Request");
}

/* ===== HTTP API: 照明（/api/light） ===== */
static void handleLightApi() {
  if (!server.hasArg("cmd")) {
    server.send(400, "text/plain; charset=utf-8", "Bad Request");
    return;
  }
  String c = server.arg("cmd");
  if (c == "on") {
    light_on();
    server.send(200, "text/plain; charset=utf-8", "送信完了：全灯点灯");
  } else if (c == "night") {
    light_night();
    server.send(200, "text/plain; charset=utf-8", "送信完了：常夜灯");
  } else if (c == "off") {
    light_off();
    server.send(200, "text/plain; charset=utf-8", "送信完了：消灯");
  } else {
    server.send(400, "text/plain; charset=utf-8", "Unknown cmd");
  }
}

/* ===== 初期化 =====
 * FSマウント → WiFi接続（固定IP）→ ルーティング登録 → サーバ起動
 */
void setup() {
  Serial.begin(115200);
  irsend.begin();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
  }

  WiFi.mode(WIFI_STA);

  IPAddress dns1(192, 168, 1, 1);
  IPAddress dns2(8, 8, 8, 8);
  if (!WiFi.config(local_IP, gateway, subnet, dns1, dns2)) {
    Serial.println("WiFi.config failed (static IP)");
  }

  WiFi.begin(ssid, pass);
  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("WiFi接続完了（固定IP）");
  Serial.print("URL: http://");
  Serial.println(WiFi.localIP());

  server.serveStatic("/favicon.ico", LittleFS, "/favicon.ico");

  server.on("/api/ac3", HTTP_GET, handleAC3Api);
  server.on("/api/ac3_state", HTTP_GET, handleAC3State);
  server.on("/api/light", HTTP_GET, handleLightApi);

  server.onNotFound([]() {
    if (!serveFile(server.uri())) {
      server.send(404, "text/plain; charset=utf-8", "Not Found");
    }
  });

  server.begin();
  Serial.println("HTTP server started");
}

/* ===== メインループ =====
 * オフタイマー監視とHTTP処理
 */
void loop() {
  if (offDeadlineMs != 0) {
    uint64_t now = millis();
    if ((int64_t)(now - offDeadlineMs) >= 0) {
      sendOffNow();
    }
  }
  server.handleClient();
}

