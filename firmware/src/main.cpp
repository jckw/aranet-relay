// Aranet4 -> Cloudflare bridge.
//
// Listens passively for the Aranet4's Bluetooth broadcasts (needs "Smart Home integrations"
// on in the Aranet app; no pairing) and posts each new measurement to the Worker's /ingest.
// Also posts a heartbeat every minute so the app can tell the bridge is alive.

#include <Arduino.h>
#include <HTTPClient.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "root_cas.h"
#include "secrets.h"

static const uint16_t ARANET_COMPANY_ID = 0x0702;  // SAF Tehnika
static const size_t ARANET4_PAYLOAD_LEN = 22;       // bytes after the company ID
static const uint32_t HEARTBEAT_MS = 60 * 1000;
static const uint32_t RETRY_MS = 10 * 1000;
static const uint32_t SENSOR_LOST_MS = 5 * 60 * 1000;

// Status light: the Adafruit Feather ESP32 V2's NeoPixel (data GPIO0, power GPIO2).
// ok: green blip every 3 s. sent: blue double flash after each post. problem: red fast blinking.
static const int NEOPIXEL_PIN = 0;
static const int NEOPIXEL_POWER_PIN = 2;
static const uint8_t LED_LEVEL = 24;  // out of 255; dim enough for a bedroom
enum class LedState { problem, ok };
static volatile LedState ledState = LedState::problem;
static volatile bool ledSentFlash = false;

static void ledTask(void*) {
  pinMode(NEOPIXEL_POWER_PIN, OUTPUT);
  digitalWrite(NEOPIXEL_POWER_PIN, HIGH);
  auto pulse = [](uint8_t r, uint8_t g, uint8_t b, uint32_t onMs, uint32_t offMs) {
    neopixelWrite(NEOPIXEL_PIN, r, g, b);
    vTaskDelay(pdMS_TO_TICKS(onMs));
    neopixelWrite(NEOPIXEL_PIN, 0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(offMs));
  };
  for (;;) {
    if (ledSentFlash) {
      ledSentFlash = false;
      pulse(0, 0, LED_LEVEL, 100, 120);
      pulse(0, 0, LED_LEVEL, 100, 600);
    } else if (ledState == LedState::ok) {
      pulse(0, LED_LEVEL, 0, 60, 2940);
    } else {
      pulse(LED_LEVEL, 0, 0, 150, 150);
    }
  }
}

struct Reading {
  uint16_t co2;
  float temperature;
  float pressure;
  uint8_t humidity;
  uint8_t battery;
  uint16_t interval;
  uint16_t ago;       // seconds since the sensor took this measurement, at receipt
  int rssi;
  uint32_t receivedMs;
};

static portMUX_TYPE readingMux = portMUX_INITIALIZER_UNLOCKED;
static Reading latest;
static bool haveReading = false;

// Payload layout (little-endian), from the aranet4 Python library and checked against this sensor:
//   [0] flags (bit 5 = integrations on) ... [8..9] CO2 ppm, [10..11] temp /20 °C,
//   [12..13] pressure /10 hPa, [14] humidity %, [15] battery %, [16] status colour,
//   [17..18] interval s, [19..20] seconds since measurement
class AranetScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (!dev->haveManufacturerData()) return;
    std::string m = dev->getManufacturerData();
    if (m.size() != 2 + ARANET4_PAYLOAD_LEN) return;
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(m.data());
    if ((raw[0] | (raw[1] << 8)) != ARANET_COMPANY_ID) return;
    const uint8_t* p = raw + 2;
    if (!(p[0] & (1 << 5))) return;  // integrations off: no measurements in the broadcast

    auto u16 = [p](int o) { return static_cast<uint16_t>(p[o] | (p[o + 1] << 8)); };
    Reading r;
    r.co2 = u16(8);
    r.temperature = u16(10) / 20.0f;
    r.pressure = u16(12) / 10.0f;
    r.humidity = p[14];
    r.battery = p[15];
    r.interval = u16(17);
    r.ago = u16(19);
    r.rssi = dev->getRSSI();
    r.receivedMs = millis();

    portENTER_CRITICAL(&readingMux);
    latest = r;
    haveReading = true;
    portEXIT_CRITICAL(&readingMux);
  }
};

static AranetScanCallbacks scanCallbacks;

