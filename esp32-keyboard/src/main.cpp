#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <time.h>
#include "esp_random.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint16_t ALERT_BLINK_INTERVAL_MS = 150;
constexpr uint8_t BUTTON_PIN = 8;
constexpr uint8_t PIXEL_BRIGHTNESS = 255;
constexpr uint16_t TCP_CONSOLE_PORT = 9000;
constexpr uint16_t LOOP_DELAY_CONNECTED_MS = 2;
constexpr uint16_t LOOP_DELAY_DISCONNECTED_MS = 15;
constexpr uint16_t LOOP_DELAY_POWERSAVE_MS = 1000;
constexpr uint32_t JITTER_WIFI_TIMEOUT_MS = 2 * 60 * 1000UL;
constexpr uint32_t MOUSE_WAKE_INTERVAL_MS = 3 * 60 * 1000;
constexpr uint8_t INPUT_TRANSFER_BLINK_HZ = 10;
constexpr uint16_t INPUT_TRANSFER_BLINK_HALF_PERIOD_MS =
    1000 / (INPUT_TRANSFER_BLINK_HZ * 2);
constexpr uint16_t INPUT_TRANSFER_ACTIVE_MS = 250;
constexpr uint32_t BREATH_PERIOD_MOUSE_JITTER_MS = 2000;
constexpr uint32_t BREATH_UPDATE_INTERVAL_MS = 30;
constexpr uint8_t BREATH_MIN_BRIGHTNESS = 10;
constexpr uint8_t BREATH_MAX_BRIGHTNESS = 255;
constexpr uint8_t RAINBOW_STEP = 1;
constexpr uint16_t DEFAULT_KEYBOARD_RATE_CPS = 100;
constexpr uint16_t MIN_KEYBOARD_RATE_CPS = 1;
constexpr uint16_t MAX_KEYBOARD_RATE_CPS = 1000;
constexpr uint16_t CONSOLE_ACK_INTERVAL_CHARS = 32;
constexpr uint16_t CONSOLE_ACK_INTERVAL_MS = 250;
constexpr uint16_t CONSOLE_BUFFER_REPORT_DELTA = 512;
constexpr uint8_t SHIFTED_SYMBOL_DELAY_MS = 2;
constexpr size_t INPUT_BUFFER_SIZE = 16 * 1024;
constexpr char CONTROL_FRAME_PREFIX = 0x1E;
constexpr size_t CONTROL_FRAME_MAX_LEN = 31;
#if CONFIG_FREERTOS_UNICORE
constexpr BaseType_t KEYBOARD_TASK_CORE = 0;
#else
constexpr BaseType_t KEYBOARD_TASK_CORE = 1;
#endif
constexpr UBaseType_t KEYBOARD_TASK_PRIORITY = 2;
constexpr size_t WIFI_SSID_MAX_LEN = 32;
constexpr size_t WIFI_PASS_MAX_LEN = 64;
constexpr uint8_t MAX_WIFI_NETWORKS = 4;
struct WifiCredentials {
  char ssid[WIFI_SSID_MAX_LEN + 1];
  char pass[WIFI_PASS_MAX_LEN + 1];
};
constexpr uint8_t EEPROM_WIFI_LEGACY_MAGIC = 0xA5;
constexpr uint8_t EEPROM_WIFI_LIST_MAGIC = 0xA6;
constexpr size_t EEPROM_WIFI_DATA_SIZE = 2 + MAX_WIFI_NETWORKS * sizeof(WifiCredentials);
constexpr uint8_t OTA_MIN_BLINK_HZ = 1;
constexpr uint8_t OTA_MAX_BLINK_HZ = 20;
constexpr char TIMEZONE[] = "AEST-10AEDT,M10.1.0/2,M4.1.0/3";
constexpr char NTP_SERVER_PRIMARY[] = "pool.ntp.org";
constexpr char NTP_SERVER_SECONDARY[] = "time.nist.gov";
constexpr uint8_t MOUSE_JITTER_ACTIVE_HOUR_START = 8;
constexpr uint8_t MOUSE_JITTER_ACTIVE_HOUR_END = 17;
constexpr uint32_t TIME_SYNC_INTERVAL_MS = 60 * 60 * 1000UL;
constexpr int16_t INPUT_TOKEN_KEY_LEFT = 0x101;
constexpr int16_t INPUT_TOKEN_KEY_RIGHT = 0x102;
constexpr int16_t INPUT_TOKEN_KEY_SPACE = 0x103;

// One-time EEPROM migration seed only; never used directly for connection.
// Once the list exists, changes here do not modify saved credentials.
constexpr char WIFI_SSID[] = "Waverley Guest";
constexpr char WIFI_PASS[] = "";
constexpr char OTA_HOSTNAME[] = "esp32-keyboard";

