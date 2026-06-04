#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <time.h>
#include "esp_sleep.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ENABLE_KEYBOARD 1

#include <USB.h>
#include <USBHIDMouse.h>
#if ENABLE_KEYBOARD
#include <USBHIDKeyboard.h>
#endif

constexpr uint8_t PIXEL_COUNT = 1;
constexpr uint8_t NEOPIXEL_PIN = 21;
constexpr uint16_t WIFI_RETRY_INTERVAL_MS = 5000;
constexpr uint16_t ALERT_BLINK_INTERVAL_MS = 150;
constexpr uint8_t BUTTON_PIN = 8;
constexpr uint8_t PIXEL_BRIGHTNESS = 255;
constexpr uint16_t TCP_CONSOLE_PORT = 9000;
constexpr uint16_t LOOP_DELAY_CONNECTED_MS = 2;
constexpr uint16_t LOOP_DELAY_DISCONNECTED_MS = 15;
constexpr uint32_t MOUSE_WAKE_INTERVAL_MS = 3 * 60 * 1000;
constexpr uint32_t BREATH_PERIOD_MOUSE_JITTER_MS = 2000;
constexpr uint32_t BREATH_UPDATE_INTERVAL_MS = 30;
constexpr uint8_t BREATH_MIN_BRIGHTNESS = 10;
constexpr uint8_t BREATH_MAX_BRIGHTNESS = 255;
constexpr uint8_t RAINBOW_STEP = 1;
constexpr uint32_t KEYBOARD_MIN_INTERVAL_MS = 10;
constexpr size_t WIFI_SSID_MAX_LEN = 32;
constexpr size_t WIFI_PASS_MAX_LEN = 64;
constexpr size_t EEPROM_WIFI_DATA_SIZE =
    1 + WIFI_SSID_MAX_LEN + 1 + WIFI_PASS_MAX_LEN + 1;
constexpr uint8_t EEPROM_WIFI_MAGIC = 0xA5;
constexpr uint8_t OTA_MIN_BLINK_HZ = 1;
constexpr uint8_t OTA_MAX_BLINK_HZ = 20;
constexpr char TIMEZONE[] = "AEST-10AEDT,M10.1.0/2,M4.1.0/3";
constexpr char NTP_SERVER_PRIMARY[] = "pool.ntp.org";
constexpr char NTP_SERVER_SECONDARY[] = "time.nist.gov";
constexpr uint8_t MOUSE_JITTER_ACTIVE_HOUR_START = 8;
constexpr uint8_t MOUSE_JITTER_ACTIVE_HOUR_END = 17;
constexpr uint32_t MOUSE_JITTER_POWERSAVE_FALLBACK_SLEEP_MS = 60000;

// TODO: set these to your Wi-Fi credentials.
constexpr char WIFI_SSID[] = "xxxxxxx";
constexpr char WIFI_PASS[] = "xxxxxxx";
constexpr char OTA_HOSTNAME[] = "esp32-airtype";

Adafruit_NeoPixel pixel(PIXEL_COUNT, NEOPIXEL_PIN, NEO_RGB + NEO_KHZ800);
WiFiServer consoleServer(TCP_CONSOLE_PORT);
WiFiClient consoleClient;
int lastConsoleButtonValue = -1;
USBHIDMouse mouse;
#if ENABLE_KEYBOARD
USBHIDKeyboard keyboard;
#endif
uint32_t lastWifiAttempt = 0;
uint32_t lastAlertToggle = 0;
bool alertState = false;
bool wasConnected = false;
bool otaConfigured = false;
bool otaActive = false;
uint32_t lastIpLog = 0;
bool keyboardTypingActive = false;
uint32_t lastMouseWake = 0;
uint32_t lastBreathUpdate = 0;
uint8_t breathHue = 0;
bool breathInvertColors = false;
uint16_t breathStep = 0;
uint32_t lastKeyboardSendMs = 0;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 15;
bool buttonPressed = false;
bool lastRawButtonPressed = false;
uint32_t lastButtonRawChangeMs = 0;
char activeWifiSsid[WIFI_SSID_MAX_LEN + 1] = {0};
char activeWifiPass[WIFI_PASS_MAX_LEN + 1] = {0};
bool wifiCredentialsSaved = false;
bool otaInProgress = false;
uint8_t otaBlinkFrequencyHz = OTA_MIN_BLINK_HZ;
bool otaBlinkState = false;
uint32_t otaBlinkStartMs = 0;
bool timeConfigured = false;
bool powerSaveModeActive = false;

