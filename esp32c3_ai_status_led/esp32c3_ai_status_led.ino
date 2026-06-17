/*
  ESP32-C3 AI Status LED Indicator

  Robust runnable version based on the Xiaohongshu video:
  "ESP32实现Claude code状态指示灯" by QStudio电子工作室.

  Protocol:
    TCP port 8080
    Send one line per command:
      thinking
      ai
      busy
      success
      error
      alarm
      off

    These forms are also accepted:
      STATE thinking
      {"state":"thinking"}
*/

#include <Arduino.h>
#include <WiFi.h>
#include <ctype.h>
#include <string.h>

// ===== Wi-Fi settings =====
// Change these before flashing.
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ===== LED pins =====
// These match the pins shown in the video. Change if your board is wired differently.
const uint8_t PIN_RED = 5;
const uint8_t PIN_YELLOW = 6;
const uint8_t PIN_GREEN = 7;

// Set true if your LED module turns on when the GPIO is LOW.
const bool ACTIVE_LOW = false;

const uint16_t TCP_PORT = 8080;
const uint32_t WIFI_RETRY_MS = 8000;
const uint16_t CLIENT_IDLE_TIMEOUT_MS = 1200;
const size_t COMMAND_BUFFER_SIZE = 96;

WiFiServer server(TCP_PORT);

enum AiState {
  STATE_OFF,
  STATE_THINKING,
  STATE_AI,
  STATE_BUSY,
  STATE_SUCCESS,
  STATE_ERROR,
  STATE_ALARM
};

AiState currentState = STATE_OFF;
const char *currentStateName = "off";
uint32_t lastFrameMs = 0;
uint32_t lastWifiAttemptMs = 0;
uint8_t frame = 0;

void writeLed(uint8_t pin, uint8_t brightness) {
  if (ACTIVE_LOW) {
    brightness = 255 - brightness;
  }
  analogWrite(pin, brightness);
}

void setRgb(uint8_t red, uint8_t yellow, uint8_t green) {
  writeLed(PIN_RED, red);
  writeLed(PIN_YELLOW, yellow);
  writeLed(PIN_GREEN, green);
}

void setState(AiState nextState, const char *name) {
  if (currentState == nextState) {
    return;
  }

  currentState = nextState;
  currentStateName = name;
  lastFrameMs = 0;
  frame = 0;

  Serial.print("State changed: ");
  Serial.println(currentStateName);
}

void trimInPlace(char *text) {
  if (text == nullptr) {
    return;
  }

  char *start = text;
  while (*start && isspace(static_cast<unsigned char>(*start))) {
    start++;
  }

  char *end = start + strlen(start);
  while (end > start && isspace(static_cast<unsigned char>(*(end - 1)))) {
    end--;
  }
  *end = '\0';

  if (start != text) {
    memmove(text, start, strlen(start) + 1);
  }
}

void lowercaseInPlace(char *text) {
  for (size_t i = 0; text[i] != '\0'; i++) {
    text[i] = static_cast<char>(tolower(static_cast<unsigned char>(text[i])));
  }
}

bool startsWith(const char *text, const char *prefix) {
  return strncmp(text, prefix, strlen(prefix)) == 0;
}

void normalizeCommand(char *command, size_t commandSize) {
  if (command == nullptr || commandSize == 0) {
    return;
  }

  command[commandSize - 1] = '\0';
  trimInPlace(command);
  lowercaseInPlace(command);

  if (startsWith(command, "state ")) {
    memmove(command, command + 6, strlen(command + 6) + 1);
    trimInPlace(command);
  }

  char *jsonKey = strstr(command, "\"state\"");
  if (jsonKey != nullptr) {
    char *colon = strchr(jsonKey, ':');
    if (colon != nullptr) {
      char *firstQuote = strchr(colon + 1, '"');
      if (firstQuote != nullptr) {
        char *secondQuote = strchr(firstQuote + 1, '"');
        if (secondQuote != nullptr && secondQuote > firstQuote) {
          size_t len = static_cast<size_t>(secondQuote - firstQuote - 1);
          if (len >= commandSize) {
            len = commandSize - 1;
          }
          memmove(command, firstQuote + 1, len);
          command[len] = '\0';
          trimInPlace(command);
        }
      }
    }
  }
}