Adafruit_NeoPixel pixel(PIXEL_COUNT, NEOPIXEL_PIN, NEO_RGB + NEO_KHZ800);
WiFiServer consoleServer(TCP_CONSOLE_PORT);
WiFiClient consoleClient;
int lastConsoleButtonValue = -1;
int lastConsoleBufferFree = -1;
uint32_t lastAcceptedAckCount = 0;
uint32_t lastTypedAckCount = 0;
uint32_t lastAcceptedAckMs = 0;
uint32_t lastTypedAckMs = 0;
volatile uint32_t acceptedCharCount = 0;
volatile uint32_t typedCharCount = 0;
volatile uint32_t expectedFileTypedCount = 0;
volatile uint32_t inputAbortGeneration = 0;
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
volatile bool keyboardTypingActive = false;
uint32_t lastMouseWake = 0;
uint32_t jitterWifiDisconnectedSince = 0;
bool jitterWifiTimeoutActive = false;
uint32_t lastBreathUpdate = 0;
uint8_t breathHue = 0;
bool breathInvertColors = false;
uint16_t breathStep = 0;
uint32_t lastKeyboardSendUs = 0;
volatile uint16_t keyboardRateCps = DEFAULT_KEYBOARD_RATE_CPS;
volatile uint32_t keyboardCharIntervalUs = 1000000UL / DEFAULT_KEYBOARD_RATE_CPS;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 15;
bool buttonPressed = false;
bool lastRawButtonPressed = false;
uint32_t lastButtonRawChangeMs = 0;
char activeWifiSsid[WIFI_SSID_MAX_LEN + 1] = {0};
char activeWifiPass[WIFI_PASS_MAX_LEN + 1] = {0};
WifiCredentials savedWifiNetworks[MAX_WIFI_NETWORKS] = {};
uint8_t savedWifiCount = 0;
bool visibleWifiNetworks[MAX_WIFI_NETWORKS] = {};
uint8_t nextWifiCandidate = 0;
bool wifiScanRunning = false;
bool wifiAttemptRunning = false;
bool wifiWasConnected = false;
uint32_t nextWifiScanStartedMs = 0;
volatile bool otaInProgress = false;
volatile uint8_t otaBlinkFrequencyHz = OTA_MIN_BLINK_HZ;
bool otaBlinkState = false;
uint32_t otaBlinkStartMs = 0;
bool timeConfigured = false;
bool powerSaveModeActive = false;
TaskHandle_t inputTransferLedTaskHandle = nullptr;
TaskHandle_t keyboardInputTaskHandle = nullptr;
TaskHandle_t otaLedTaskHandle = nullptr;
SemaphoreHandle_t ledMutex = nullptr;
SemaphoreHandle_t consoleTxMutex = nullptr;
SemaphoreHandle_t inputBufferMutex = nullptr;
volatile bool inputTransferBlinkActive = false;
bool controlFrameActive = false;
char controlFrameBuffer[CONTROL_FRAME_MAX_LEN + 1] = {0};
size_t controlFrameLength = 0;
volatile bool fileEndRequested = false;
volatile bool fileFinishedAckSent = false;
#if ENABLE_KEYBOARD
int16_t inputBuffer[INPUT_BUFFER_SIZE] = {0};
size_t inputBufferHead = 0;
size_t inputBufferTail = 0;
size_t inputBufferCount = 0;
#endif

static bool sendConsoleButtonState(bool buttonDown, bool force);
static bool sendConsoleAcceptedAck(bool force);
static bool sendConsoleTypedAck(bool force);
static bool sendConsoleRateAck();
static bool sendConsoleBufferFinishedAck();
static bool sendConsoleFileFinishedAck();
static bool sendConsoleBufferState(bool force);
static void serviceConsoleStatusAcks();
static size_t getInputBufferFree();
static bool handleControlFrameChar(char c);
static void closeConsoleClient(const char* reason);
static void restoreWifiStationMode();
static void startOtaLedTask();
#if ENABLE_KEYBOARD
static bool keyboardInputCancelled(uint32_t abortGeneration);
static size_t writeKeyboardToken(int16_t token, uint32_t abortGeneration);
static void resetInputBuffer();
static void startKeyboardInputTask();
static bool enqueueInputToken(int16_t token);
#endif

static bool readWifiEntry(size_t offset, WifiCredentials& entry) {
  EEPROM.get(offset, entry);
  return entry.ssid[0] != '\0' &&
         memchr(entry.ssid, '\0', sizeof(entry.ssid)) != nullptr &&
         memchr(entry.pass, '\0', sizeof(entry.pass)) != nullptr;
}

static void loadWifiList() {
  if (!EEPROM.begin(EEPROM_WIFI_DATA_SIZE)) {
    Serial.println("EEPROM unavailable; Wi-Fi disabled (no hardcoded fallback)");
    return;
  }
  if (EEPROM.read(0) == EEPROM_WIFI_LIST_MAGIC) {
    uint8_t count = EEPROM.read(1);
    if (count > MAX_WIFI_NETWORKS) {
      Serial.println("Invalid EEPROM Wi-Fi list; Wi-Fi disabled");
      return;
    }
    for (uint8_t i = 0; i < count; ++i) {
      WifiCredentials entry = {};
      if (readWifiEntry(2 + i * sizeof(entry), entry)) {
        savedWifiNetworks[savedWifiCount++] = entry;
      }
    }
  } else {
    // Preserve the old single saved network as the first list entry.
    WifiCredentials legacy = {};
    if (EEPROM.read(0) == EEPROM_WIFI_LEGACY_MAGIC && readWifiEntry(1, legacy)) {
      savedWifiNetworks[savedWifiCount++] = legacy;
    } else if (EEPROM.read(0) != 0xFF) {
      Serial.println("Unrecognized EEPROM Wi-Fi data; leaving it unchanged");
      return;
    }
    // Add the current credentials once, without duplicating an identical pair.
    if (savedWifiCount == 0 || strcmp(legacy.ssid, WIFI_SSID) != 0 ||
        strcmp(legacy.pass, WIFI_PASS) != 0) {
      WifiCredentials& seed = savedWifiNetworks[savedWifiCount++];
      strlcpy(seed.ssid, WIFI_SSID, sizeof(seed.ssid));
      strlcpy(seed.pass, WIFI_PASS, sizeof(seed.pass));
    }
    EEPROM.write(0, EEPROM_WIFI_LIST_MAGIC);
    EEPROM.write(1, savedWifiCount);
    for (uint8_t i = 0; i < savedWifiCount; ++i) {
      EEPROM.put(2 + i * sizeof(WifiCredentials), savedWifiNetworks[i]);
    }
    if (!EEPROM.commit()) {
      savedWifiCount = 0;
      Serial.println("Failed to save Wi-Fi list; Wi-Fi disabled");
      return;
    }
    // Connection candidates must be read back from the saved list.
    for (uint8_t i = 0; i < savedWifiCount; ++i) {
      EEPROM.get(2 + i * sizeof(WifiCredentials), savedWifiNetworks[i]);
    }
    Serial.println("Migrated Wi-Fi credentials to EEPROM list");
  }
  for (uint8_t i = 0; i < savedWifiCount; ++i) {
    Serial.printf("EEPROM Wi-Fi %u: %s\n", i + 1, savedWifiNetworks[i].ssid);
  }
}

