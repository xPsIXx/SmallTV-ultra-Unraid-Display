// SmallTV-Ultra UnraidClaw dashboard
// Arduino IDE: File > Open this file. Board = Generic ESP8266 Module.
// TFT_eSPI must be pointed at Setup_SmallTV_Ultra.h via User_Setup_Select.h

// SmallTV-Ultra / SmallTV dashboard for UnraidClaw
// Polls https://<unraid>:9876 with x-api-key and draws CPU load, memory,
// array, docker and VM counts on the 240x240 ST7789.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <LittleFS.h>

#ifdef ESP32
  #include <WiFi.h>
  #include <WiFiClientSecure.h>
  #include <HTTPClient.h>
  #include <WebServer.h>
  #include <ESPmDNS.h>
  using HttpServer = WebServer;
#else
  #include <ESP8266WiFi.h>
  #include <WiFiClientSecure.h>
  #include <ESP8266HTTPClient.h>
  #include <ESP8266WebServer.h>
  #include <ESP8266mDNS.h>
  using HttpServer = ESP8266WebServer;
#endif

#include "config.h"

TFT_eSPI tft;
HttpServer server(80);

// ---- persisted settings ---------------------------------------------------
struct Settings {
  char host[MAX_HOST_LEN];
  uint16_t port;
  bool useHttps;
  bool insecureTls;
  char apiKey[MAX_KEY_LEN];
  uint16_t pollMs;
  uint8_t brightness;
  bool invertBl;
} cfg;

// ---- live stats -----------------------------------------------------------
struct Stats {
  bool haveMetrics = false;
  bool haveInfo = false;
  bool haveArray = false;
  bool lastOk = false;
  char lastError[48] = "not polled";
  uint32_t lastOkMs = 0;
  uint32_t lastFailMs = 0;

  char hostname[24] = "unraid";
  char cpuModel[40] = "";
  int cores = 0;
  int threads = 0;
  float load1 = 0, load5 = 0, load15 = 0;
  float memUsedPct = 0;
  uint64_t memUsed = 0, memTotal = 0;
  uint64_t uptimeSec = 0;

  char arrayState[16] = "-";
  float arrayUsedPct = -1;
  uint64_t arrayUsedKB = 0, arrayTotalKB = 0;

  int dockerRunning = -1;
  int dockerTotal = -1;
  int vmsRunning = -1;
  int vmsTotal = -1;
} st;

uint32_t lastFastPoll = 0;
uint32_t lastSlowPoll = 0;
bool portalActive = false;

// Unraid orange + dark chrome
static const uint16_t COL_BG     = 0x10A2; // ~#111318
static const uint16_t COL_PANEL  = 0x2124;
static const uint16_t COL_ORANGE = 0xFD20; // #FF8C00-ish
static const uint16_t COL_TEXT   = 0xEF7D;
static const uint16_t COL_DIM    = 0x8410;
static const uint16_t COL_GREEN  = 0x07E0;
static const uint16_t COL_RED    = 0xF800;
static const uint16_t COL_YELLOW = 0xFFE0;
static const uint16_t COL_BARBG  = 0x3186;
static const uint16_t COL_CYAN   = 0x07FF;

static void setBacklight(uint8_t bri) {
#ifdef TFT_BL
  pinMode(TFT_BL, OUTPUT);
#ifdef ESP32
  analogWrite(TFT_BL, cfg.invertBl ? (255 - bri) : bri);
#else
  analogWrite(TFT_BL, cfg.invertBl ? (255 - bri) : bri);
#endif
#else
  (void)bri;
#endif
}

static void defaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.port = DEFAULT_PORT;
  cfg.useHttps = DEFAULT_HTTPS;
  cfg.insecureTls = DEFAULT_INSECURE_TLS;
  cfg.pollMs = DEFAULT_POLL_MS;
  cfg.brightness = DEFAULT_BRIGHTNESS;
  cfg.invertBl = true;
}

