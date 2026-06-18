/*
  ESP32-C3 Signal Light (frame-animation protocol).

  This firmware is a "dumb display": it does not know about Claude or about
  state names. The local daemon owns all logic and pushes JSON frame sequences.

  Protocol (newline-delimited JSON over TCP port 8080):
    daemon → board:
      {"type":"effect","effect":{"id":"...","frames":[{"r":0,"y":0,"g":255,"ms":120}, ...]}}
      {"type":"ping","ts":1234567890}
    board → daemon (optional):
      {"type":"pong","ts":1234567890}
      {"type":"hello","fw":"...", "ip":"..."}

  Frames:
    - r, y, g: 0-255 brightness for the red / yellow / green channel.
    - ms: frame duration. A frame with ms == null (or 0) holds forever.
    - The board loops frames until a new "effect" arrives.

  Watchdog:
    - If no daemon message arrives for WATCHDOG_MS, the board reboots
      (covers Wi-Fi soft-failure where TCP appears alive but is wedged).
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>

// ===== Change these before flashing =====
// Add as many Wi-Fi networks as you want. The board tries them in order on
// every reconnect cycle and stays on the first one that works. Useful if you
// move the board between home / office, or want a phone hotspot as fallback.
struct WifiCred { const char *ssid; const char *password; };
const WifiCred WIFI_NETWORKS[] = {
  { "YOUR_WIFI_SSID",      "YOUR_WIFI_PASSWORD"      },
  // { "Secondary_SSID",   "secondary_password"      },
  // { "Phone_Hotspot",    "hotspot_password"        },
};
const size_t WIFI_NETWORKS_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);
const uint32_t WIFI_PER_NETWORK_TIMEOUT_MS = 8000;  // give each SSID this long before trying the next

// ===== Pins (match the supermini wiring) =====
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

// ===== Network =====
const uint16_t TCP_PORT          = 8080;
const uint32_t WATCHDOG_MS       = 30000;   // reboot if no msg for this long
const uint16_t CLIENT_RX_BUFFER  = 2048;    // one frame batch fits comfortably

// ===== Effect storage =====
struct Frame { uint8_t r, y, g; uint16_t ms; };  // ms == 0 ⇒ hold forever
const size_t MAX_FRAMES = 32;

WiFiServer server(TCP_PORT);
WiFiClient currentClient;

Frame effect[MAX_FRAMES];
size_t effectLen = 0;
size_t frameIdx = 0;
uint32_t frameStartedMs = 0;
float effectTempo = 1.0f;  // animation speed multiplier; ms /= tempo
uint32_t lastDaemonMsgMs = 0;
uint32_t lastWifiAttemptMs = 0;
char lineBuf[CLIENT_RX_BUFFER];
size_t lineLen = 0;

// ===== LEDC hardware PWM =====
// True hardware PWM via low-level ledcAttach/ledcWrite. Runs independently
// of CPU / Wi-Fi / flash cache, so it never flickers regardless of what
// the rest of the firmware is doing.
//
// For guaranteed full-OFF on common-anode wiring, we ledcDetach() the pin
// and drive it HIGH (LED off). Re-attach when duty becomes nonzero again.

const uint32_t LEDC_FREQ_HZ = 1000;
const uint8_t  LEDC_RES_BITS = 8;
const uint8_t  LEDC_MAX = 255;

bool ledcAttachedR = false, ledcAttachedY = false, ledcAttachedG = false;

static inline void ledOffHard(uint8_t pin) {
  digitalWrite(pin, ACTIVE_LOW ? HIGH : LOW);
}

static inline void setChannel(uint8_t pin, uint8_t duty, bool &attached) {
  if (duty == 0) {
    if (attached) {
      ledcDetach(pin);
      pinMode(pin, OUTPUT);
      attached = false;
    }
    ledOffHard(pin);
  } else {
    if (!attached) {
      ledcAttach(pin, LEDC_FREQ_HZ, LEDC_RES_BITS);
      attached = true;
    }
    uint32_t hwDuty = ACTIVE_LOW ? (LEDC_MAX - duty) : duty;
    ledcWrite(pin, hwDuty);
  }
}

void writeRgb(uint8_t r, uint8_t y, uint8_t g) {
  setChannel(PIN_RED,    r, ledcAttachedR);
  setChannel(PIN_YELLOW, y, ledcAttachedY);
  setChannel(PIN_GREEN,  g, ledcAttachedG);
}

static inline void hardOn (uint8_t pin) { digitalWrite(pin, ACTIVE_LOW ? LOW  : HIGH); }
static inline void hardOff(uint8_t pin) { digitalWrite(pin, ACTIVE_LOW ? HIGH : LOW ); }

void setEffectFromFrames(const Frame *frames, size_t n) {
  if (n > MAX_FRAMES) n = MAX_FRAMES;
  for (size_t i = 0; i < n; i++) effect[i] = frames[i];
  effectLen = n;
  frameIdx = 0;
  frameStartedMs = millis();
  if (n > 0) writeRgb(effect[0].r, effect[0].y, effect[0].g);
  else       writeRgb(0, 0, 0);
}

void setSolid(uint8_t r, uint8_t y, uint8_t g) {
  Frame f = { r, y, g, 0 };
  setEffectFromFrames(&f, 1);
}

void tickAnimation() {
  if (effectLen == 0) return;
  Frame &cur = effect[frameIdx];
  if (cur.ms == 0) return;  // hold forever
  uint32_t scaled = (uint32_t)((float)cur.ms / (effectTempo > 0.1f ? effectTempo : 1.0f));
  if (scaled < 20) scaled = 20;  // clamp to a sane minimum
  if (millis() - frameStartedMs < scaled) return;
  frameIdx = (frameIdx + 1) % effectLen;
  frameStartedMs = millis();
  Frame &nxt = effect[frameIdx];
  writeRgb(nxt.r, nxt.y, nxt.g);
}

// Parse a numeric "tempo" field (float) from the line. Defaults to 1.0.
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

// ---- minimal JSON line parser (no full JSON; we only extract a few fields) ----
// Returns pointer to char inside src after the matched key, or nullptr.
const char *findKey(const char *src, const char *key) {
  // Look for "key" (with quotes). Returns position just after the closing quote.
  size_t kl = strlen(key);
  for (const char *p = src; *p; p++) {
    if (*p != '"') continue;
    if (strncmp(p + 1, key, kl) == 0 && p[1 + kl] == '"') {
      return p + 2 + kl;
    }
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

// Parse the frames array of an effect line. Returns count parsed.
// We scan for objects like {"r":N,"y":N,"g":N,"ms":N|null}.
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
    if (rp && rp < end) r  = atoi(strchr(rp, ':') + 1);
    if (yp && yp < end) y  = atoi(strchr(yp, ':') + 1);
    if (gp && gp < end) g  = atoi(strchr(gp, ':') + 1);
    if (mp && mp < end) {
      const char *vp = strchr(mp, ':');
      if (vp) {
        vp++;
        while (*vp == ' ') vp++;
        if (strncmp(vp, "null", 4) == 0) ms = 0;
        else                              ms = atoi(vp);
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

  char typeBuf[16] = {0};
  const char *tk = findKey(line, "type");
  if (tk && extractString(tk, typeBuf, sizeof(typeBuf))) {
    if (strcmp(typeBuf, "ping") == 0) {
      client.print("{\"type\":\"pong\"}\n");
      return;
    }
    if (strcmp(typeBuf, "effect") == 0) {
      Frame parsed[MAX_FRAMES];
      size_t n = parseFrames(line, parsed, MAX_FRAMES);
      effectTempo = parseTempo(line);
      if (n > 0) setEffectFromFrames(parsed, n);
      return;
    }
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
  if (lineLen < CLIENT_RX_BUFFER - 1) {
    lineBuf[lineLen++] = c;
  } else {
    lineLen = 0;  // overflow; drop and resync
  }
}

size_t currentWifiIdx = 0;

void startWifi() {
  if (WIFI_NETWORKS_COUNT == 0) return;
  const WifiCred &net = WIFI_NETWORKS[currentWifiIdx];
  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(net.ssid, net.password);
  lastWifiAttemptMs = millis();
  // Visual hint while Wi-Fi is down: soft pulse yellow.
  Frame yellow[] = {
    { 0, 80,  0, 300 },
    { 0, 0,   0, 200 },
  };
  setEffectFromFrames(yellow, 2);
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiAttemptMs <= WIFI_PER_NETWORK_TIMEOUT_MS) return;
  // Current network didn't connect in time → rotate to the next one.
  currentWifiIdx = (currentWifiIdx + 1) % WIFI_NETWORKS_COUNT;
  startWifi();
}

void setup() {
  Serial.begin(115200);
  delay(150);

  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_YELLOW, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  // Start fully off. writeRgb() will lazily attach LEDC channels only
  // when a nonzero duty is requested.
  hardOff(PIN_RED);
  hardOff(PIN_YELLOW);
  hardOff(PIN_GREEN);
  writeRgb(0, 0, 0);

  startWifi();
  server.begin();
  lastDaemonMsgMs = millis();
  Serial.printf("signal-light fw ready, TCP %u\n", TCP_PORT);
}

void loop() {
  maintainWifi();

  static wl_status_t prev = WL_IDLE_STATUS;
  wl_status_t now = WiFi.status();
  if (now != prev) {
    prev = now;
    if (now == WL_CONNECTED) {
      Serial.print("Wi-Fi connected, IP=");
      Serial.println(WiFi.localIP());
      // Default to off until daemon sends an effect.
      setSolid(0, 0, 0);
      lastDaemonMsgMs = millis();
    }
  }

  // Accept a fresh client. We keep one active client at a time.
  if (!currentClient || !currentClient.connected()) {
    WiFiClient incoming = server.available();
    if (incoming) {
      if (currentClient) currentClient.stop();
      currentClient = incoming;
      currentClient.setNoDelay(true);
      lineLen = 0;
      lastDaemonMsgMs = millis();
      // Greet the daemon (informational only).
      currentClient.printf("{\"type\":\"hello\",\"ip\":\"%s\"}\n", WiFi.localIP().toString().c_str());
    }
  }

  if (currentClient && currentClient.connected()) {
    while (currentClient.available() > 0) handleClientByte(currentClient);
  }

  // Watchdog: if daemon stopped sending anything (incl. pings) → reboot.
  // Only enforce after Wi-Fi has been up at least once.
  if (WiFi.status() == WL_CONNECTED && millis() - lastDaemonMsgMs > WATCHDOG_MS) {
    Serial.println("watchdog: no daemon msgs, rebooting");
    delay(50);
    esp_restart();
  }

  tickAnimation();
  // PWM is in a hardware-timer ISR — loop() only updates duty targets.
  delay(1);
}