static void startWifiScan() {
  if (savedWifiCount == 0) return;
  WiFi.disconnect();
  WiFi.scanDelete();
  wifiAttemptRunning = false;
  nextWifiCandidate = 0;
  memset(visibleWifiNetworks, 0, sizeof(visibleWifiNetworks));
  Serial.println("Scanning for saved Wi-Fi networks...");
  int16_t result = WiFi.scanNetworks(true);
  wifiScanRunning = result != WIFI_SCAN_FAILED;
  nextWifiScanStartedMs = millis();
  if (!wifiScanRunning) Serial.println("Wi-Fi scan failed; will retry");
}

static void tryNextWifiNetwork() {
  while (nextWifiCandidate < savedWifiCount) {
    uint8_t index = nextWifiCandidate++;
    if (!visibleWifiNetworks[index]) {
      Serial.printf("Skipping absent EEPROM Wi-Fi: %s\n", savedWifiNetworks[index].ssid);
      continue;
    }
    strlcpy(activeWifiSsid, savedWifiNetworks[index].ssid, sizeof(activeWifiSsid));
    strlcpy(activeWifiPass, savedWifiNetworks[index].pass, sizeof(activeWifiPass));
    Serial.printf("Connecting to EEPROM Wi-Fi %u: %s\n", index + 1, activeWifiSsid);
    WiFi.begin(activeWifiSsid, activeWifiPass);
    lastWifiAttempt = millis();
    wifiAttemptRunning = true;
    return;
  }
  Serial.println("No remaining matching Wi-Fi networks; will rescan");
  wifiAttemptRunning = false;
  nextWifiScanStartedMs = millis();
}

static void restoreWifiStationMode() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
}

static void ensureTimeConfigured() {
  if (timeConfigured) {
    return;
  }
  sntp_set_sync_interval(TIME_SYNC_INTERVAL_MS);
  configTzTime(TIMEZONE, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);
  timeConfigured = true;
  Serial.printf("Time sync configured for timezone: %s, interval: %ums\n",
                TIMEZONE, TIME_SYNC_INTERVAL_MS);
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
  if (savedWifiCount == 0) return;
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiAttemptRunning) {
      Serial.printf("Wi-Fi connected after %lu ms (boot %lu ms)\n",
                    static_cast<unsigned long>(millis() - lastWifiAttempt),
                    static_cast<unsigned long>(millis()));
    }
    wifiAttemptRunning = false;
    wifiWasConnected = true;
    return;
  }
  if (wifiWasConnected) {
    wifiWasConnected = false;
    startWifiScan();
    return;
  }
  if (wifiScanRunning) {
    int16_t count = WiFi.scanComplete();
    if (count == WIFI_SCAN_RUNNING) return;
    wifiScanRunning = false;
    if (count < 0) {
      Serial.println("Wi-Fi scan failed; will retry");
      WiFi.scanDelete();
      nextWifiScanStartedMs = millis();
      return;
    }
    for (uint8_t i = 0; i < savedWifiCount; ++i) {
      for (int16_t j = 0; j < count; ++j) {
        if (WiFi.SSID(j) == savedWifiNetworks[i].ssid) visibleWifiNetworks[i] = true;
      }
    }
    WiFi.scanDelete();
    tryNextWifiNetwork();
    return;
  }
  if (wifiAttemptRunning) {
    if (millis() - lastWifiAttempt < WIFI_CONNECT_TIMEOUT_MS) return;
    Serial.printf("Wi-Fi connection timed out: %s; trying next match\n", activeWifiSsid);
    WiFi.disconnect();
    wifiAttemptRunning = false;
    tryNextWifiNetwork();
    return;
  }
  if (millis() - nextWifiScanStartedMs >= WIFI_RETRY_INTERVAL_MS) startWifiScan();
}

static void applySolidColor(uint8_t r, uint8_t g, uint8_t b) {
  if (ledMutex != nullptr) {
    xSemaphoreTake(ledMutex, portMAX_DELAY);
  }
  pixel.setPixelColor(0, r, g, b);
  pixel.show();
  if (ledMutex != nullptr) {
    xSemaphoreGive(ledMutex);
  }
}

static void setPowerSaveMode(bool enable) {
  if (powerSaveModeActive == enable) {
    return;
  }

  powerSaveModeActive = enable;
  if (enable) {
    if (consoleClient) {
      closeConsoleClient("terminated (Wi-Fi power save)");
    }
    WiFi.setSleep(true);
  } else {
    restoreWifiStationMode();
  }
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

static void notifyOtaLedTask() {
  if (otaLedTaskHandle != nullptr) {
    xTaskNotifyGive(otaLedTaskHandle);
  }
}

static void otaLedTask(void* parameter) {
  (void)parameter;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    bool ledOn = false;

    while (otaInProgress) {
      uint8_t blinkHz = otaBlinkFrequencyHz;
      if (blinkHz < OTA_MIN_BLINK_HZ) {
        blinkHz = OTA_MIN_BLINK_HZ;
      } else if (blinkHz > OTA_MAX_BLINK_HZ) {
        blinkHz = OTA_MAX_BLINK_HZ;
      }

      ledOn = !ledOn;
      applySolidColor(0, 0, ledOn ? 255 : 0);
      uint32_t halfPeriodMs = max<uint32_t>(1, 1000UL / (static_cast<uint32_t>(blinkHz) * 2UL));
      taskDelayMs(halfPeriodMs);
    }
  }
}

static void startOtaLedTask() {
  BaseType_t created = xTaskCreate(otaLedTask, "ota-led", 2048, nullptr, 2,
                                   &otaLedTaskHandle);
  if (created != pdPASS) {
    otaLedTaskHandle = nullptr;
    Serial.println("Failed to create OTA LED task");
  }
}

