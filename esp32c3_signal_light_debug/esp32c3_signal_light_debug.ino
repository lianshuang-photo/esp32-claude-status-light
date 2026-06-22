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
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// Persistent storage for "which Wi-Fi network worked last time", so reboots
// can connect in ~1s instead of going through every entry from the top.
// Also stores the most recent BLE-provisioned SSID/password.
Preferences prefs;
const char *NVS_NAMESPACE   = "signal";
const char *NVS_KEY_WIFI    = "last_wifi";
const char *NVS_KEY_BLE_SSID = "ble_ssid";
const char *NVS_KEY_BLE_PSK  = "ble_psk";

// ===== BLE GATT (Wi-Fi provisioning over Bluetooth) =====
// Anyone (Mac / phone) can connect, read the Wi-Fi scan list, write
// SSID+password, and read back the resulting IP. No pairing — this is a
// debug accessory, not a vault.
#define BLE_SERVICE_UUID    "a2c9b001-1234-4abc-8def-5f6c7d8e9012"
#define BLE_CHAR_SCAN_UUID  "a2c9b002-1234-4abc-8def-5f6c7d8e9012"  // read: JSON list of APs
#define BLE_CHAR_CMD_UUID   "a2c9b003-1234-4abc-8def-5f6c7d8e9012"  // write: {"ssid":"x","psk":"y"}
#define BLE_CHAR_STAT_UUID  "a2c9b004-1234-4abc-8def-5f6c7d8e9012"  // read/notify: {"state":"...","ip":"..."}

BLEServer *bleServer = nullptr;
BLECharacteristic *bleCharScan = nullptr;
BLECharacteristic *bleCharCmd  = nullptr;
BLECharacteristic *bleCharStat = nullptr;
bool bleClientConnected = false;
bool bleProvisioningPending = false;       // a {ssid, psk} write came in, main loop should act
String bleProvSsid;
String bleProvPsk;
uint32_t bleStatusLastUpdateMs = 0;

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
const uint32_t WATCHDOG_MS      = 90000;
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

// ===== LEDC hardware PWM =====
// We use the ESP32 LEDC peripheral via low-level ledcAttach/ledcWrite,
// NOT analogWrite. LEDC is a true hardware PWM generator; once configured
// it runs completely independently of CPU / Wi-Fi / flash cache, so it
// never flickers or stalls regardless of what the rest of the firmware
// is doing.
//
// "OFF" guarantee: simply writing duty=0 to LEDC sometimes leaves a tiny
// residual signal on ESP32-C3 (likely a 1-cycle glitch every PWM period),
// which manifests as a faint glow on a sensitive LED. So when a channel
// needs to be fully OFF, we ledcDetach() it and drive the GPIO HIGH (with
// common-anode wiring HIGH = LED off). When the duty comes back up we
// re-attach.
//
// 8-bit resolution × 1 kHz frequency is plenty for status-light use.

const uint32_t LEDC_FREQ_HZ = 1000;
const uint8_t  LEDC_RES_BITS = 8;
const uint8_t  LEDC_MAX = 255;

// Track whether each channel is currently attached (true) or detached + HIGH (false).
bool ledcAttachedR = false, ledcAttachedY = false, ledcAttachedG = false;

static inline void ledOffHard(uint8_t pin) {
  // Common-anode: HIGH = off.
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
    // On common-anode wiring, duty 255 should be fully on → invert.
    uint32_t hwDuty = ACTIVE_LOW ? (LEDC_MAX - duty) : duty;
    ledcWrite(pin, hwDuty);
  }
}

void writeRgb(uint8_t r, uint8_t y, uint8_t g) {
  setChannel(PIN_RED,    r, ledcAttachedR);
  setChannel(PIN_YELLOW, y, ledcAttachedY);
  setChannel(PIN_GREEN,  g, ledcAttachedG);
}

// Legacy helpers (used during setup before LEDC is initialised):
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
  Serial.printf("[EFF ] %u frame(s), f0=(r=%u y=%u g=%u ms=%u)\n",
                (unsigned)n,
                (unsigned)effect[0].r, (unsigned)effect[0].y, (unsigned)effect[0].g,
                (unsigned)effect[0].ms);
}