static void loadSettings() {
  defaults();
  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  File f = LittleFS.open(SETTINGS_PATH, "r");
  if (!f) return;
  JsonDocument doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    strlcpy(cfg.host, doc["host"] | "", MAX_HOST_LEN);
    cfg.port = doc["port"] | DEFAULT_PORT;
    cfg.useHttps = doc["https"] | DEFAULT_HTTPS;
    cfg.insecureTls = doc["insecure"] | DEFAULT_INSECURE_TLS;
    strlcpy(cfg.apiKey, doc["key"] | "", MAX_KEY_LEN);
    cfg.pollMs = doc["poll"] | DEFAULT_POLL_MS;
    cfg.brightness = doc["bri"] | DEFAULT_BRIGHTNESS;
    cfg.invertBl = doc["invbl"] | true;
  }
  f.close();
}

static void saveSettings() {
  JsonDocument doc;
  doc["host"] = cfg.host;
  doc["port"] = cfg.port;
  doc["https"] = cfg.useHttps;
  doc["insecure"] = cfg.insecureTls;
  doc["key"] = cfg.apiKey;
  doc["poll"] = cfg.pollMs;
  doc["bri"] = cfg.brightness;
  doc["invbl"] = cfg.invertBl;
  File f = LittleFS.open(SETTINGS_PATH, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

static String fmtBytes(uint64_t b) {
  char buf[20];
  if (b >= (1ull << 40)) snprintf(buf, sizeof(buf), "%.1fT", b / 1099511627776.0);
  else if (b >= (1ull << 30)) snprintf(buf, sizeof(buf), "%.1fG", b / 1073741824.0);
  else if (b >= (1ull << 20)) snprintf(buf, sizeof(buf), "%.0fM", b / 1048576.0);
  else snprintf(buf, sizeof(buf), "%llu", (unsigned long long)b);
  return String(buf);
}

static String fmtKB(uint64_t kb) { return fmtBytes(kb * 1024ull); }

static String ago(uint32_t ms) {
  if (!ms) return "-";
  uint32_t s = (millis() - ms) / 1000;
  char buf[16];
  if (s < 60) snprintf(buf, sizeof(buf), "%lus", (unsigned long)s);
  else snprintf(buf, sizeof(buf), "%lum", (unsigned long)(s / 60));
  return String(buf);
}

static void drawBar(int x, int y, int w, int h, float pct, uint16_t col) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  tft.fillRoundRect(x, y, w, h, 3, COL_BARBG);
  int fw = (int)((w * pct) / 100.0f + 0.5f);
  if (fw > 0) tft.fillRoundRect(x, y, fw, h, 3, col);
}

static uint16_t heat(float pct) {
  if (pct < 0) return COL_DIM;
  if (pct < 60) return COL_GREEN;
  if (pct < 85) return COL_YELLOW;
  return COL_RED;
}

static void splash(const char *line1, const char *line2 = "") {
  tft.fillScreen(COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_ORANGE, COL_BG);
  tft.drawString("UNRAID", 120, 90, 4);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString(line1, 120, 130, 2);
  if (line2[0]) {
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString(line2, 120, 154, 2);
  }
}

static void drawDashboard() {
  tft.fillScreen(COL_BG);

  // header
  tft.fillRect(0, 0, 240, 36, COL_PANEL);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_ORANGE, COL_PANEL);
  tft.drawString("UNRAID", 8, 4, 2);
  tft.setTextColor(COL_TEXT, COL_PANEL);
  tft.drawString(st.hostname, 8, 20, 1);

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(st.lastOk ? COL_GREEN : COL_RED, COL_PANEL);
  tft.drawString(st.lastOk ? "LIVE" : "WAIT", 232, 6, 2);
  tft.setTextColor(COL_DIM, COL_PANEL);
  tft.drawString(ago(st.lastOkMs), 232, 22, 1);

  int y = 46;

  // CPU / load — UnraidClaw exposes loadavg, not %busy.
  // Normalise 1m load against thread count when we have it.
  float cpuPct = -1;
  if (st.threads > 0) cpuPct = (st.load1 / (float)st.threads) * 100.0f;
  else if (st.haveMetrics) cpuPct = min(st.load1 * 25.0f, 100.0f);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("CPU LOAD", 10, y, 1);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  {
    char buf[24];
    snprintf(buf, sizeof(buf), "1m %.2f", st.load1);
    tft.drawString(buf, 230, y, 1);
  }
  y += 12;
  drawBar(10, y, 220, 12, cpuPct < 0 ? 0 : cpuPct, heat(cpuPct));
  y += 16;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  {
    char buf[48];
    if (st.threads)
      snprintf(buf, sizeof(buf), "5m %.2f   15m %.2f   %dT", st.load5, st.load15, st.threads);
    else
      snprintf(buf, sizeof(buf), "5m %.2f   15m %.2f", st.load5, st.load15);
    tft.drawString(buf, 10, y, 1);
  }
  y += 16;

  // Memory
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("MEMORY", 10, y, 1);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.0f%%", st.memUsedPct);
    tft.drawString(buf, 230, y, 1);
  }
  y += 12;
  drawBar(10, y, 220, 12, st.memUsedPct, heat(st.memUsedPct));
  y += 16;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  {
    String line = fmtBytes(st.memUsed) + " / " + fmtBytes(st.memTotal);
    tft.drawString(line, 10, y, 1);
  }
  y += 16;

  // Array
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("ARRAY", 10, y, 1);
  tft.setTextDatum(TR_DATUM);
  uint16_t stateCol = COL_DIM;
  if (!strcasecmp(st.arrayState, "STARTED") || !strcasecmp(st.arrayState, "START"))
    stateCol = COL_GREEN;
  else if (st.arrayState[0] && strcmp(st.arrayState, "-"))
    stateCol = COL_YELLOW;
  tft.setTextColor(stateCol, COL_BG);
  tft.drawString(st.arrayState, 230, y, 1);
  y += 12;
  float ap = st.arrayUsedPct < 0 ? 0 : st.arrayUsedPct;
  drawBar(10, y, 220, 12, ap, heat(ap));
  y += 16;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_DIM, COL_BG);
  if (st.arrayTotalKB > 0) {
    String line = fmtKB(st.arrayUsedKB) + " / " + fmtKB(st.arrayTotalKB);
    if (st.arrayUsedPct >= 0) line += "  " + String(st.arrayUsedPct, 0) + "%";
    tft.drawString(line, 10, y, 1);
  } else {
    tft.drawString("waiting for array data", 10, y, 1);
  }
  y += 20;

  // Docker + VM tiles
  auto tile = [](int x, const char *title, int run, int tot, uint16_t accent) {
    tft.fillRoundRect(x, 196, 108, 38, 6, COL_PANEL);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(accent, COL_PANEL);
    tft.drawString(title, x + 8, 200, 1);
    tft.setTextColor(COL_TEXT, COL_PANEL);
    char buf[20];
    if (run < 0) snprintf(buf, sizeof(buf), "--");
    else if (tot < 0) snprintf(buf, sizeof(buf), "%d", run);
    else snprintf(buf, sizeof(buf), "%d / %d", run, tot);
    tft.drawString(buf, x + 8, 214, 2);
  };
  tile(8, "DOCKER", st.dockerRunning, st.dockerTotal, COL_CYAN);
  tile(124, "VMS", st.vmsRunning, st.vmsTotal, COL_ORANGE);

  if (!st.lastOk) {
    tft.setTextDatum(BC_DATUM);
    tft.setTextColor(COL_RED, COL_BG);
    tft.drawString(st.lastError, 120, 192, 1);
  }
}