static bool asciiCharUsesShift(char c) {
  return (c >= 'A' && c <= 'Z') || strchr("~!@#$%^&*()_+{}|:\"<>?", c) != nullptr;
}

static bool sendConsoleButtonState(bool buttonDown, bool force);
static void closeConsoleClient(const char* reason);

static bool loadWifiCredentialsFromEeprom() {
  if (EEPROM.read(0) != EEPROM_WIFI_MAGIC) {
    return false;
  }

  char storedSsid[WIFI_SSID_MAX_LEN + 1] = {0};
  char storedPass[WIFI_PASS_MAX_LEN + 1] = {0};

  for (size_t i = 0; i < WIFI_SSID_MAX_LEN; ++i) {
    storedSsid[i] = static_cast<char>(EEPROM.read(1 + i));
  }
  storedSsid[WIFI_SSID_MAX_LEN] = '\0';

  size_t passOffset = 1 + WIFI_SSID_MAX_LEN + 1;
  for (size_t i = 0; i < WIFI_PASS_MAX_LEN; ++i) {
    storedPass[i] = static_cast<char>(EEPROM.read(passOffset + i));
  }
  storedPass[WIFI_PASS_MAX_LEN] = '\0';

  storedSsid[WIFI_SSID_MAX_LEN] = '\0';
  storedPass[WIFI_PASS_MAX_LEN] = '\0';

  if (storedSsid[0] == '\0') {
    return false;
  }

  strlcpy(activeWifiSsid, storedSsid, sizeof(activeWifiSsid));
  strlcpy(activeWifiPass, storedPass, sizeof(activeWifiPass));
  return true;
}

static void setFallbackWifiCredentials() {
  strlcpy(activeWifiSsid, WIFI_SSID, sizeof(activeWifiSsid));
  strlcpy(activeWifiPass, WIFI_PASS, sizeof(activeWifiPass));
}

static bool saveWifiCredentialsToEeprom(const char* ssid, const char* pass) {
  EEPROM.write(0, EEPROM_WIFI_MAGIC);

  for (size_t i = 0; i < WIFI_SSID_MAX_LEN; ++i) {
    uint8_t value = i < strlen(ssid) ? static_cast<uint8_t>(ssid[i]) : 0;
    EEPROM.write(1 + i, value);
  }
  EEPROM.write(1 + WIFI_SSID_MAX_LEN, 0);

  size_t passOffset = 1 + WIFI_SSID_MAX_LEN + 1;
  for (size_t i = 0; i < WIFI_PASS_MAX_LEN; ++i) {
    uint8_t value = i < strlen(pass) ? static_cast<uint8_t>(pass[i]) : 0;
    EEPROM.write(passOffset + i, value);
  }
  EEPROM.write(passOffset + WIFI_PASS_MAX_LEN, 0);

  return EEPROM.commit();
}

static void beginWifiConnect() {
  Serial.printf("Connecting to Wi-Fi SSID: %s\n", activeWifiSsid);
  WiFi.begin(activeWifiSsid, activeWifiPass);
  lastWifiAttempt = millis();
}

static void ensureTimeConfigured() {
  if (timeConfigured) {
    return;
  }
  configTzTime(TIMEZONE, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);
  timeConfigured = true;
  Serial.printf("Time sync configured for timezone: %s\n", TIMEZONE);
}