void setSolid(uint8_t r, uint8_t y, uint8_t g) {
  Frame f = { r, y, g, 0 };
  setEffectFromFrames(&f, 1);
}

// Per-frame serial logging eats real CPU on a single-core ESP32-C3 when
// frames run at ~10 Hz. Set to true only when debugging frame timing.
const bool LOG_FRAMES = false;

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
  if (LOG_FRAMES) {
    Serial.printf("[FRM ] →%u r=%u y=%u g=%u (dwell=%ums)\n",
                  (unsigned)frameIdx, (unsigned)nxt.r, (unsigned)nxt.y, (unsigned)nxt.g, (unsigned)nxt.ms);
  }
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
  char typeBuf[16] = {0};
  const char *tk = findKey(line, "type");
  bool isPing = tk && extractString(tk, typeBuf, sizeof(typeBuf)) && strcmp(typeBuf, "ping") == 0;

  // Only log non-ping traffic — pings come every 5s and dominate the serial log.
  if (!isPing) {
    char preview[80];
    size_t plen = strlen(line);
    if (plen > sizeof(preview) - 1) plen = sizeof(preview) - 1;
    memcpy(preview, line, plen);
    preview[plen] = '\0';
    Serial.printf("[RX  ] %s\n", preview);
  }

  if (tk) {
    if (isPing) {
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

// If the board has been BLE-provisioned, those credentials take priority
// over WIFI_NETWORKS[]. -1 means "no BLE creds, use compiled-in list".
String bleSsidLive;
String blePskLive;
bool useBleCreds() { return bleSsidLive.length() > 0; }

void startWifi() {
  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  if (useBleCreds()) {
    Serial.printf("[WIFI] trying BLE-provisioned %s\n", bleSsidLive.c_str());
    WiFi.begin(bleSsidLive.c_str(), blePskLive.c_str());
  } else if (WIFI_NETWORKS_COUNT > 0) {
    const WifiCred &net = WIFI_NETWORKS[currentWifiIdx];
    Serial.printf("[WIFI] trying #%u %s\n", (unsigned)currentWifiIdx, net.ssid);
    WiFi.begin(net.ssid, net.password);
  } else {
    Serial.println("[WIFI] no credentials, waiting for BLE provisioning");
  }
  lastWifiAttemptMs = millis();
  Frame yellow[] = { { 0, 80, 0, 300 }, { 0, 0, 0, 200 } };
  setEffectFromFrames(yellow, 2);
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiAttemptMs <= WIFI_PER_NETWORK_TIMEOUT_MS) return;
  // Don't rotate if BLE creds are in use — keep retrying that one network.
  if (!useBleCreds() && WIFI_NETWORKS_COUNT > 0) {
    currentWifiIdx = (currentWifiIdx + 1) % WIFI_NETWORKS_COUNT;
  }
  startWifi();
}

// ===== BLE provisioning =====

void bleUpdateStatus(const char *state, const String &ip) {
  if (!bleCharStat) return;
  String json = "{\"state\":\"";
  json += state;
  json += "\",\"ip\":\"";
  json += ip;
  json += "\"}";
  bleCharStat->setValue(json.c_str());
  if (bleClientConnected) {
    bleCharStat->notify();
  }
  bleStatusLastUpdateMs = millis();
}

// Cache of the most recent Wi-Fi scan results, refreshed by the main loop
// (NOT by the BLE read callback, which runs in a context that can't safely
// call long-running radio operations).
String wifiScanCacheJson = "[]";
uint32_t wifiScanCacheAtMs = 0;
volatile bool wifiScanRequested = false;
const uint32_t WIFI_SCAN_CACHE_TTL_MS = 30000;  // 30s
const uint32_t WIFI_SCAN_MIN_INTERVAL_MS = 5000;  // never scan more than once per 5s

void doWifiScanIntoCache() {
  // Briefly disconnect Wi-Fi STA so the scan returns real results.
  bool wasConnected = (WiFi.status() == WL_CONNECTED);
  if (wasConnected || WiFi.status() == WL_DISCONNECTED) {
    // disconnect=true forces the connect attempt to abort so scan can run
    WiFi.disconnect(false, false);
    delay(50);
  }
  Serial.println("[BLE ] running Wi-Fi scan...");
  int n = WiFi.scanNetworks(false /*async*/, true /*show hidden*/, false, 250);
  String json = "[";
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"";
    String s = WiFi.SSID(i);
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    json += s;
    json += "\",\"rssi\":";
    json += String(WiFi.RSSI(i));
    json += ",\"open\":";
    json += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false");
    json += "}";
    if (json.length() > 480) break;
  }
  json += "]";
  WiFi.scanDelete();
  wifiScanCacheJson = json;
  wifiScanCacheAtMs = millis();
  Serial.printf("[BLE ] scan: %d networks, %u bytes JSON\n", n, (unsigned)json.length());

  // Restart Wi-Fi connection attempt so we keep trying in the background.
  startWifi();
}