// ---- HTTP helpers ---------------------------------------------------------
static bool httpGet(const char *path, JsonDocument &doc) {
  if (!cfg.host[0] || !cfg.apiKey[0]) {
    strlcpy(st.lastError, "set host + API key", sizeof(st.lastError));
    st.lastOk = false;
    return false;
  }

  char url[192];
  snprintf(url, sizeof(url), "%s://%s:%u%s",
           cfg.useHttps ? "https" : "http",
           cfg.host, (unsigned)cfg.port, path);

  WiFiClientSecure secure;
  WiFiClient plain;
#ifdef ESP32
  if (cfg.insecureTls) secure.setInsecure();
#else
  secure.setInsecure();
  secure.setBufferSizes(1024, 512);
#endif

  HTTPClient http;
  http.setTimeout(8000);
  http.setReuse(false);
  bool okBegin;
  if (cfg.useHttps) okBegin = http.begin(secure, url);
  else okBegin = http.begin(plain, url);
  if (!okBegin) {
    strlcpy(st.lastError, "http begin fail", sizeof(st.lastError));
    st.lastOk = false;
    return false;
  }
  http.addHeader("x-api-key", cfg.apiKey);
  http.addHeader("Accept", "application/json");

  int code = http.GET();
  if (code != 200) {
    snprintf(st.lastError, sizeof(st.lastError), "HTTP %d %s", code, path);
    st.lastOk = false;
    http.end();
    return false;
  }

  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    snprintf(st.lastError, sizeof(st.lastError), "json %s", err.c_str());
    st.lastOk = false;
    return false;
  }
  if (doc["ok"] != true) {
    strlcpy(st.lastError, "api ok=false", sizeof(st.lastError));
    st.lastOk = false;
    return false;
  }
  st.lastOk = true;
  st.lastOkMs = millis();
  st.lastError[0] = 0;
  return true;
}

