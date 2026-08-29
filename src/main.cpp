#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include "mbedtls/base64.h"

#include "camera.h"
#include "secrets.h"
#include "web.h"

// Retry cadence after a drop. Backing off keeps a long outage from hammering the
// radio, and the reboot is a last resort for the states a reconnect cannot clear.
static constexpr unsigned long RETRY_MIN_MS = 5000;
static constexpr unsigned long RETRY_MAX_MS = 60000;
static constexpr unsigned long REBOOT_AFTER_OFFLINE_MS = 10UL * 60UL * 1000UL;

// The exact SSID as broadcast, which may carry padding the config file does not.
// Resolved once by scanning, then reused so reconnects skip the scan.
static String apSsid;

static bool serversStarted = false;
static bool everConnected = false;
static unsigned long offlineSince = 0;
static unsigned long lastAttempt = 0;
static unsigned long retryDelay = RETRY_MIN_MS;
static uint32_t reconnectCount = 0;

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
  Serial.printf("wifi down, reason %u: %s\n", reason, meaning);
}

// Routers are allowed to broadcast an SSID with leading or trailing spaces, and
// nothing in a config file or a scan listing shows them. Match on the trimmed
// name, then keep whatever exact string the radio actually reported.
static bool resolveSsid() {
  Serial.printf("scanning for [%s]\n", WIFI_SSID);
  const int found = WiFi.scanNetworks();

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

  if (best < 0) {
    Serial.println("  no matching AP. Check WIFI_SSID, and that the network is 2.4GHz");
    WiFi.scanDelete();
    return false;
  }

  apSsid = WiFi.SSID(best);
  if (apSsid != WIFI_SSID) {
    Serial.printf("  AP broadcasts [%s], config says [%s]. Using the AP's.\n",
                  apSsid.c_str(), WIFI_SSID);
  }
  Serial.printf("  found on ch%d at %d dBm\n", WiFi.channel(best), bestRssi);
  WiFi.scanDelete();
  return true;
}

// Non-blocking. Called every pass, does nothing while the link is healthy, and
// drives reconnection when it is not. Nothing here waits, so the camera keeps
// serving frames to anyone still connected while the radio sorts itself out.
static void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!everConnected) {
      everConnected = true;
      Serial.printf("wifi up, %s, RSSI %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (MDNS.begin(MDNS_HOSTNAME)) MDNS.addService("http", "tcp", 80);
    } else if (offlineSince != 0) {
      // The first connect is not a reconnect. Counting it would put a permanent
      // off-by-one into the only number that says whether the link is healthy.
      reconnectCount++;
      Serial.printf("wifi back after %lus, %s (reconnect #%u)\n",
                    (millis() - offlineSince) / 1000,
                    WiFi.localIP().toString().c_str(), reconnectCount);
      // mDNS loses its registration across a link drop and has to be restarted.
      MDNS.end();
      if (MDNS.begin(MDNS_HOSTNAME)) MDNS.addService("http", "tcp", 80);
    }
    offlineSince = 0;
    retryDelay = RETRY_MIN_MS;

    if (!serversStarted && startWebServers()) {
      serversStarted = true;
      Serial.printf("ready. open http://%s or http://%s.local\n",
                    WiFi.localIP().toString().c_str(), MDNS_HOSTNAME);
    }
    return;
  }

  if (offlineSince == 0) offlineSince = millis();

  // A radio that has been unreachable this long is usually in a state a fresh
  // boot clears and a reconnect does not. Cheaper than staying dark until
  // someone notices and pulls the power.
  if (millis() - offlineSince > REBOOT_AFTER_OFFLINE_MS) {
    Serial.println("offline too long, restarting");
    Serial.flush();
    ESP.restart();
  }

  if (millis() - lastAttempt < retryDelay) return;
  lastAttempt = millis();

  if (apSsid.isEmpty() && !resolveSsid()) {
    retryDelay = min(retryDelay * 2, RETRY_MAX_MS);
    return;
  }

  Serial.printf("connecting to [%s]...\n", apSsid.c_str());
  WiFi.disconnect();
  WiFi.begin(apSsid.c_str(), WIFI_PASS);
  retryDelay = min(retryDelay * 2, RETRY_MAX_MS);
}

static void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.onEvent(onWifiDisconnect, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.setAutoReconnect(true);

  // Credentials live in the firmware, so there is nothing to gain from writing
  // them to NVS on every boot and flash wear to lose.
  WiFi.persistent(false);

  // This AP advertises WPA and WPA2 together. Recent IDF releases refuse the
  // weaker half by default, so lower the floor rather than fail on a mixed router.
  WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);

  // Modem sleep saves power but adds hundreds of milliseconds of latency and
  // stalls an MJPEG stream badly. Trade it back for responsiveness.
  WiFi.setSleep(false);
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

  // The network is brought up but not waited for. ensureWifi() takes it from
  // here, so a router that is down at boot delays the camera rather than
  // ending the run, which is the same path a mid-flight drop takes.
  startWifi();
  Serial.println("send 'p' for a frame, 'd' to force a wifi drop");
}

void loop() {
  // Serial capture is kept as a fallback for when the network is the thing that
  // is broken, which is exactly when the browser view is no help.
  bool wantDump = false;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == 'p') wantDump = true;
    // Reconnect logic that has never been exercised is a guess. This makes the
    // drop reproducible without power-cycling the router.
    if (c == 'd') {
      Serial.println("forcing disconnect");
      WiFi.disconnect(false, false);
    }
  }
  if (wantDump) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      dumpBase64(fb);
      esp_camera_fb_return(fb);
    }
  }

  ensureWifi();

  static unsigned long lastReport = 0;
  if (millis() - lastReport > 15000) {
    lastReport = millis();
    Serial.printf("up %lus  heap %u  psram %u  reconnects %u  wifi %s\n",
                  millis() / 1000, ESP.getFreeHeap(), ESP.getFreePsram(), reconnectCount,
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "DOWN");
  }
  delay(50);
}