static inline void taskDelayMs(uint32_t ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool getLocalTimeSnapshot(struct tm* localTime, time_t* nowOut) {
  time_t now = time(nullptr);
  if (now < 100000) {
    return false;
  }
  if (localtime_r(&now, localTime) == nullptr) {
    return false;
  }
  if (nowOut != nullptr) {
    *nowOut = now;
  }
  return true;
}

static void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  uint32_t now = millis();
  if (now - lastWifiAttempt >= WIFI_RETRY_INTERVAL_MS) {
    Serial.println("Wi-Fi not connected, retrying...");
    WiFi.disconnect();
    beginWifiConnect();
  }
}

static void applySolidColor(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, r, g, b);
  pixel.show();
}

static void setPowerSaveMode(bool enable) {
  if (powerSaveModeActive == enable) {
    return;
  }

  powerSaveModeActive = enable;
  WiFi.setSleep(enable);
  Serial.printf("Power save mode %s\n", enable ? "ENABLED" : "DISABLED");
}

static uint32_t wheelColor(uint8_t pos) {
  pos = 255 - pos;
  uint8_t r, g, b;
  if (pos < 85) {
    r = 255 - pos * 3;
    g = 0;
    b = pos * 3;
  } else if (pos < 170) {
    pos -= 85;
    r = 0;
    g = pos * 3;
    b = 255 - pos * 3;
  } else {
    pos -= 170;
    r = pos * 3;
    g = 255 - pos * 3;
    b = 0;
  }
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void updateBreathingLed(uint32_t breathPeriodMs) {
  uint32_t now = millis();
  if (now - lastBreathUpdate < BREATH_UPDATE_INTERVAL_MS) {
    return;
  }
  lastBreathUpdate = now;
  breathHue = static_cast<uint8_t>(breathHue + RAINBOW_STEP);
  uint32_t rampSteps = max<uint32_t>(1, breathPeriodMs / (2 * BREATH_UPDATE_INTERVAL_MS));
  if (breathStep >= (rampSteps * 2)) {
    breathStep = 0;
  }

  uint32_t normalizedStep =
      breathStep <= rampSteps ? breathStep : (2 * rampSteps) - breathStep;
  uint8_t brightness = BREATH_MIN_BRIGHTNESS +
                       ((BREATH_MAX_BRIGHTNESS - BREATH_MIN_BRIGHTNESS) *
                        normalizedStep) /
                           rampSteps;
  breathStep = (breathStep + 1) % (2 * rampSteps);
  uint32_t color = wheelColor(breathHue);
  uint8_t r = (color >> 16) & 0xFF;
  uint8_t g = (color >> 8) & 0xFF;
  uint8_t b = color & 0xFF;
  r = (r * brightness) / 255;
  g = (g * brightness) / 255;
  b = (b * brightness) / 255;
  if (breathInvertColors) {
    r = 255 - r;
    g = 255 - g;
    b = 255 - b;
  }
  applySolidColor(r, g, b);
}

static void showAlertPattern() {
  uint32_t now = millis();
  if (lastAlertToggle == 0) {
    lastAlertToggle = now;
    alertState = true;
    applySolidColor(255, 0, 0);
    return;
  }
  if (now - lastAlertToggle >= ALERT_BLINK_INTERVAL_MS) {
    lastAlertToggle = now;
    alertState = !alertState;
    if (alertState) {
      applySolidColor(255, 0, 0);
    } else {
      applySolidColor(0, 0, 255);
    }
  }
}

static void updateOtaBlinkLed() {
  uint32_t now = millis();
  uint32_t elapsedMs = now - otaBlinkStartMs;
  uint32_t halfCycles =
      (elapsedMs * static_cast<uint32_t>(otaBlinkFrequencyHz) * 2UL) / 1000UL;
  bool shouldBeOn = (halfCycles % 2UL) == 0;
  if (shouldBeOn == otaBlinkState) {
    return;
  }

  otaBlinkState = shouldBeOn;
  if (shouldBeOn) {
    applySolidColor(0, 0, 255);
  } else {
    applySolidColor(0, 0, 0);
  }
}

static bool isMouseJitterWindowActive() {
  struct tm localTime = {};
  if (!getLocalTimeSnapshot(&localTime, nullptr)) {
    return false;
  }

  bool isWeekday = localTime.tm_wday >= 1 && localTime.tm_wday <= 5;
  bool inActiveHours = localTime.tm_hour >= MOUSE_JITTER_ACTIVE_HOUR_START &&
                       localTime.tm_hour < MOUSE_JITTER_ACTIVE_HOUR_END;
  return isWeekday && inActiveHours;
}

static uint64_t getMouseJitterPowerSaveSleepUs() {
  struct tm localTime = {};
  time_t now = 0;
  if (!getLocalTimeSnapshot(&localTime, &now)) {
    return static_cast<uint64_t>(MOUSE_JITTER_POWERSAVE_FALLBACK_SLEEP_MS) *
           1000ULL;
  }

  struct tm nextWake = localTime;
  nextWake.tm_sec = 0;
  nextWake.tm_min = 0;
  nextWake.tm_hour = MOUSE_JITTER_ACTIVE_HOUR_START;

  if (localTime.tm_wday >= 1 && localTime.tm_wday <= 5 &&
      localTime.tm_hour < MOUSE_JITTER_ACTIVE_HOUR_START) {
    // Wake at 08:00 today.
  } else {
    nextWake.tm_mday += 1;
    do {
      nextWake.tm_wday = -1;
      mktime(&nextWake);
    } while (nextWake.tm_wday == 0 || nextWake.tm_wday == 6);
  }

  time_t nextWakeEpoch = mktime(&nextWake);
  if (nextWakeEpoch <= now) {
    return static_cast<uint64_t>(MOUSE_JITTER_POWERSAVE_FALLBACK_SLEEP_MS) *
           1000ULL;
  }

  return static_cast<uint64_t>(nextWakeEpoch - now) * 1000000ULL;
}

static void enterMouseJitterPowerSave() {
  if (consoleClient && consoleClient.connected()) {
    sendConsoleButtonState(false, true);
    closeConsoleClient("terminated (power save)");
  }

  applySolidColor(0, 0, 0);
  setPowerSaveMode(true);
  esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BUTTON_PIN), 0);
  esp_sleep_enable_timer_wakeup(getMouseJitterPowerSaveSleepUs());
  Serial.println("Entering light sleep for inactive mouse jitter mode");
  esp_light_sleep_start();
}