static void pollMetrics() {
  JsonDocument doc;
  if (!httpGet("/api/system/metrics", doc)) return;
  JsonObject data = doc["data"];
  if (data["memory"].is<JsonObject>()) {
    st.memUsedPct = data["memory"]["usedPercent"] | 0.0f;
    st.memUsed = data["memory"]["usedBytes"] | 0;
    st.memTotal = data["memory"]["totalBytes"] | 0;
  }
  if (data["cpuLoad"].is<JsonObject>()) {
    st.load1 = data["cpuLoad"]["load1m"] | 0.0f;
    st.load5 = data["cpuLoad"]["load5m"] | 0.0f;
    st.load15 = data["cpuLoad"]["load15m"] | 0.0f;
  }
  st.haveMetrics = true;
}

static void pollInfo() {
  JsonDocument doc;
  if (!httpGet("/api/system/info", doc)) return;
  JsonObject data = doc["data"];
  if (data["os"].is<JsonObject>()) {
    const char *hn = data["os"]["hostname"] | st.hostname;
    strlcpy(st.hostname, hn, sizeof(st.hostname));
    // uptime may be seconds or a string depending on GraphQL scalar
    if (data["os"]["uptime"].is<uint64_t>())
      st.uptimeSec = data["os"]["uptime"];
  }
  if (data["cpu"].is<JsonObject>()) {
    const char *m = data["cpu"]["model"] | "";
    strlcpy(st.cpuModel, m, sizeof(st.cpuModel));
    st.cores = data["cpu"]["cores"] | 0;
    st.threads = data["cpu"]["threads"] | 0;
  }
  if (data["memory"].is<JsonObject>()) {
    st.memUsedPct = data["memory"]["usedPercent"] | st.memUsedPct;
    st.memUsed = data["memory"]["usedBytes"] | st.memUsed;
    st.memTotal = data["memory"]["totalBytes"] | st.memTotal;
  }
  if (data["cpuLoad"].is<JsonObject>()) {
    st.load1 = data["cpuLoad"]["load1m"] | st.load1;
    st.load5 = data["cpuLoad"]["load5m"] | st.load5;
    st.load15 = data["cpuLoad"]["load15m"] | st.load15;
  }
  st.haveInfo = true;
}