enum class JitterWindowState : uint8_t {
  Unknown,
  Active,
  Inactive,
};

static JitterWindowState getJitterWindowState() {
  struct tm localTime = {};
  if (!getLocalTimeSnapshot(&localTime, nullptr)) {
    return JitterWindowState::Unknown;
  }

  bool isWeekday = localTime.tm_wday >= 1 && localTime.tm_wday <= 5;
  bool inActiveHours = localTime.tm_hour >= MOUSE_JITTER_ACTIVE_HOUR_START &&
                       localTime.tm_hour < MOUSE_JITTER_ACTIVE_HOUR_END;
  return isWeekday && inActiveHours ? JitterWindowState::Active
                                    : JitterWindowState::Inactive;
}

enum class LedMode : uint8_t {
  Alert,
  OtaBlink,
  InputReady,
  JitterBreathing,
  Off,
};

static LedMode determineLedMode(bool connected, bool pressedSnapshot,
                                JitterWindowState jitterWindowState) {
  if (otaInProgress) {
    return LedMode::OtaBlink;
  }
  if (!pressedSnapshot && jitterWifiTimeoutActive) {
    return LedMode::JitterBreathing;
  }
  if (!pressedSnapshot && jitterWindowState == JitterWindowState::Inactive) {
    return LedMode::Off;
  }
  if (!connected) {
    return LedMode::Alert;
  }
  if (pressedSnapshot) {
    return LedMode::InputReady;
  }
  if (jitterWindowState == JitterWindowState::Unknown) {
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
      if (otaLedTaskHandle == nullptr) {
        updateOtaBlinkLed();
      }
      break;
    case LedMode::InputReady:
      if (!inputTransferBlinkActive) {
        applySolidColor(0, 255, 0);
      }
      break;
    case LedMode::JitterBreathing:
      updateBreathingLed(BREATH_PERIOD_MOUSE_JITTER_MS);
      break;
    case LedMode::Off:
      applySolidColor(0, 0, 0);
      break;
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
  if (consoleTxMutex != nullptr) {
    xSemaphoreTake(consoleTxMutex, portMAX_DELAY);
  }
  int currentValue = buttonDown ? 1 : 0;
  if (!force && currentValue == lastConsoleButtonValue) {
    if (consoleTxMutex != nullptr) {
      xSemaphoreGive(consoleTxMutex);
    }
    return true;
  }
  size_t written = consoleClient.print(buttonDown ? "1\n" : "0\n");
  if (written == 0) {
    if (consoleTxMutex != nullptr) {
      xSemaphoreGive(consoleTxMutex);
    }
    return false;
  }
  lastConsoleButtonValue = currentValue;
  if (consoleTxMutex != nullptr) {
    xSemaphoreGive(consoleTxMutex);
  }
  Serial.printf("Console TX state: %d\n", currentValue);
  return true;
}

static bool shouldSendConsoleCountAck(uint32_t currentCount, uint32_t lastCount,
                                      uint32_t lastAckMs, bool force) {
  if (force) {
    return true;
  }
  uint32_t now = millis();
  return currentCount - lastCount >= CONSOLE_ACK_INTERVAL_CHARS ||
         now - lastAckMs >= CONSOLE_ACK_INTERVAL_MS;
}

static bool sendConsoleAcceptedAck(bool force) {
  if (!consoleClient || !consoleClient.connected()) {
    return false;
  }
  uint32_t currentCount = acceptedCharCount;
  if (!shouldSendConsoleCountAck(currentCount, lastAcceptedAckCount,
                                 lastAcceptedAckMs, force)) {
    return true;
  }
  if (consoleTxMutex != nullptr) {
    xSemaphoreTake(consoleTxMutex, portMAX_DELAY);
  }
  size_t written = consoleClient.printf("E %lu\n",
                                        static_cast<unsigned long>(currentCount));
  if (consoleTxMutex != nullptr) {
    xSemaphoreGive(consoleTxMutex);
  }
  if (written > 0) {
    lastAcceptedAckCount = currentCount;
    lastAcceptedAckMs = millis();
  }
  return written > 0;
}

static bool sendConsoleTypedAck(bool force) {
  if (!consoleClient || !consoleClient.connected()) {
    return false;
  }
  uint32_t currentCount = typedCharCount;
  if (!shouldSendConsoleCountAck(currentCount, lastTypedAckCount, lastTypedAckMs,
                                 force)) {
    return true;
  }
  if (consoleTxMutex != nullptr) {
    xSemaphoreTake(consoleTxMutex, portMAX_DELAY);
  }
  size_t written = consoleClient.printf("A %lu\n",
                                        static_cast<unsigned long>(currentCount));
  if (consoleTxMutex != nullptr) {
    xSemaphoreGive(consoleTxMutex);
  }
  if (written > 0) {
    lastTypedAckCount = currentCount;
    lastTypedAckMs = millis();
  }
  return written > 0;
}

static bool sendConsoleRateAck() {
  if (!consoleClient || !consoleClient.connected()) {
    return false;
  }
  if (consoleTxMutex != nullptr) {
    xSemaphoreTake(consoleTxMutex, portMAX_DELAY);
  }
  size_t written = consoleClient.printf(
      "R %u %lu\n", static_cast<unsigned>(keyboardRateCps),
      static_cast<unsigned long>(keyboardCharIntervalUs));
  if (consoleTxMutex != nullptr) {
    xSemaphoreGive(consoleTxMutex);
  }
  return written > 0;
}

static bool sendConsoleBufferFinishedAck() {
  if (!consoleClient || !consoleClient.connected()) {
    return false;
  }
  if (consoleTxMutex != nullptr) {
    xSemaphoreTake(consoleTxMutex, portMAX_DELAY);
  }
  size_t written = consoleClient.printf("F %lu\n",
                                        static_cast<unsigned long>(typedCharCount));
  if (consoleTxMutex != nullptr) {
    xSemaphoreGive(consoleTxMutex);
  }
  return written > 0;
}

static bool sendConsoleFileFinishedAck() {
  if (!consoleClient || !consoleClient.connected()) {
    return false;
  }
  if (consoleTxMutex != nullptr) {
    xSemaphoreTake(consoleTxMutex, portMAX_DELAY);
  }
  size_t written = consoleClient.printf(
      "D %lu %lu\n", static_cast<unsigned long>(typedCharCount),
      static_cast<unsigned long>(expectedFileTypedCount));
  if (consoleTxMutex != nullptr) {
    xSemaphoreGive(consoleTxMutex);
  }
  return written > 0;
}

static size_t getInputBufferFree() {
#if ENABLE_KEYBOARD
  if (inputBufferMutex == nullptr) {
    return 0;
  }
  xSemaphoreTake(inputBufferMutex, portMAX_DELAY);
  size_t freeBytes = INPUT_BUFFER_SIZE - inputBufferCount;
  xSemaphoreGive(inputBufferMutex);
  return freeBytes;
#else
  return 0;
#endif
}

static bool sendConsoleBufferState(bool force) {
  if (!consoleClient || !consoleClient.connected()) {
    return false;
  }
  size_t freeBytes = getInputBufferFree();
  int freeBytesInt = static_cast<int>(freeBytes);
  bool meaningfulChange =
      lastConsoleBufferFree < 0 ||
      abs(freeBytesInt - lastConsoleBufferFree) >= CONSOLE_BUFFER_REPORT_DELTA ||
      freeBytes == 0 || freeBytes == INPUT_BUFFER_SIZE;
  if (!force && !meaningfulChange) {
    return true;
  }
  if (consoleTxMutex != nullptr) {
    xSemaphoreTake(consoleTxMutex, portMAX_DELAY);
  }
  if (!force && freeBytesInt == lastConsoleBufferFree) {
    if (consoleTxMutex != nullptr) {
      xSemaphoreGive(consoleTxMutex);
    }
    return true;
  }
  size_t written = consoleClient.printf("B %u\n", static_cast<unsigned>(freeBytes));
  if (written == 0) {
    if (consoleTxMutex != nullptr) {
      xSemaphoreGive(consoleTxMutex);
    }
    return false;
  }
  lastConsoleBufferFree = freeBytesInt;
  if (consoleTxMutex != nullptr) {
    xSemaphoreGive(consoleTxMutex);
  }
  return true;
}

static void serviceConsoleStatusAcks() {
  if (!consoleClient || !consoleClient.connected()) {
    return;
  }

  sendConsoleAcceptedAck(false);
  sendConsoleTypedAck(false);
  sendConsoleBufferState(false);

#if ENABLE_KEYBOARD
  if (fileEndRequested && !fileFinishedAckSent &&
      getInputBufferFree() == INPUT_BUFFER_SIZE &&
      typedCharCount >= expectedFileTypedCount &&
      typedCharCount >= acceptedCharCount) {
    bool sent = sendConsoleAcceptedAck(true);
    sent = sendConsoleTypedAck(true) && sent;
    sent = sendConsoleBufferFinishedAck() && sent;
    sent = sendConsoleFileFinishedAck() && sent;
    if (sent) {
      fileFinishedAckSent = true;
    }
  }
#endif
}

static void closeConsoleClient(const char* reason) {
  if (!consoleClient) {
    return;
  }
  Serial.printf("Console client %s\n", reason);
  if (consoleTxMutex != nullptr) {
    xSemaphoreTake(consoleTxMutex, portMAX_DELAY);
  }
  consoleClient.stop();
  if (consoleTxMutex != nullptr) {
    xSemaphoreGive(consoleTxMutex);
  }
  lastConsoleButtonValue = -1;
  lastConsoleBufferFree = -1;
  lastAcceptedAckCount = 0;
  lastTypedAckCount = 0;
  lastAcceptedAckMs = 0;
  lastTypedAckMs = 0;
#if ENABLE_KEYBOARD
  resetInputBuffer();
#endif
  acceptedCharCount = 0;
  typedCharCount = 0;
  expectedFileTypedCount = 0;
  fileEndRequested = false;
  fileFinishedAckSent = false;
  controlFrameActive = false;
  controlFrameLength = 0;
}

static bool readButtonPressed() {
  return digitalRead(BUTTON_PIN) == LOW;
}

static bool parseUnsignedLong(const char* text, uint32_t* value) {
  if (text == nullptr || value == nullptr || *text == '\0') {
    return false;
  }

  uint32_t parsed = 0;
  while (*text != '\0') {
    if (*text < '0' || *text > '9') {
      return false;
    }
    parsed = (parsed * 10UL) + static_cast<uint32_t>(*text - '0');
    text++;
  }
  *value = parsed;
  return true;
}

static void setKeyboardRate(uint32_t requestedRateCps) {
  if (requestedRateCps < MIN_KEYBOARD_RATE_CPS) {
    requestedRateCps = MIN_KEYBOARD_RATE_CPS;
  } else if (requestedRateCps > MAX_KEYBOARD_RATE_CPS) {
    requestedRateCps = MAX_KEYBOARD_RATE_CPS;
  }

  keyboardRateCps = static_cast<uint16_t>(requestedRateCps);
  keyboardCharIntervalUs = max<uint32_t>(1, 1000000UL / requestedRateCps);
  Serial.printf("Keyboard typing rate set to %u cps (%lu us/char)\n",
                static_cast<unsigned>(keyboardRateCps),
                static_cast<unsigned long>(keyboardCharIntervalUs));
  sendConsoleRateAck();
}

static bool handleCompletedControlFrame() {
  controlFrameBuffer[controlFrameLength] = '\0';
  if (strncmp(controlFrameBuffer, "END ", 4) == 0) {
    uint32_t expected = 0;
    if (parseUnsignedLong(controlFrameBuffer + 4, &expected)) {
      expectedFileTypedCount = expected;
      fileEndRequested = true;
      fileFinishedAckSent = false;
      Serial.printf("Console control file end requested, expected typed=%lu\n",
                    static_cast<unsigned long>(expectedFileTypedCount));
      if (getInputBufferFree() == INPUT_BUFFER_SIZE) {
        serviceConsoleStatusAcks();
      }
      return true;
    }
  }
  if (strncmp(controlFrameBuffer, "RATE ", 5) == 0) {
    uint32_t requestedRate = 0;
    if (parseUnsignedLong(controlFrameBuffer + 5, &requestedRate)) {
      setKeyboardRate(requestedRate);
      return true;
    }
  }
#if ENABLE_KEYBOARD
  if (strcmp(controlFrameBuffer, "KEY LEFT") == 0) {
    return enqueueInputToken(INPUT_TOKEN_KEY_LEFT);
  }
  if (strcmp(controlFrameBuffer, "KEY RIGHT") == 0) {
    return enqueueInputToken(INPUT_TOKEN_KEY_RIGHT);
  }
  if (strcmp(controlFrameBuffer, "KEY SPACE") == 0) {
    return enqueueInputToken(INPUT_TOKEN_KEY_SPACE);
  }
#endif

  Serial.println("Ignoring invalid console control frame");
  return false;
}

static bool handleControlFrameChar(char c) {
  if (!controlFrameActive && c != CONTROL_FRAME_PREFIX) {
    return false;
  }

  if (!controlFrameActive) {
    controlFrameActive = true;
    controlFrameLength = 0;
    return true;
  }

  if (c == '\n') {
    handleCompletedControlFrame();
    controlFrameActive = false;
    controlFrameLength = 0;
    return true;
  }

  if (controlFrameLength < CONTROL_FRAME_MAX_LEN) {
    controlFrameBuffer[controlFrameLength++] = c;
  } else {
    Serial.println("Console control frame too long, dropping");
    controlFrameActive = false;
    controlFrameLength = 0;
  }
  return true;
}

static void notifyInputTransferLedTask() {
  if (inputTransferLedTaskHandle != nullptr) {
    xTaskNotifyGive(inputTransferLedTaskHandle);
  }
}

static bool timeBefore(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) < 0;
}