bool applyCommand(char *command) {
  normalizeCommand(command, COMMAND_BUFFER_SIZE);

  if (strcmp(command, "off") == 0) {
    setState(STATE_OFF, "off");
  } else if (strcmp(command, "thinking") == 0) {
    setState(STATE_THINKING, "thinking");
  } else if (strcmp(command, "ai") == 0) {
    setState(STATE_AI, "ai");
  } else if (strcmp(command, "busy") == 0) {
    setState(STATE_BUSY, "busy");
  } else if (strcmp(command, "success") == 0) {
    setState(STATE_SUCCESS, "success");
  } else if (strcmp(command, "error") == 0) {
    setState(STATE_ERROR, "error");
  } else if (strcmp(command, "alarm") == 0) {
    setState(STATE_ALARM, "alarm");
  } else {
    Serial.print("Unknown command: ");
    Serial.println(command);
    return false;
  }

  return true;
}

uint8_t triangleWave(uint8_t value, uint8_t period, uint8_t low, uint8_t high) {
  uint8_t phase = value % period;
  uint8_t half = period / 2;
  uint8_t span = high - low;

  if (phase >= half) {
    phase = period - 1 - phase;
  }

  return low + static_cast<uint8_t>((static_cast<uint16_t>(span) * phase) / (half - 1));
}

void tickLed() {
  uint32_t now = millis();
  if (now - lastFrameMs < 80) {
    return;
  }

  lastFrameMs = now;
  frame++;

  switch (currentState) {
    case STATE_OFF:
      setRgb(0, 0, 0);
      break;

    case STATE_THINKING:
      setRgb(0, 0, triangleWave(frame, 24, 20, 255));
      break;

    case STATE_AI:
      setRgb(0, triangleWave(frame, 20, 30, 220), 120);
      break;

    case STATE_BUSY:
      setRgb(0, 180, 0);
      break;

    case STATE_SUCCESS:
      setRgb(0, 0, (frame % 8 < 4) ? 255 : 45);
      break;

    case STATE_ERROR:
      setRgb((frame % 6 < 3) ? 255 : 0, 0, 0);
      break;

    case STATE_ALARM:
      if (frame % 10 < 5) {
        setRgb(255, 0, 0);
      } else {
        setRgb(0, 255, 0);
      }
      break;
  }
}

void readAndApplyClient(WiFiClient &client) {
  char command[COMMAND_BUFFER_SIZE];
  size_t length = 0;
  uint32_t lastByteMs = millis();

  client.setNoDelay(true);

  while (client.connected() && millis() - lastByteMs < CLIENT_IDLE_TIMEOUT_MS) {
    while (client.available() > 0) {
      char c = static_cast<char>(client.read());
      lastByteMs = millis();

      if (c == '\r') {
        continue;
      }

      if (c == '\n') {
        command[length] = '\0';
        trimInPlace(command);
        if (length > 0) {
          bool ok = applyCommand(command);
          client.println(ok ? "OK" : "ERR");
        }
        length = 0;
        continue;
      }

      if (length < COMMAND_BUFFER_SIZE - 1) {
        command[length++] = c;
      } else {
        command[COMMAND_BUFFER_SIZE - 1] = '\0';
        client.println("ERR command too long");
        length = 0;
      }
    }

    tickLed();
    delay(1);
  }

  if (length > 0) {
    command[length] = '\0';
    trimInPlace(command);
    bool ok = applyCommand(command);
    client.println(ok ? "OK" : "ERR");
  }

  client.stop();
}

void startWifiAttempt() {
  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(WIFI_SSID);
  WiFi.disconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttemptMs = millis();
  setState(STATE_ALARM, "alarm");
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  uint32_t now = millis();
  if (lastWifiAttemptMs == 0 || now - lastWifiAttemptMs > WIFI_RETRY_MS) {
    startWifiAttempt();
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_YELLOW, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  analogWriteResolution(PIN_RED, 8);
  analogWriteResolution(PIN_YELLOW, 8);
  analogWriteResolution(PIN_GREEN, 8);
  setRgb(0, 0, 0);

  startWifiAttempt();
  server.begin();

  Serial.print("TCP server listening on port ");
  Serial.println(TCP_PORT);
}

void loop() {
  maintainWifi();

  static wl_status_t lastWifiStatus = WL_IDLE_STATUS;
  wl_status_t wifiStatus = WiFi.status();
  if (wifiStatus != lastWifiStatus) {
    lastWifiStatus = wifiStatus;
    if (wifiStatus == WL_CONNECTED) {
      Serial.print("WiFi connected, IP: ");
      Serial.println(WiFi.localIP());
      setState(STATE_OFF, "off");
    }
  }

  WiFiClient client = server.available();
  if (client) {
    readAndApplyClient(client);
  }

  tickLed();
  delay(1);
}