static void pollArray() {
  JsonDocument doc;
  if (!httpGet("/api/array/status", doc)) return;
  JsonObject data = doc["data"].is<JsonObject>() ? doc["data"].as<JsonObject>() : doc.as<JsonObject>();
  // UnraidClaw wraps {ok,data}; data is the array object
  if (doc["data"].is<JsonObject>()) data = doc["data"];
  const char *state = data["state"] | "-";
  strlcpy(st.arrayState, state, sizeof(st.arrayState));
  JsonObject cap = data["capacity"]["kilobytes"];
  if (!cap.isNull()) {
    st.arrayUsedKB = cap["used"] | 0;
    st.arrayTotalKB = cap["total"] | 0;
    if (st.arrayTotalKB > 0)
      st.arrayUsedPct = (100.0f * (float)st.arrayUsedKB) / (float)st.arrayTotalKB;
  }
  st.haveArray = true;
}

static void pollDocker() {
  JsonDocument doc;
  if (!httpGet("/api/docker/containers", doc)) return;
  JsonVariant list = doc["data"];
  // sometimes nested under data.containers
  if (list.is<JsonObject>() && list["containers"].is<JsonArray>())
    list = list["containers"];
  if (!list.is<JsonArray>()) return;
  int total = 0, run = 0;
  for (JsonObject c : list.as<JsonArray>()) {
    total++;
    const char *s = c["state"] | "";
    if (!strcasecmp(s, "running") || !strcasecmp(s, "STARTED")) run++;
  }
  st.dockerTotal = total;
  st.dockerRunning = run;
}

static void pollVms() {
  JsonDocument doc;
  if (!httpGet("/api/vms", doc)) return;
  JsonVariant list = doc["data"];
  if (list.is<JsonObject>() && list["vms"].is<JsonArray>())
    list = list["vms"];
  if (!list.is<JsonArray>()) return;
  int total = 0, run = 0;
  for (JsonObject v : list.as<JsonArray>()) {
    total++;
    const char *s = v["state"] | "";
    if (!strcasecmp(s, "running") || !strcasecmp(s, "STARTED") ||
        !strcasecmp(s, "running"))
      run++;
  }
  st.vmsTotal = total;
  st.vmsRunning = run;
}