static void inputTransferLedTask(void* parameter) {
  (void)parameter;

  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    uint32_t activeUntil = millis() + INPUT_TRANSFER_ACTIVE_MS;
    bool ledOn = false;
    inputTransferBlinkActive = true;

    while (readButtonPressed() && timeBefore(millis(), activeUntil)) {
      ledOn = !ledOn;
      applySolidColor(0, ledOn ? 255 : 0, 0);

      uint32_t notified = ulTaskNotifyTake(
          pdTRUE, pdMS_TO_TICKS(INPUT_TRANSFER_BLINK_HALF_PERIOD_MS));
      if (notified > 0) {
        activeUntil = millis() + INPUT_TRANSFER_ACTIVE_MS;
      }
    }

    inputTransferBlinkActive = false;
    if (readButtonPressed() && WiFi.status() == WL_CONNECTED && !otaInProgress) {
      applySolidColor(0, 255, 0);
    }
  }
}

static void startInputTransferLedTask() {
  ledMutex = xSemaphoreCreateMutex();
  if (ledMutex == nullptr) {
    Serial.println("Failed to create LED mutex");
  }

  BaseType_t created = xTaskCreate(inputTransferLedTask, "input-transfer-led",
                                   2048, nullptr, 1,
                                   &inputTransferLedTaskHandle);
  if (created != pdPASS) {
    inputTransferLedTaskHandle = nullptr;
    Serial.println("Failed to create INPUT transfer LED task");
  }
}