static void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Wi-Fi: connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWi-Fi: connected, IP %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWi-Fi: failed. Check WIFI_SSID / WIFI_PASSWORD in secrets.h (2.4 GHz networks only).");
    // Show what this board can see, so a missing 2.4 GHz band is obvious.
    int n = WiFi.scanNetworks();
    bool seen = false;
    Serial.printf("Wi-Fi: %d networks visible on 2.4 GHz:\n", n);
    for (int i = 0; i < n; i++) {
      bool match = WiFi.SSID(i) == WIFI_SSID;
      seen |= match;
      Serial.printf("  %s %-32s ch %2d  %d dBm\n", match ? "*" : " ", WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
    }
    Serial.println(seen ? "Wi-Fi: your network is on 2.4 GHz, so the password is probably wrong."
                        : "Wi-Fi: your network isn't on 2.4 GHz here. Turn on the 2.4 GHz band in the router settings.");
    WiFi.scanDelete();
  }
}

// Certificate checks need the real date.
static void syncClock() {
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  Serial.print("Clock: syncing");
  for (int i = 0; i < 40 && time(nullptr) < 1700000000; i++) {
    delay(500);
    Serial.print('.');
  }
  Serial.println(time(nullptr) >= 1700000000 ? " done" : " failed (will retry)");
}

static bool post(const Reading& r) {
  WiFiClientSecure tls;
  tls.setCACert(ROOT_CAS);
  HTTPClient http;
  if (!http.begin(tls, String(ARANET_URL) + "/ingest")) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + INGEST_TOKEN);
  http.setUserAgent("aranet-bridge-esp32");

  uint32_t ago = r.ago + (millis() - r.receivedMs) / 1000;
  char body[200];
  snprintf(body, sizeof body,
           "{\"co2\":%u,\"temperature\":%.2f,\"pressure\":%.1f,\"humidity\":%u,\"battery\":%u,"
           "\"interval\":%u,\"ago\":%lu,\"rssi\":%d}",
           r.co2, r.temperature, r.pressure, r.humidity, r.battery, r.interval, (unsigned long)ago, r.rssi);
  int code = http.POST(body);
  String resp = http.getString();
  http.end();
  Serial.printf("POST %s -> %d %s\n", body, code, resp.c_str());
  return code == 200;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\nAranet4 bridge starting");
  xTaskCreate(ledTask, "led", 2048, nullptr, 1, nullptr);
  if (String(WIFI_SSID) == "your-network") {
    Serial.println("Set WIFI_SSID and WIFI_PASSWORD in include/secrets.h, then flash again.");
  }

  connectWifi();
  syncClock();

  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, true);  // want every repeat broadcast
  scan->setActiveScan(false);                    // listen only
  scan->setInterval(100);
  scan->setWindow(50);                           // leave the radio half free for Wi-Fi
  scan->setMaxResults(0);                        // don't keep results in memory
  scan->start(0, false, true);                   // scan forever
  Serial.println("Bluetooth: listening for Aranet4");
}

void loop() {
  static uint32_t lastPostMs = 0, lastAttemptMs = 0, lastMeasuredMs = 0, lastWarnMs = 0;
  static bool everPosted = false;
  static uint32_t lastFailMs = 0;

  connectWifi();
  if (time(nullptr) < 1700000000) syncClock();

  Reading r;
  bool have;
  portENTER_CRITICAL(&readingMux);
  r = latest;
  have = haveReading;
  portEXIT_CRITICAL(&readingMux);

  uint32_t now = millis();
  bool healthy = WiFi.status() == WL_CONNECTED && have && now - r.receivedMs <= SENSOR_LOST_MS &&
                 everPosted && (lastFailMs == 0 || lastPostMs > lastFailMs);
  ledState = healthy ? LedState::ok : LedState::problem;

  if (!have || now - r.receivedMs > SENSOR_LOST_MS) {
    if (now - lastWarnMs > 30000) {
      Serial.println(have ? "Bluetooth: no broadcast from the Aranet4 for 5 minutes. Is it in range?"
                          : "Bluetooth: waiting for the first Aranet4 broadcast...");
      lastWarnMs = now;
    }
    delay(500);
    return;
  }

  // A new measurement is one taken more than 30 s from the last one posted.
  uint32_t measuredMs = r.receivedMs - r.ago * 1000UL;
  bool fresh = !everPosted || (int32_t)(measuredMs - lastMeasuredMs) > 30000;
  bool heartbeat = now - lastPostMs > HEARTBEAT_MS;

  if ((fresh || heartbeat) && now - lastAttemptMs > RETRY_MS && WiFi.status() == WL_CONNECTED) {
    lastAttemptMs = now;
    if (post(r)) {
      lastPostMs = now;
      lastMeasuredMs = measuredMs;
      everPosted = true;
      ledSentFlash = true;
    } else {
      lastFailMs = now;
    }
  }
  delay(500);
}
