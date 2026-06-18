/*
  ESP32-C3 Signal Light — DEBUG firmware.

  Same protocol as esp32c3_signal_light.ino, but with verbose serial output:
    [WIFI]  status changes + IP
    [CONN]  client connects / drops
    [RX  ]  raw line from daemon (truncated)
    [EFF ]  parsed frame count + first frame
    [PING] / [PONG]
    [WDG ]  watchdog reboot warning

  Use this before wiring LEDs to confirm the link is alive on Serial Monitor.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>

// Add as many Wi-Fi networks as you want. The board tries them in order on
// every reconnect cycle and stays on the first one that works.
struct WifiCred { const char *ssid; const char *password; };
const WifiCred WIFI_NETWORKS[] = {
  { "YOUR_WIFI_SSID",      "YOUR_WIFI_PASSWORD"      },
  // { "Secondary_SSID",   "secondary_password"      },
  // { "Phone_Hotspot",    "hotspot_password"        },
};
const size_t WIFI_NETWORKS_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);
const uint32_t WIFI_PER_NETWORK_TIMEOUT_MS = 8000;

// Wiring confirmed on this board (ESP32-C3 SuperMini + KR2311 traffic-light module):
//   3.3V  → common anode (any of the + pads on the LED module)
//   GPIO5 → 330Ω → RL- (red cathode)
//   GPIO6 → 330Ω → YL- (yellow cathode)
//   GPIO7 → 330Ω → GL- (green cathode)
// Common-anode means GPIO LOW = LED ON, GPIO HIGH = LED OFF → ACTIVE_LOW = true.
const uint8_t PIN_RED    = 5;
const uint8_t PIN_YELLOW = 6;
const uint8_t PIN_GREEN  = 7;
const bool    ACTIVE_LOW = true;

const uint16_t TCP_PORT         = 8080;
const uint32_t WATCHDOG_MS      = 30000;
const uint16_t CLIENT_RX_BUFFER = 2048;

struct Frame { uint8_t r, y, g; uint16_t ms; };
const size_t MAX_FRAMES = 32;

WiFiServer server(TCP_PORT);
WiFiClient currentClient;
Frame effect[MAX_FRAMES];
size_t effectLen = 0, frameIdx = 0;
uint32_t frameStartedMs = 0;
float effectTempo = 1.0f;
uint32_t lastDaemonMsgMs = 0;
uint32_t lastWifiAttemptMs = 0;
char lineBuf[CLIENT_RX_BUFFER];
size_t lineLen = 0;

// ===== Software PWM driven by a hardware timer ISR =====
// We avoid ESP32's hardware LEDC (analogWrite) because once a GPIO gets
// attached to a LEDC channel on ESP32-C3, subsequent digitalWrite() calls
// become unreliable — "OFF doesn't actually turn off".
//
// Earlier this PWM ran from loop() — but that meant any Wi-Fi or TCP work in
// loop() would stretch the gap between PWM ticks and the LED would visibly
// "dip" every ~1 s. Moving the tick into a hardware timer ISR guarantees a
// rock-steady cadence regardless of what loop() is doing.
//
// Levels: 16. Period: 2 ms (≈ 500 Hz refresh). Slot tick: 125 us.
const uint8_t  SW_PWM_LEVELS = 16;
volatile uint8_t pwmDutyR = 0, pwmDutyY = 0, pwmDutyG = 0;
volatile uint8_t pwmSlot = 0;
hw_timer_t *pwmTimer = nullptr;

static inline void hardOn (uint8_t pin) { digitalWrite(pin, ACTIVE_LOW ? LOW  : HIGH); }
static inline void hardOff(uint8_t pin) { digitalWrite(pin, ACTIVE_LOW ? HIGH : LOW ); }

void IRAM_ATTR pwmIsr() {
  uint8_t slot = pwmSlot;
  uint8_t dr = pwmDutyR, dy = pwmDutyY, dg = pwmDutyG;
  (slot < dr) ? hardOn(PIN_RED)    : hardOff(PIN_RED);
  (slot < dy) ? hardOn(PIN_YELLOW) : hardOff(PIN_YELLOW);
  (slot < dg) ? hardOn(PIN_GREEN)  : hardOff(PIN_GREEN);
  pwmSlot = (slot + 1) % SW_PWM_LEVELS;
}

void writeRgb(uint8_t r, uint8_t y, uint8_t g) {
  // Quantize 0..255 to 0..SW_PWM_LEVELS. Only literal 0 means OFF; any
  // nonzero input keeps at least 1 level so dim states still glow.
  pwmDutyR = (r == 0) ? 0 : (uint8_t)((uint16_t)r * SW_PWM_LEVELS / 255 + 1);
  pwmDutyY = (y == 0) ? 0 : (uint8_t)((uint16_t)y * SW_PWM_LEVELS / 255 + 1);
  pwmDutyG = (g == 0) ? 0 : (uint8_t)((uint16_t)g * SW_PWM_LEVELS / 255 + 1);
  if (pwmDutyR > SW_PWM_LEVELS) pwmDutyR = SW_PWM_LEVELS;
  if (pwmDutyY > SW_PWM_LEVELS) pwmDutyY = SW_PWM_LEVELS;
  if (pwmDutyG > SW_PWM_LEVELS) pwmDutyG = SW_PWM_LEVELS;
}

void pwmTimerStart() {
  // Arduino-ESP32 3.x timer API: timerBegin(frequency_hz). 1 MHz means
  // alarm count == microseconds. Fire every 125 us → 500 Hz LED refresh.
  pwmTimer = timerBegin(1000000);
  timerAttachInterrupt(pwmTimer, &pwmIsr);
  timerAlarm(pwmTimer, 125, true, 0);  // 125us auto-reload, no count limit
}

void setEffectFromFrames(const Frame *frames, size_t n) {
  if (n > MAX_FRAMES) n = MAX_FRAMES;
  for (size_t i = 0; i < n; i++) effect[i] = frames[i];
  effectLen = n;
  frameIdx = 0;
  frameStartedMs = millis();
  if (n > 0) writeRgb(effect[0].r, effect[0].y, effect[0].g);
  else       writeRgb(0, 0, 0);
  Serial.printf("[EFF ] %u frame(s), f0=(r=%u y=%u g=%u ms=%u)\n",
                (unsigned)n,
                (unsigned)effect[0].r, (unsigned)effect[0].y, (unsigned)effect[0].g,
                (unsigned)effect[0].ms);
}

void setSolid(uint8_t r, uint8_t y, uint8_t g) {
  Frame f = { r, y, g, 0 };
  setEffectFromFrames(&f, 1);
}

void tickAnimation() {
  if (effectLen == 0) return;
  Frame &cur = effect[frameIdx];
  if (cur.ms == 0) return;
  uint32_t scaled = (uint32_t)((float)cur.ms / (effectTempo > 0.1f ? effectTempo : 1.0f));
  if (scaled < 20) scaled = 20;
  if (millis() - frameStartedMs < scaled) return;
  frameIdx = (frameIdx + 1) % effectLen;
  frameStartedMs = millis();
  Frame &nxt = effect[frameIdx];
  writeRgb(nxt.r, nxt.y, nxt.g);
}

float parseTempo(const char *src) {
  const char *p = strstr(src, "\"tempo\"");
  if (!p) return 1.0f;
  p = strchr(p, ':');
  if (!p) return 1.0f;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  float v = atof(p);
  if (v <= 0.0f) v = 1.0f;
  if (v > 6.0f)  v = 6.0f;
  return v;
}

const char *findKey(const char *src, const char *key) {
  size_t kl = strlen(key);
  for (const char *p = src; *p; p++) {
    if (*p != '"') continue;
    if (strncmp(p + 1, key, kl) == 0 && p[1 + kl] == '"') return p + 2 + kl;
  }
  return nullptr;
}

bool extractString(const char *after, char *out, size_t outSize) {
  const char *colon = strchr(after, ':');
  if (!colon) return false;
  const char *q1 = strchr(colon + 1, '"');
  if (!q1) return false;
  const char *q2 = strchr(q1 + 1, '"');
  if (!q2) return false;
  size_t n = (size_t)(q2 - q1 - 1);
  if (n >= outSize) n = outSize - 1;
  memcpy(out, q1 + 1, n);
  out[n] = '\0';
  return true;
}

size_t parseFrames(const char *src, Frame *outFrames, size_t maxFrames) {
  const char *p = strstr(src, "\"frames\"");
  if (!p) return 0;
  p = strchr(p, '[');
  if (!p) return 0;
  p++;
  size_t count = 0;
  while (*p && count < maxFrames) {
    while (*p == ' ' || *p == ',' || *p == '\t') p++;
    if (*p == ']' || *p == '\0') break;
    if (*p != '{') { p++; continue; }
    const char *end = strchr(p, '}');
    if (!end) break;
    int r = 0, y = 0, g = 0, ms = 0;
    const char *rp = strstr(p, "\"r\"");
    const char *yp = strstr(p, "\"y\"");
    const char *gp = strstr(p, "\"g\"");
    const char *mp = strstr(p, "\"ms\"");
    if (rp && rp < end) r = atoi(strchr(rp, ':') + 1);
    if (yp && yp < end) y = atoi(strchr(yp, ':') + 1);
    if (gp && gp < end) g = atoi(strchr(gp, ':') + 1);
    if (mp && mp < end) {
      const char *vp = strchr(mp, ':');
      if (vp) {
        vp++;
        while (*vp == ' ') vp++;
        ms = (strncmp(vp, "null", 4) == 0) ? 0 : atoi(vp);
      }
    }
    outFrames[count].r  = (uint8_t)(r  < 0 ? 0 : (r  > 255 ? 255 : r));
    outFrames[count].y  = (uint8_t)(y  < 0 ? 0 : (y  > 255 ? 255 : y));
    outFrames[count].g  = (uint8_t)(g  < 0 ? 0 : (g  > 255 ? 255 : g));
    outFrames[count].ms = (uint16_t)(ms < 0 ? 0 : (ms > 65535 ? 65535 : ms));
    count++;
    p = end + 1;
  }
  return count;
}

void handleLine(const char *line, WiFiClient &client) {
  lastDaemonMsgMs = millis();
  // truncated echo
  char preview[80];
  size_t plen = strlen(line);
  if (plen > sizeof(preview) - 1) plen = sizeof(preview) - 1;
  memcpy(preview, line, plen);
  preview[plen] = '\0';
  Serial.printf("[RX  ] %s\n", preview);

  char typeBuf[16] = {0};
  const char *tk = findKey(line, "type");
  if (tk && extractString(tk, typeBuf, sizeof(typeBuf))) {
    if (strcmp(typeBuf, "ping") == 0) {
      Serial.println("[PING] → PONG");
      client.print("{\"type\":\"pong\"}\n");
      return;
    }
    if (strcmp(typeBuf, "effect") == 0) {
      Frame parsed[MAX_FRAMES];
      size_t n = parseFrames(line, parsed, MAX_FRAMES);
      effectTempo = parseTempo(line);
      Serial.printf("[EFF ] tempo=%.2f x\n", effectTempo);
      if (n > 0) setEffectFromFrames(parsed, n);
      else       Serial.println("[EFF ] !! parse yielded 0 frames");
      return;
    }
    Serial.printf("[?   ] unknown type=%s\n", typeBuf);
  }
}

void handleClientByte(WiFiClient &client) {
  int b = client.read();
  if (b < 0) return;
  char c = (char)b;
  if (c == '\r') return;
  if (c == '\n') {
    if (lineLen > 0) {
      lineBuf[lineLen] = '\0';
      handleLine(lineBuf, client);
    }
    lineLen = 0;
    return;
  }
  if (lineLen < CLIENT_RX_BUFFER - 1) lineBuf[lineLen++] = c;
  else                                 lineLen = 0;
}

size_t currentWifiIdx = 0;

void startWifi() {
  if (WIFI_NETWORKS_COUNT == 0) return;
  const WifiCred &net = WIFI_NETWORKS[currentWifiIdx];
  Serial.printf("[WIFI] trying #%u %s\n", (unsigned)currentWifiIdx, net.ssid);
  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(net.ssid, net.password);
  lastWifiAttemptMs = millis();
  Frame yellow[] = { { 0, 80, 0, 300 }, { 0, 0, 0, 200 } };
  setEffectFromFrames(yellow, 2);
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiAttemptMs <= WIFI_PER_NETWORK_TIMEOUT_MS) return;
  currentWifiIdx = (currentWifiIdx + 1) % WIFI_NETWORKS_COUNT;
  startWifi();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32-C3 Signal Light DEBUG ===");
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_YELLOW, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  // No analogWriteResolution() / ledcSetup() — software PWM in a hardware
  // timer ISR so OFF is truly OFF and cadence is rock-steady even when
  // loop() is busy with Wi-Fi / TCP work.
  hardOff(PIN_RED);
  hardOff(PIN_YELLOW);
  hardOff(PIN_GREEN);
  writeRgb(0, 0, 0);
  pwmTimerStart();
  startWifi();
  server.begin();
  lastDaemonMsgMs = millis();
  Serial.printf("[TCP ] listening on port %u\n", TCP_PORT);
}

void loop() {
  maintainWifi();
  static wl_status_t prev = WL_IDLE_STATUS;
  wl_status_t now = WiFi.status();
  if (now != prev) {
    prev = now;
    Serial.printf("[WIFI] status=%d\n", (int)now);
    if (now == WL_CONNECTED) {
      Serial.print("[WIFI] connected, IP=");
      Serial.println(WiFi.localIP());
      setSolid(0, 0, 0);
      lastDaemonMsgMs = millis();
    }
  }

  if (!currentClient || !currentClient.connected()) {
    WiFiClient incoming = server.available();
    if (incoming) {
      if (currentClient) currentClient.stop();
      currentClient = incoming;
      currentClient.setNoDelay(true);
      lineLen = 0;
      lastDaemonMsgMs = millis();
      Serial.print("[CONN] daemon from ");
      Serial.println(currentClient.remoteIP());
      currentClient.printf("{\"type\":\"hello\",\"ip\":\"%s\"}\n", WiFi.localIP().toString().c_str());
    }
  }

  if (currentClient && currentClient.connected()) {
    while (currentClient.available() > 0) handleClientByte(currentClient);
  }

  if (WiFi.status() == WL_CONNECTED && millis() - lastDaemonMsgMs > WATCHDOG_MS) {
    Serial.println("[WDG ] no daemon msgs, rebooting");
    delay(50);
    esp_restart();
  }

  tickAnimation();
  // PWM is driven by the hardware timer ISR — loop() doesn't touch the LEDs
  // directly, it just updates the pwmDutyR/Y/G targets via writeRgb().
  delay(1);
}