#if ENABLE_KEYBOARD
static void resetInputBuffer() {
  if (inputBufferMutex == nullptr) {
    inputAbortGeneration++;
    return;
  }
  xSemaphoreTake(inputBufferMutex, portMAX_DELAY);
  inputBufferHead = 0;
  inputBufferTail = 0;
  inputBufferCount = 0;
  inputAbortGeneration++;
  xSemaphoreGive(inputBufferMutex);
  if (keyboardInputTaskHandle != nullptr) {
    xTaskNotifyGive(keyboardInputTaskHandle);
  }
}

static bool enqueueInputToken(int16_t token) {
  if (inputBufferMutex == nullptr) {
    return false;
  }
  bool queued = false;
  xSemaphoreTake(inputBufferMutex, portMAX_DELAY);
  if (inputBufferCount < INPUT_BUFFER_SIZE) {
    inputBuffer[inputBufferHead] = token;
    inputBufferHead = (inputBufferHead + 1) % INPUT_BUFFER_SIZE;
    inputBufferCount++;
    queued = true;
  }
  xSemaphoreGive(inputBufferMutex);
  if (queued && keyboardInputTaskHandle != nullptr) {
    xTaskNotifyGive(keyboardInputTaskHandle);
  }
  return queued;
}

static bool dequeueInputChar(int16_t* c) {
  if (inputBufferMutex == nullptr || c == nullptr) {
    return false;
  }
  bool hasChar = false;
  xSemaphoreTake(inputBufferMutex, portMAX_DELAY);
  if (inputBufferCount > 0) {
    *c = inputBuffer[inputBufferTail];
    inputBufferTail = (inputBufferTail + 1) % INPUT_BUFFER_SIZE;
    inputBufferCount--;
    hasChar = true;
  }
  xSemaphoreGive(inputBufferMutex);
  return hasChar;
}
#endif