enum class LedMode : uint8_t {
  Alert,
  OtaBlink,
  InputReady,
  JitterBreathing,
  Off,
};

static LedMode determineLedMode(bool connected, bool pressedSnapshot,
                                bool mouseJitterWindowActive) {
  if (otaInProgress) {
    return LedMode::OtaBlink;
  }
  if (pressedSnapshot) {
    return LedMode::InputReady;
  }
  if (!mouseJitterWindowActive) {
    return LedMode::Off;
  }
  if (!connected) {
    return LedMode::Alert;
  }
  return LedMode::JitterBreathing;
}

static void renderLedMode(LedMode mode) {
  switch (mode) {
    case LedMode::Alert:
      showAlertPattern();
      break;
    case LedMode::OtaBlink:
      updateOtaBlinkLed();
      break;
    case LedMode::InputReady:
      applySolidColor(0, 255, 0);
      break;
    case LedMode::JitterBreathing:
      updateBreathingLed(BREATH_PERIOD_MOUSE_JITTER_MS);
      break;
    case LedMode::Off:
      applySolidColor(0, 0, 0);
      break;
  }
}

static void persistActiveWifiCredentialsIfNeeded() {
  if (wifiCredentialsSaved || WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (saveWifiCredentialsToEeprom(activeWifiSsid, activeWifiPass)) {
    wifiCredentialsSaved = true;
    Serial.printf("Saved Wi-Fi credentials to EEPROM for SSID: %s\n",
                  activeWifiSsid);
  } else {
    Serial.println("Failed to save Wi-Fi credentials to EEPROM");
  }
}

static void logIpAndMac(const char* prefix) {
  String ip = WiFi.localIP().toString();
  String mac = WiFi.macAddress();
  Serial.printf("%s IP: %s, MAC: %s\n", prefix, ip.c_str(), mac.c_str());
}

static void printIpAddress() {
  String ip = WiFi.localIP().toString();
  Serial.printf("IP address: %s\n", ip.c_str());
}

static bool sendConsoleButtonState(bool buttonDown, bool force) {
  if (!consoleClient || !consoleClient.connected()) {
    return false;
  }
  int currentValue = buttonDown ? 1 : 0;
  if (!force && currentValue == lastConsoleButtonValue) {
    return true;
  }
  size_t written = consoleClient.print(buttonDown ? "1\n" : "0\n");
  if (written == 0) {
    return false;
  }
  lastConsoleButtonValue = currentValue;
  Serial.printf("Console TX state: %d\n", currentValue);
  return true;
}

static void closeConsoleClient(const char* reason) {
  if (!consoleClient) {
    return;
  }
  Serial.printf("Console client %s\n", reason);
  consoleClient.stop();
  lastConsoleButtonValue = -1;
}

static bool updateDebouncedButtonState() {
  bool rawPressed = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();
  if (rawPressed != lastRawButtonPressed) {
    lastRawButtonPressed = rawPressed;
    lastButtonRawChangeMs = now;
  }
  if (rawPressed != buttonPressed &&
      now - lastButtonRawChangeMs >= BUTTON_DEBOUNCE_MS) {
    buttonPressed = rawPressed;
    return true;
  }
  return false;
}

static void handleTcpConsoleInput(bool buttonDown) {
  if (WiFi.status() != WL_CONNECTED) {
    if (consoleClient) {
      closeConsoleClient("dropped (Wi-Fi lost)");
    }
    return;
  }

  if (!consoleClient || !consoleClient.connected()) {
    if (consoleClient) {
      closeConsoleClient("disconnected");
    }
    WiFiClient candidate = consoleServer.available();
    if (candidate) {
      candidate.setNoDelay(true);
      consoleClient = candidate;
      Serial.printf("Console client connected from %s:%u\n",
                    consoleClient.remoteIP().toString().c_str(),
                    consoleClient.remotePort());
      lastConsoleButtonValue = -1;
      if (!sendConsoleButtonState(buttonDown, true)) {
        Serial.println("Console state send pending after connect");
      }
    }
    return;
  }

  while (consoleClient.available()) {
    char raw = static_cast<char>(consoleClient.read());
    char c = raw == '\r' ? '\n' : raw;
    if (c == '\n') {
      Serial.println("Console RX: <newline>");
    } else if (c >= 32 && c <= 126) {
      Serial.printf("Console RX: %c\n", c);
    } else {
      Serial.printf("Console RX: 0x%02X\n", static_cast<unsigned char>(c));
    }
#if ENABLE_KEYBOARD
    if (buttonDown && keyboardTypingActive) {
      uint32_t now = millis();
      uint32_t elapsed = now - lastKeyboardSendMs;
      if (elapsed < KEYBOARD_MIN_INTERVAL_MS) {
        taskDelayMs(KEYBOARD_MIN_INTERVAL_MS - elapsed);
        now = millis();
      }
      bool needsShift = asciiCharUsesShift(c);
      keyboard.write(static_cast<uint8_t>(c));
      taskDelayMs(1);
      keyboard.releaseAll();
      if (needsShift) {
        taskDelayMs(1);
        keyboard.releaseAll();
      }
      lastKeyboardSendMs = now;
    }
#endif
  }

  if (!consoleClient.connected()) {
    closeConsoleClient("disconnected");
    return;
  }

  if (!sendConsoleButtonState(buttonDown, false) && !consoleClient.connected()) {
    closeConsoleClient("disconnected during state send");
  }
}

static void ensureOtaConfigured() {
  if (otaConfigured) {
    return;
  }
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    otaBlinkFrequencyHz = OTA_MIN_BLINK_HZ;
    otaBlinkState = true;
    otaBlinkStartMs = millis();
    applySolidColor(0, 0, 255);
    Serial.printf("OTA start (%s)\n",
                  ArduinoOTA.getCommand() == U_FLASH ? "sketch" : "filesystem");
  });
  ArduinoOTA.onEnd([]() {
    otaInProgress = false;
    applySolidColor(0, 255, 0);
    Serial.println("\nOTA end");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    uint8_t percent = total == 0 ? 0 : static_cast<uint8_t>((progress * 100U) / total);
    otaBlinkFrequencyHz = static_cast<uint8_t>(
        OTA_MIN_BLINK_HZ +
        ((static_cast<uint32_t>(percent) * (OTA_MAX_BLINK_HZ - OTA_MIN_BLINK_HZ)) /
         100U));
    Serial.printf("OTA progress: %u%%, blink: %uHz\n", percent, otaBlinkFrequencyHz);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    Serial.printf("OTA Error[%u]: ", error);
    switch (error) {
      case OTA_AUTH_ERROR:
        Serial.println("Auth Failed");
        break;
      case OTA_BEGIN_ERROR:
        Serial.println("Begin Failed");
        break;
      case OTA_CONNECT_ERROR:
        Serial.println("Connect Failed");
        break;
      case OTA_RECEIVE_ERROR:
        Serial.println("Receive Failed");
        break;
      case OTA_END_ERROR:
        Serial.println("End Failed");
        break;
      default:
        Serial.println("Unknown error");
        break;
    }
  });
  otaConfigured = true;
}