// BLE callback: just returns the cached JSON. Real scanning happens in loop().
class ScanCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic *c) override {
    Serial.printf("[BLE ] scan read (cache age %us)\n",
                  (unsigned)((millis() - wifiScanCacheAtMs) / 1000));
    if (wifiScanCacheAtMs == 0 ||
        millis() - wifiScanCacheAtMs > WIFI_SCAN_CACHE_TTL_MS) {
      wifiScanRequested = true;  // ask main loop for a fresh scan
    }
    c->setValue(wifiScanCacheJson.c_str());
  }
};

// Receive {"ssid":"...","psk":"..."} from the client.
class CmdCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String payload = c->getValue().c_str();
    Serial.printf("[BLE ] cmd recv: %s\n", payload.c_str());
    // tiny JSON extract — no library needed
    int s1 = payload.indexOf("\"ssid\"");
    int p1 = payload.indexOf("\"psk\"");
    if (s1 < 0 || p1 < 0) {
      bleUpdateStatus("error_payload", "");
      return;
    }
    int sq1 = payload.indexOf('"', payload.indexOf(':', s1));
    int sq2 = payload.indexOf('"', sq1 + 1);
    int pq1 = payload.indexOf('"', payload.indexOf(':', p1));
    int pq2 = payload.indexOf('"', pq1 + 1);
    if (sq1 < 0 || sq2 < 0 || pq1 < 0 || pq2 < 0) {
      bleUpdateStatus("error_payload", "");
      return;
    }
    bleProvSsid = payload.substring(sq1 + 1, sq2);
    bleProvPsk  = payload.substring(pq1 + 1, pq2);
    bleProvisioningPending = true;
    bleUpdateStatus("connecting", "");
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    bleClientConnected = true;
    Serial.println("[BLE ] client connected");
  }
  void onDisconnect(BLEServer *) override {
    bleClientConnected = false;
    Serial.println("[BLE ] client disconnected");
    BLEDevice::startAdvertising();  // resume advertising after disconnect
  }
};