static bool updateDebouncedButtonState() {
  bool rawPressed = readButtonPressed();
  uint32_t now = millis();
  if (rawPressed != lastRawButtonPressed) {
    lastRawButtonPressed = rawPressed;
    lastButtonRawChangeMs = now;
    Serial.printf("Button raw state: %s\n", rawPressed ? "LOW/PRESSED" : "HIGH/RELEASED");
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
    WiFiClient candidate = consoleServer.accept();
    if (candidate) {
      candidate.setNoDelay(true);
      consoleClient = candidate;
      Serial.printf("Console client connected from %s:%u\n",
                    consoleClient.remoteIP().toString().c_str(),
                    consoleClient.remotePort());
      acceptedCharCount = 0;
      typedCharCount = 0;
      expectedFileTypedCount = 0;
      fileEndRequested = false;
      fileFinishedAckSent = false;
      controlFrameActive = false;
      controlFrameLength = 0;
#if ENABLE_KEYBOARD
      resetInputBuffer();
#endif
      lastConsoleButtonValue = -1;
      lastConsoleBufferFree = -1;
      lastAcceptedAckCount = 0;
      lastTypedAckCount = 0;
      lastAcceptedAckMs = 0;
      lastTypedAckMs = 0;
      if (!sendConsoleButtonState(buttonDown, true)) {
        Serial.println("Console state send pending after connect");
      }
      sendConsoleAcceptedAck(true);
      sendConsoleTypedAck(true);
      sendConsoleRateAck();
      sendConsoleBufferState(true);
    }
    return;
  }

  while (consoleClient.available()) {
    int nextByte = consoleClient.peek();
    if (nextByte < 0) {
      break;
    }
    if (controlFrameActive ||
        static_cast<char>(nextByte) == CONTROL_FRAME_PREFIX) {
      char controlChar = static_cast<char>(consoleClient.read());
      handleControlFrameChar(controlChar);
      continue;
    }
    if (!buttonDown || !keyboardTypingActive) {
      break;
    }
#if ENABLE_KEYBOARD
    if (getInputBufferFree() == 0) {
      sendConsoleBufferState(true);
      break;
    }
#endif
    char raw = static_cast<char>(consoleClient.read());
    char c = raw == '\r' ? '\n' : raw;
#if ENABLE_KEYBOARD
    if (enqueueInputToken(static_cast<unsigned char>(c))) {
      acceptedCharCount++;
      sendConsoleAcceptedAck(false);
      sendConsoleBufferState(false);
    } else {
      sendConsoleBufferState(true);
      break;
    }
#endif
  }

  if (!consoleClient.connected()) {
    closeConsoleClient("disconnected");
    return;
  }

  if (!sendConsoleButtonState(buttonDown, false) && !consoleClient.connected()) {
    closeConsoleClient("disconnected during state send");
    return;
  }
  serviceConsoleStatusAcks();
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
    notifyOtaLedTask();
    Serial.printf("OTA start (%s)\n",
                  ArduinoOTA.getCommand() == U_FLASH ? "sketch" : "filesystem");
  });
  ArduinoOTA.onEnd([]() {
    otaInProgress = false;
    notifyOtaLedTask();
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
    notifyOtaLedTask();
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
static bool keyboardInputCancelled(uint32_t abortGeneration) {
  return !keyboardTypingActive || !buttonPressed ||
         abortGeneration != inputAbortGeneration;
}

static size_t writeKeyboardToken(int16_t token, uint32_t abortGeneration) {
  if (keyboardInputCancelled(abortGeneration)) {
    keyboard.releaseAll();
    return 0;
  }

  keyboard.releaseAll();
  taskDelayMs(1);

  if (token == INPUT_TOKEN_KEY_LEFT) {
    keyboard.press(KEY_LEFT_ARROW);
    taskDelayMs(SHIFTED_SYMBOL_DELAY_MS);
    keyboard.releaseAll();
    taskDelayMs(1);
    return 1;
  }
  if (token == INPUT_TOKEN_KEY_RIGHT) {
    keyboard.press(KEY_RIGHT_ARROW);
    taskDelayMs(SHIFTED_SYMBOL_DELAY_MS);
    keyboard.releaseAll();
    taskDelayMs(1);
    return 1;
  }
  if (token == INPUT_TOKEN_KEY_SPACE) {
    size_t written = keyboard.write(' ');
    taskDelayMs(1);
    keyboard.releaseAll();
    taskDelayMs(1);
    return written;
  }

  char c = static_cast<char>(token & 0xFF);
  if (c == '#') {
    keyboard.press(KEY_LEFT_SHIFT);
    taskDelayMs(SHIFTED_SYMBOL_DELAY_MS);
    if (keyboardInputCancelled(abortGeneration)) {
      keyboard.releaseAll();
      return 0;
    }
    size_t written = keyboard.write('3');
    taskDelayMs(SHIFTED_SYMBOL_DELAY_MS);
    keyboard.releaseAll();
    taskDelayMs(1);
    return written;
  }

  size_t written = keyboard.write(static_cast<uint8_t>(c));
  taskDelayMs(1);
  keyboard.releaseAll();
  taskDelayMs(1);
  return written;
}

static void keyboardInputTask(void* parameter) {
  (void)parameter;

  for (;;) {
    int16_t c = 0;
    uint32_t abortGeneration = inputAbortGeneration;
    if (!keyboardTypingActive || !buttonPressed || !dequeueInputChar(&c)) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
      continue;
    }

    uint32_t nowUs = micros();
    uint32_t elapsedUs = nowUs - lastKeyboardSendUs;
    uint32_t intervalUs = keyboardCharIntervalUs;
    if (elapsedUs < intervalUs) {
      uint32_t remainingUs = intervalUs - elapsedUs;
      if (remainingUs >= 1000) {
        taskDelayMs(remainingUs / 1000);
      }
      remainingUs = intervalUs - (micros() - lastKeyboardSendUs);
      if (remainingUs > 0 && remainingUs < 1000) {
        delayMicroseconds(remainingUs);
      }
    }

    if (!consoleClient || !consoleClient.connected() ||
        keyboardInputCancelled(abortGeneration)) {
      continue;
    }

    if (writeKeyboardToken(c, abortGeneration) > 0) {
      typedCharCount++;
      notifyInputTransferLedTask();
    }
    lastKeyboardSendUs = micros();
  }
}

static void startKeyboardInputTask() {
  inputBufferMutex = xSemaphoreCreateMutex();
  if (inputBufferMutex == nullptr) {
    Serial.println("Failed to create input buffer mutex");
    return;
  }

  BaseType_t created = xTaskCreatePinnedToCore(
      keyboardInputTask, "keyboard-input", 4096, nullptr,
      KEYBOARD_TASK_PRIORITY, &keyboardInputTaskHandle, KEYBOARD_TASK_CORE);
  if (created != pdPASS) {
    keyboardInputTaskHandle = nullptr;
    Serial.println("Failed to create keyboard input task");
  } else {
    Serial.printf("Keyboard input task pinned to core %d\n",
                  static_cast<int>(KEYBOARD_TASK_CORE));
  }
}

static void updateKeyboardTypingState(bool enable) {
  if (enable == keyboardTypingActive) {
    return;
  }
  keyboardTypingActive = enable;
  if (keyboardTypingActive) {
    Serial.println("Keyboard typing ENABLED");
  } else {
    keyboard.releaseAll();
    resetInputBuffer();
    if (keyboardInputTaskHandle != nullptr) {
      xTaskNotifyGive(keyboardInputTaskHandle);
    }
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
  consoleTxMutex = xSemaphoreCreateMutex();
  if (consoleTxMutex == nullptr) {
    Serial.println("Failed to create console TX mutex");
  }
  pixel.begin();
  pixel.setBrightness(PIXEL_BRIGHTNESS);
  pixel.clear();
  pixel.show();
  startOtaLedTask();
  mouse.begin();
#if ENABLE_KEYBOARD
  keyboard.begin();
  startKeyboardInputTask();
#endif
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  bool initialPressed = readButtonPressed();
  buttonPressed = initialPressed;
  lastRawButtonPressed = initialPressed;
  lastButtonRawChangeMs = millis();
  startInputTransferLedTask();
#if ENABLE_KEYBOARD
  updateKeyboardTypingState(initialPressed);
#endif
  loadWifiList();
  restoreWifiStationMode();
  ensureTimeConfigured();
  startWifiScan();
  jitterWifiDisconnectedSince = millis();
  ensureOtaConfigured();

  consoleServer.begin();
  Serial.printf("TCP console listening on port %u\n", TCP_CONSOLE_PORT);
  printChipInfo();
  beginOtaIfConnected();
}

void loop() {
  maintainWifi();
  bool stateChanged = updateDebouncedButtonState();
  bool pressedSnapshot = readButtonPressed();
  if (pressedSnapshot != buttonPressed &&
      millis() - lastButtonRawChangeMs >= BUTTON_DEBOUNCE_MS) {
    buttonPressed = pressedSnapshot;
    stateChanged = true;
  }
  bool inputPressed = buttonPressed;
#if ENABLE_KEYBOARD
  updateKeyboardTypingState(inputPressed);
#endif
  bool connected = WiFi.status() == WL_CONNECTED;
  uint32_t nowMs = millis();
  if (inputPressed || connected || stateChanged) {
    jitterWifiDisconnectedSince = nowMs;
    jitterWifiTimeoutActive = false;
  } else if (!jitterWifiTimeoutActive &&
             nowMs - jitterWifiDisconnectedSince >= JITTER_WIFI_TIMEOUT_MS) {
    jitterWifiTimeoutActive = true;
    // Send the first fallback jiggle immediately, then use the normal interval.
    lastMouseWake = nowMs - MOUSE_WAKE_INTERVAL_MS;
    Serial.println("Wi-Fi unavailable for 2 minutes; jitter enabled regardless of time");
  }
  JitterWindowState jitterWindowState = getJitterWindowState();
  bool mouseJitterWindowActive = jitterWifiTimeoutActive ||
                                jitterWindowState == JitterWindowState::Active;
  bool mouseJitterPowerSave =
      !inputPressed && !jitterWifiTimeoutActive &&
      jitterWindowState == JitterWindowState::Inactive &&
      !otaInProgress;
  setPowerSaveMode(mouseJitterPowerSave);
  LedMode ledMode = determineLedMode(connected, inputPressed, jitterWindowState);
  if (stateChanged && !inputPressed && ledMode == LedMode::Off) {
    applySolidColor(0, 0, 0);
  }
  if (!inputPressed && mouseJitterWindowActive &&
      nowMs - lastMouseWake >= MOUSE_WAKE_INTERVAL_MS) {
    performMouseWakeJiggle();
    lastMouseWake = nowMs;
  }
  if (!inputPressed && consoleClient && consoleClient.connected()) {
    sendConsoleButtonState(false, true);
    closeConsoleClient("terminated (button released)");
  }
  if (!mouseJitterPowerSave) {
    handleTcpConsoleInput(inputPressed);
  }

  if (connected && !wasConnected) {
    printIpAddress();
    logIpAndMac("Wi-Fi connected,");
  } else if (!connected && wasConnected) {
    Serial.println("Wi-Fi disconnected, entering alert mode.");
    lastIpLog = 0;
    lastAlertToggle = 0;
  }
  wasConnected = connected;

  if (connected) {
    ensureTimeConfigured();
    beginOtaIfConnected();
    ledMode = determineLedMode(connected, pressedSnapshot, jitterWindowState);
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
    Serial.printf("Button state changed: %s (raw=%s)\n",
                  buttonPressed ? "PRESSED" : "RELEASED",
                  pressedSnapshot ? "LOW" : "HIGH");
  }
  if (otaActive) {
    ArduinoOTA.handle();
  }
  uint32_t loopDelay = mouseJitterPowerSave
                           ? LOOP_DELAY_POWERSAVE_MS
                           : (wasConnected ? LOOP_DELAY_CONNECTED_MS
                                           : LOOP_DELAY_DISCONNECTED_MS);
  taskDelayMs(loopDelay);
}