static void beginOtaIfConnected() {
  if (otaActive || WiFi.status() != WL_CONNECTED) {
    return;
  }
  ensureOtaConfigured();
  ArduinoOTA.begin();
  otaActive = true;
  Serial.printf("OTA ready: %s.local (%s)\n", OTA_HOSTNAME,
                WiFi.localIP().toString().c_str());
}

#if ENABLE_KEYBOARD
static void updateKeyboardTypingState(bool enable) {
  if (enable == keyboardTypingActive) {
    return;
  }
  keyboardTypingActive = enable;
  if (keyboardTypingActive) {
    Serial.println("Keyboard typing ENABLED");
  } else {
    keyboard.releaseAll();
    Serial.println("Keyboard typing DISABLED");
  }
}

#endif

static void performMouseWakeJiggle() {
  constexpr int8_t STEP = 10;
  constexpr uint32_t STEP_DELAY_MS = 2;
  static const int8_t moves[4][2] = {
      {STEP, 0}, {0, STEP}, {-STEP, 0}, {0, -STEP},
  };
  uint32_t idx = esp_random() % 4;
  int8_t dx = moves[idx][0];
  int8_t dy = moves[idx][1];
  mouse.move(dx, dy, 0);
  taskDelayMs(STEP_DELAY_MS);
  mouse.move(-dx, -dy, 0);
  breathInvertColors = !breathInvertColors;
}