void bleStart() {
  // Device name = "SignalLight-XXXX" where XXXX = last 4 hex of MAC
  uint64_t mac = ESP.getEfuseMac();
  char name[24];
  snprintf(name, sizeof(name), "SignalLight-%04X", (uint16_t)(mac & 0xFFFF));
  Serial.printf("[BLE ] device name: %s\n", name);

  BLEDevice::init(name);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  BLEService *svc = bleServer->createService(BLE_SERVICE_UUID);

  bleCharScan = svc->createCharacteristic(
    BLE_CHAR_SCAN_UUID,
    BLECharacteristic::PROPERTY_READ);
  bleCharScan->setCallbacks(new ScanCallbacks());

  bleCharCmd = svc->createCharacteristic(
    BLE_CHAR_CMD_UUID,
    BLECharacteristic::PROPERTY_WRITE);
  bleCharCmd->setCallbacks(new CmdCallbacks());

  bleCharStat = svc->createCharacteristic(
    BLE_CHAR_STAT_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  bleCharStat->addDescriptor(new BLE2902());
  bleUpdateStatus(useBleCreds() ? "wifi_known" : "waiting", "");

  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[BLE ] advertising");
}

void bleApplyProvisioning() {
  if (!bleProvisioningPending) return;
  bleProvisioningPending = false;
  Serial.printf("[BLE ] applying ssid=%s\n", bleProvSsid.c_str());
  bleSsidLive = bleProvSsid;
  blePskLive  = bleProvPsk;
  prefs.putString(NVS_KEY_BLE_SSID, bleSsidLive);
  prefs.putString(NVS_KEY_BLE_PSK,  blePskLive);
  // Force a fresh connect attempt with the new credentials.
  WiFi.disconnect(true);
  delay(100);
  startWifi();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32-C3 Signal Light DEBUG ===");
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_YELLOW, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  // Start in fully-off state. writeRgb() lazily attaches LEDC channels
  // only when a nonzero duty is set.
  hardOff(PIN_RED);
  hardOff(PIN_YELLOW);
  hardOff(PIN_GREEN);
  writeRgb(0, 0, 0);

  prefs.begin(NVS_NAMESPACE, false);

  // BLE-provisioned credentials win over compiled-in WIFI_NETWORKS[].
  bleSsidLive = prefs.getString(NVS_KEY_BLE_SSID, "");
  blePskLive  = prefs.getString(NVS_KEY_BLE_PSK,  "");
  if (useBleCreds()) {
    Serial.printf("[NVS ] using BLE-provisioned SSID=%s\n", bleSsidLive.c_str());
  } else {
    uint8_t savedIdx = prefs.getUChar(NVS_KEY_WIFI, 0);
    if (savedIdx < WIFI_NETWORKS_COUNT) {
      currentWifiIdx = savedIdx;
      Serial.printf("[WIFI] resuming with last-known network #%u\n", (unsigned)savedIdx);
    }
  }

  startWifi();
  server.begin();
  lastDaemonMsgMs = millis();
  Serial.printf("[TCP ] listening on port %u\n", TCP_PORT);

  // Only start BLE if we have NO Wi-Fi credentials at all. Otherwise BLE
  // would steal main-loop CPU and make LED animations stutter.
  // To re-enter provisioning mode after the fact: erase NVS or hold a
  // button (TODO) for 3 s during boot.
  bool haveAnyCreds = useBleCreds() || WIFI_NETWORKS_COUNT > 0;
  if (!haveAnyCreds) {
    Serial.println("[BLE ] no Wi-Fi creds, starting BLE provisioning");
    bleStart();
    wifiScanRequested = true;
  } else {
    Serial.println("[BLE ] Wi-Fi creds present, skipping BLE (run-time provisioning disabled)");
  }
}

void loop() {
  maintainWifi();
  static wl_status_t prev = WL_IDLE_STATUS;
  wl_status_t now = WiFi.status();
  if (now != prev) {
    prev = now;
    Serial.printf("[WIFI] status=%d\n", (int)now);
    if (now == WL_CONNECTED) {
      String ip = WiFi.localIP().toString();
      Serial.print("[WIFI] connected, IP=");
      Serial.println(ip);
      setSolid(0, 0, 0);
      lastDaemonMsgMs = millis();
      if (!useBleCreds()) {
        uint8_t saved = prefs.getUChar(NVS_KEY_WIFI, 255);
        if (saved != currentWifiIdx) {
          prefs.putUChar(NVS_KEY_WIFI, (uint8_t)currentWifiIdx);
          Serial.printf("[WIFI] saved last-known network #%u to NVS\n", (unsigned)currentWifiIdx);
        }
      }
      bleUpdateStatus("connected", ip);
    } else if (now == WL_CONNECT_FAILED || now == WL_NO_SSID_AVAIL) {
      bleUpdateStatus("failed", "");
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

  // Handle any pending BLE-provisioned Wi-Fi credentials.
  bleApplyProvisioning();

  // If BLE client asked for a fresh Wi-Fi scan, do it from loop()
  // (NOT from the BLE callback — scanNetworks is heavy and must run here).
  if (wifiScanRequested &&
      (wifiScanCacheAtMs == 0 || millis() - wifiScanCacheAtMs > WIFI_SCAN_MIN_INTERVAL_MS)) {
    wifiScanRequested = false;
    doWifiScanIntoCache();
  }

  tickAnimation();
  // PWM is driven by the hardware timer ISR — loop() doesn't touch the LEDs
  // directly, it just updates the pwmDutyR/Y/G targets via writeRgb().
  delay(1);
}
