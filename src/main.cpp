#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include "mbedtls/base64.h"

#include "camera.h"
#include "secrets.h"
#include "web.h"

static constexpr unsigned long WIFI_TIMEOUT_MS = 20000;

// WiFi.status() only ever says "not connected". The disconnect event carries the
// reason, which is the difference between a wrong password and an AP that was
// never found, and that distinction is most of the debugging.
static void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info) {
  const uint8_t reason = info.wifi_sta_disconnected.reason;
  const char *meaning = "see esp_wifi_types.h";
  switch (reason) {
    case 2:   meaning = "auth expired"; break;
    case 8:   meaning = "deauthenticated by AP"; break;
    case 15:  meaning = "4-way handshake timeout: wrong password"; break;
    case 201: meaning = "no AP found: SSID mismatch, or 5GHz only"; break;
    case 202: meaning = "auth failed: wrong password"; break;
    case 203: meaning = "assoc failed: AP refused, check MAC filtering"; break;
    case 204: meaning = "handshake timeout"; break;
  }
  Serial.printf("\nwifi disconnect reason %u: %s\n", reason, meaning);
}

// Routers are allowed to broadcast an SSID with leading or trailing spaces, and
// nothing in a config file or a scan listing shows them. Match on the trimmed
// name, then connect with whatever exact string the radio actually reported.
static int findBestAp(int found) {
  String want = WIFI_SSID;
  want.trim();

  int best = -1;
  int bestRssi = -128;
  for (int i = 0; i < found; i++) {
    String seen = WiFi.SSID(i);
    seen.trim();
    if (seen == want && WiFi.RSSI(i) > bestRssi) {
      bestRssi = WiFi.RSSI(i);
      best = i;
    }
  }
  return best;
}

static bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.onEvent(onWifiDisconnect, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  // This AP advertises WPA and WPA2 together. Recent IDF releases refuse the
  // weaker half by default, so lower the floor rather than fail on a mixed router.
  WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);

  // Modem sleep saves power but adds hundreds of milliseconds of latency and
  // stalls an MJPEG stream badly. Trade it back for responsiveness.
  WiFi.setSleep(false);

  Serial.printf("looking for [%s]\n", WIFI_SSID);
  const int found = WiFi.scanNetworks();
  for (int i = 0; i < found; i++) {
    Serial.printf("  [%s] ch%-3d %4d dBm\n", WiFi.SSID(i).c_str(),
                  WiFi.channel(i), WiFi.RSSI(i));
  }

  const int best = findBestAp(found);
  if (best < 0) {
    Serial.println("no AP matched. Check WIFI_SSID, and that the network is 2.4GHz");
    WiFi.scanDelete();
    return false;
  }

  const String exactSsid = WiFi.SSID(best);
  if (exactSsid != WIFI_SSID) {
    Serial.printf("note: AP broadcasts [%s], config says [%s]. Using the AP's.\n",
                  exactSsid.c_str(), WIFI_SSID);
  }
  Serial.printf("connecting to [%s] ch%d %d dBm", exactSsid.c_str(),
                WiFi.channel(best), WiFi.RSSI(best));
  WiFi.scanDelete();

  WiFi.begin(exactSsid.c_str(), WIFI_PASS);
  const unsigned long deadline = millis() + WIFI_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("wifi failed (status %d)\n", WiFi.status());
    return false;
  }
  Serial.printf("wifi up, %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

// Chunk size is divisible by 3 so only the final chunk carries base64 padding,
// which keeps the stream concatenable on the receiving end.
static void dumpBase64(const camera_fb_t *fb) {
  constexpr size_t CHUNK = 240;
  unsigned char out[CHUNK / 3 * 4 + 1];

  Serial.printf("---BEGIN JPEG %u---\n", fb->len);
  for (size_t off = 0; off < fb->len; off += CHUNK) {
    const size_t remaining = fb->len - off;
    const size_t n = remaining < CHUNK ? remaining : CHUNK;
    size_t written = 0;
    mbedtls_base64_encode(out, sizeof(out), &written, fb->buf + off, n);
    out[written] = '\0';
    Serial.println(reinterpret_cast<char *>(out));
  }
  Serial.println("---END JPEG---");
}

void setup() {
  Serial.begin(115200);
  delay(2000);  // USB-TTL adapters routinely swallow the first output
  Serial.println();

  Serial.printf("PSRAM: %s (%u bytes free)\n",
                psramFound() ? "found" : "MISSING", ESP.getFreePsram());

  if (!cameraInit()) return;
  if (!connectWifi()) return;

  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("also at http://%s.local\n", MDNS_HOSTNAME);
  }

  if (!startWebServers()) return;
  Serial.printf("ready. open http://%s\n", WiFi.localIP().toString().c_str());
  Serial.println("send 'p' for a base64 frame over serial");
}

void loop() {
  // Serial capture is kept as a fallback for when the network is the thing that
  // is broken, which is exactly when the browser view is no help.
  bool wantDump = false;
  while (Serial.available()) {
    if (Serial.read() == 'p') wantDump = true;
  }
  if (wantDump) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      dumpBase64(fb);
      esp_camera_fb_return(fb);
    }
  }

  static unsigned long lastReport = 0;
  if (millis() - lastReport > 15000) {
    lastReport = millis();
    Serial.printf("up %lus  heap %u  psram %u  wifi %s\n", millis() / 1000,
                  ESP.getFreeHeap(), ESP.getFreePsram(),
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "DOWN");
  }
  delay(50);
}