static void printChipInfo() {
  Serial.println("=== ESP32-S3 Mini Info ===");
  Serial.printf("Chip model: %s\n", ESP.getChipModel());
  Serial.printf("Chip revision: %d\n", ESP.getChipRevision());
  Serial.printf("CPU frequency: %d MHz\n", ESP.getCpuFreqMHz());
  Serial.printf("Flash size: %u MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf("Sketch size / free: %u KB / %u KB\n",
                ESP.getSketchSize() / 1024, ESP.getFreeSketchSpace() / 1024);
  Serial.printf("Heap size / free: %u KB / %u KB\n",
                ESP.getHeapSize() / 1024, ESP.getFreeHeap() / 1024);
  Serial.println("==========================");
}

void setup() {
  USB.begin();
  Serial.begin(115200);
  delay(250);
  while (!Serial && millis() < 2000) {
    taskDelayMs(10);
  }
  Serial.println("ESP32-S3 Mini setup complete. Driving NeoPixel on pin 21.");
  pixel.begin();
  pixel.setBrightness(PIXEL_BRIGHTNESS);
  pixel.clear();
  pixel.show();
  mouse.begin();
#if ENABLE_KEYBOARD
  keyboard.begin();
#endif
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  bool initialPressed = digitalRead(BUTTON_PIN) == LOW;
  buttonPressed = initialPressed;
  lastRawButtonPressed = initialPressed;
  lastButtonRawChangeMs = millis();
#if ENABLE_KEYBOARD
  updateKeyboardTypingState(initialPressed);
#endif
  if (!EEPROM.begin(EEPROM_WIFI_DATA_SIZE)) {
    Serial.println("EEPROM init failed, using fallback Wi-Fi credentials");
    setFallbackWifiCredentials();
  } else {
    if (loadWifiCredentialsFromEeprom()) {
      Serial.printf("Loaded Wi-Fi credentials from EEPROM for SSID: %s\n",
                    activeWifiSsid);
      wifiCredentialsSaved = true;
    } else {
      Serial.println("No Wi-Fi credentials found in EEPROM, using fallback");
      setFallbackWifiCredentials();
    }
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  ensureTimeConfigured();
  beginWifiConnect();
  ensureOtaConfigured();

  consoleServer.begin();
  Serial.printf("TCP console listening on port %u\n", TCP_CONSOLE_PORT);
  printChipInfo();
  beginOtaIfConnected();
}

void loop() {
  maintainWifi();
  bool connected = WiFi.status() == WL_CONNECTED;
  bool stateChanged = updateDebouncedButtonState();
  bool pressedSnapshot = buttonPressed;
#if ENABLE_KEYBOARD
  updateKeyboardTypingState(pressedSnapshot);
#endif
  bool mouseJitterWindowActive = isMouseJitterWindowActive();
  bool mouseJitterPowerSave = !pressedSnapshot && !mouseJitterWindowActive && !otaInProgress;
  LedMode ledMode = determineLedMode(connected, pressedSnapshot, mouseJitterWindowActive);
  setPowerSaveMode(mouseJitterPowerSave);
  if (stateChanged && !pressedSnapshot && ledMode == LedMode::Off) {
    applySolidColor(0, 0, 0);
  }
  uint32_t nowMs = millis();
  if (!pressedSnapshot && mouseJitterWindowActive &&
      nowMs - lastMouseWake >= MOUSE_WAKE_INTERVAL_MS) {
    performMouseWakeJiggle();
    lastMouseWake = nowMs;
  }
  if (stateChanged && !pressedSnapshot && consoleClient && consoleClient.connected()) {
    sendConsoleButtonState(false, true);
    closeConsoleClient("terminated (button released)");
  }
  if (!mouseJitterPowerSave) {
    handleTcpConsoleInput(pressedSnapshot);
  }

  if (connected && !wasConnected) {
    printIpAddress();
    logIpAndMac("Wi-Fi connected,");
    persistActiveWifiCredentialsIfNeeded();
  } else if (!connected && wasConnected) {
    Serial.println("Wi-Fi disconnected, entering alert mode.");
    lastIpLog = 0;
    lastAlertToggle = 0;
  }
  wasConnected = connected;

  if (connected) {
    ensureTimeConfigured();
    beginOtaIfConnected();
    persistActiveWifiCredentialsIfNeeded();
    ledMode = determineLedMode(connected, pressedSnapshot, mouseJitterWindowActive);
    renderLedMode(ledMode);
    if (millis() - lastIpLog >= 2000) {
      logIpAndMac("Wi-Fi connected,");
      lastIpLog = millis();
    }
  } else {
    otaActive = false;
    renderLedMode(ledMode);
  }

  if (stateChanged) {
    Serial.printf("Button state changed: %s\n",
                  pressedSnapshot ? "PRESSED" : "RELEASED");
  }
  if (otaActive) {
    ArduinoOTA.handle();
  }
  if (mouseJitterPowerSave) {
    enterMouseJitterPowerSave();
    return;
  }
  uint32_t loopDelay =
      wasConnected ? LOOP_DELAY_CONNECTED_MS : LOOP_DELAY_DISCONNECTED_MS;
  taskDelayMs(loopDelay);
}