// ---- config web UI --------------------------------------------------------
static const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>SmallTV UnraidClaw</title>
<style>
body{font-family:sans-serif;background:#111318;color:#eee;margin:20px}
label{display:block;margin-top:12px;color:#aaa;font-size:13px}
input,select{width:100%;padding:8px;border-radius:6px;border:1px solid #333;background:#1b1e26;color:#fff;box-sizing:border-box}
button{margin-top:16px;padding:10px 16px;background:#ff8c2f;border:0;border-radius:6px;font-weight:700}
.note{color:#888;font-size:12px;margin-top:8px}
</style></head><body>
<h2>SmallTV &rarr; UnraidClaw</h2>
<form method=POST action=/save>
<label>Unraid IP / hostname</label>
<input name=host value="%HOST%" placeholder="192.168.1.10">
<label>Port</label>
<input name=port type=number value="%PORT%">
<label>Use HTTPS</label>
<select name=https><option value=1 %H1%>yes (default)</option><option value=0 %H0%>no</option></select>
<label>Skip TLS verify (self-signed cert)</label>
<select name=insecure><option value=1 %I1%>yes (needed for UnraidClaw)</option><option value=0 %I0%>no</option></select>
<label>UnraidClaw API key</label>
<input name=key value="%KEY%" placeholder="paste key from Settings &gt; UnraidClaw">
<label>Poll interval (ms)</label>
<input name=poll type=number value="%POLL%">
<label>Brightness 0-255</label>
<input name=bri type=number value="%BRI%">
<label>Backlight inverted</label>
<select name=invbl><option value=1 %B1%>yes (SmallTV default)</option><option value=0 %B0%>no</option></select>
<button>Save</button>
</form>
<p class=note>Firmware %FW% &middot; grant the key <code>info:read</code>, <code>array:read</code>, <code>docker:read</code>, <code>vms:read</code>. UnraidClaw does not currently expose GPU utilisation — CPU load averages and memory are shown instead.</p>
</body></html>
)HTML";

static String htmlEscape(const String &s) {
  String o = s;
  o.replace("&", "&amp;");
  o.replace("\"", "&quot;");
  o.replace("<", "&lt;");
  return o;
}

static void handleRoot() {
  String page = FPSTR(PAGE);
  page.replace("%HOST%", htmlEscape(cfg.host));
  page.replace("%PORT%", String(cfg.port));
  page.replace("%H1%", cfg.useHttps ? "selected" : "");
  page.replace("%H0%", cfg.useHttps ? "" : "selected");
  page.replace("%I1%", cfg.insecureTls ? "selected" : "");
  page.replace("%I0%", cfg.insecureTls ? "" : "selected");
  page.replace("%KEY%", htmlEscape(cfg.apiKey));
  page.replace("%POLL%", String(cfg.pollMs));
  page.replace("%BRI%", String(cfg.brightness));
  page.replace("%B1%", cfg.invertBl ? "selected" : "");
  page.replace("%B0%", cfg.invertBl ? "" : "selected");
  page.replace("%FW%", FW_VERSION);
  server.send(200, "text/html", page);
}

static void handleSave() {
  strlcpy(cfg.host, server.arg("host").c_str(), MAX_HOST_LEN);
  cfg.port = server.arg("port").toInt();
  if (!cfg.port) cfg.port = DEFAULT_PORT;
  cfg.useHttps = server.arg("https") != "0";
  cfg.insecureTls = server.arg("insecure") != "0";
  strlcpy(cfg.apiKey, server.arg("key").c_str(), MAX_KEY_LEN);
  cfg.pollMs = server.arg("poll").toInt();
  if (cfg.pollMs < 2000) cfg.pollMs = 2000;
  cfg.brightness = constrain(server.arg("bri").toInt(), 0, 255);
  cfg.invertBl = server.arg("invbl") != "0";
  saveSettings();
  setBacklight(cfg.brightness);
  lastFastPoll = 0;
  lastSlowPoll = 0;
  server.send(200, "text/html",
              "<meta http-equiv=refresh content='2;url=/'>"
              "<body style='background:#111;color:#eee;font-family:sans-serif'>"
              "Saved. Polling UnraidClaw&hellip;</body>");
}

static void startWeb() {
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status", []() {
    JsonDocument doc;
    doc["ok"] = st.lastOk;
    doc["host"] = st.hostname;
    doc["load1"] = st.load1;
    doc["mem"] = st.memUsedPct;
    doc["array"] = st.arrayState;
    doc["err"] = st.lastError;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
  });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(50);

#ifdef TFT_BL
  pinMode(TFT_BL, OUTPUT);
#endif

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(COL_BG);

  loadSettings();
  setBacklight(cfg.brightness);

  splash("connecting WiFi");

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setHostname("smalltv-unraid");
  bool connected = wm.autoConnect(AP_SSID);
  if (!connected) {
    splash("WiFi setup", "join SmallTV-Unraid");
    // stay in portal until reboot — autoConnect already blocked
  }

  splash(WiFi.localIP().toString().c_str(), "open this IP in a browser");
  MDNS.begin("smalltv-unraid");
  startWeb();

  if (cfg.host[0] && cfg.apiKey[0]) {
    pollInfo();
    pollMetrics();
    pollArray();
    pollDocker();
    pollVms();
  }
  drawDashboard();
  lastFastPoll = millis();
  lastSlowPoll = millis();
}

void loop() {
  server.handleClient();
#ifdef ESP8266
  MDNS.update();
#endif

  uint32_t now = millis();
  if (cfg.host[0] && cfg.apiKey[0] && now - lastFastPoll >= cfg.pollMs) {
    lastFastPoll = now;
    pollMetrics();
    drawDashboard();
  }
  if (cfg.host[0] && cfg.apiKey[0] && now - lastSlowPoll >= DEFAULT_SLOW_POLL_MS) {
    lastSlowPoll = now;
    pollInfo();
    pollArray();
    pollDocker();
    pollVms();
    drawDashboard();
  }
}
