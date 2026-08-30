#include <Arduino.h>
#include <ArduinoOTA.h>
#include "esp_ota_ops.h"
#include <ESPmDNS.h>
#include <WiFi.h>
#include "mbedtls/base64.h"

#include "camera.h"
#include "clock.h"
#include "motion.h"
#include "config.h"
#include "portal.h"
#include "recording.h"
#include "statusled.h"
#include "storage.h"
#include "secrets.h"
#include "web.h"

// Retry cadence after a drop. Backing off keeps a long outage from hammering the
// radio, and the reboot is a last resort for the states a reconnect cannot clear.
// Three presses of the reset button wipes stored settings and returns the camera
// to setup mode, reachable through a pinhole in the housing.
//
// The gesture that actually works on this hardware is press and hold for about a
// second, release, pause a second, three times. A light tap does not hold the
// enable line low long enough to reset the chip, so the press never becomes a
// boot and the counter never sees it. Found the hard way.
// The window is short on purpose: a power supply that drops out repeatedly looks
// identical to someone at the pinhole, and five seconds is hard to hit by
// accident while still being comfortable to do deliberately. Brownouts are
// excluded from the count for the same reason.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif

static constexpr int RESET_PRESSES = 3;
static constexpr unsigned long RESET_WINDOW_MS = 5000;

extern "C" bool verifyRollbackLater() { return true; }

static Config cfg;
static bool portalMode = false;
static bool cameraReady = false;
static TrialState trial;
static bool onTrial = false;

static bool otaInProgress = false;

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
  Serial.printf("scanning for [%s]\n", cfg.wifiSsid.c_str());
  const int found = WiFi.scanNetworks();

  String want = cfg.wifiSsid;
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
  if (apSsid != cfg.wifiSsid) {
    Serial.printf("  AP broadcasts [%s], stored config says [%s]. Using the AP's.\n",
                  apSsid.c_str(), cfg.wifiSsid.c_str());
  }
  Serial.printf("  found on ch%d at %d dBm\n", WiFi.channel(best), bestRssi);
  WiFi.scanDelete();
  return true;
}


// Started only once the link is up, because ArduinoOTA binds to the network
// interface. The camera and both servers are torn down before the write begins:
// an OTA flash competing with a live MJPEG stream for heap is how a half-written
// image happens.
static void startOta() {
  ArduinoOTA.setHostname(cfg.cameraName.c_str());

  // We register mDNS ourselves in ensureWifi(), including the http service that
  // ArduinoOTA's own begin() would not restore. Leaving it enabled here makes
  // every service restart drop the camera's advertisement.
  ArduinoOTA.setMdnsEnabled(false);

  // Prefer the admin password so there is only one to remember. Configs written
  // before this existed fall back to the generated one rather than silently
  // leaving OTA open.
  if (!cfg.otaMd5.isEmpty()) {
    ArduinoOTA.setPasswordHash(cfg.otaMd5.c_str());
    Serial.println("OTA uses the admin password");
  } else if (!cfg.otaPassword.isEmpty()) {
    ArduinoOTA.setPassword(cfg.otaPassword.c_str());
    Serial.println("OTA uses the generated password, see the status page");
  } else {
    Serial.println("WARNING: OTA has no password, anyone on this network can reflash");
  }

  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    Serial.println("\nOTA starting, asking any stream to release the camera");
    webBeginUpdate();
    delay(400);
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static int lastPct = -1;
    const int pct = total ? (done * 100) / total : 0;
    if (pct != lastPct && pct % 10 == 0) {
      Serial.printf("OTA %d%%\n", pct);
      lastPct = pct;
    }
  });
  ArduinoOTA.onEnd([]() {
    otaInProgress = false;
    Serial.println("OTA done, rebooting into the new slot");
  });
  ArduinoOTA.onError([](ota_error_t err) {
    otaInProgress = false;
    Serial.printf("OTA failed (%u). The running firmware is untouched, reboot to recover\n", err);
  });

  ArduinoOTA.begin();
  Serial.printf("OTA ready on %s.local\n", cfg.cameraName.c_str());
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
      if (MDNS.begin(cfg.cameraName.c_str())) {
        MDNS.addService("http", "tcp", 80);
        MDNS.enableArduino(3232, true);  // ArduinoOTA no longer does this itself
      }
    } else if (offlineSince != 0) {
      // The first connect is not a reconnect. Counting it would put a permanent
      // off-by-one into the only number that says whether the link is healthy.
      reconnectCount++;
      webSetReconnects(reconnectCount);
      Serial.printf("wifi back after %lus, %s (reconnect #%u)\n",
                    (millis() - offlineSince) / 1000,
                    WiFi.localIP().toString().c_str(), reconnectCount);
      // mDNS loses its registration across a link drop and has to be restarted.
      MDNS.end();
      if (MDNS.begin(cfg.cameraName.c_str())) {
        MDNS.addService("http", "tcp", 80);
        MDNS.enableArduino(3232, true);
      }
    }
    offlineSince = 0;
    retryDelay = RETRY_MIN_MS;

    static bool otaStarted = false;
    if (!otaStarted) {
      startOta();
      otaStarted = true;
    }

    if (!serversStarted && startWebServers(cameraReady)) {
      serversStarted = true;

      // Opened only once the servers exist, so anyone joining it has something
      // to reach. Skipped entirely when the setting is off.
      if (cfg.apWindow) startApWindow();

      // Needs the network, so it starts here rather than in setup().
      clockBegin(cfg.timezone);
      statusLedSet(cameraReady ? Status::Online : Status::CameraFault);
      if (onTrial) {
        // Reaching the network with the servers answering is the only property
        // that decides whether a bad update needs a ladder. Nothing weaker
        // counts as proof.
        esp_ota_mark_app_valid_cancel_rollback();
        Serial.printf("firmware %s confirmed\n", FIRMWARE_VERSION);
        trialClear();
        onTrial = false;
      }
      Serial.printf("ready. open http://%s or http://%s.local\n",
                    WiFi.localIP().toString().c_str(), cfg.cameraName.c_str());
    }
    return;
  }

  if (offlineSince == 0) {
    offlineSince = millis();
    statusLedSet(Status::Searching);
  }

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
  WiFi.begin(apSsid.c_str(), cfg.wifiPass.c_str());
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

// Grabbing a frame costs about 18ms; decoding one to compare it costs far more.
// So the two run at different rates: history is captured often because it is
// cheap and choppy history is the thing that makes a pre-roll useless, while
// detection runs rarely because the buffer already covers the approach.
//
// The cost of slower detection is that movement must persist about a second to
// trigger. With five seconds of history behind the trigger, that is paid for.
static constexpr uint32_t PREROLL_INTERVAL_MS = 200;   // 5 fps of history
static constexpr uint32_t MOTION_INTERVAL_MS = 800;    // decode rate
// Auto exposure only needs to know what the light is doing, which changes over
// minutes, so it samples far more slowly than detection does when detection is
// off. Light does not need chasing at five frames a second.
static constexpr uint32_t AUTO_INTERVAL_MS = 3000;

// After a recording ends, ignore motion for a moment. Without it the movement
// that ends one recording immediately starts the next.
static constexpr uint32_t MOTION_COOLDOWN_MS = 4000;

static uint32_t lastMotionCheck = 0;
static uint32_t lastPrerollPush = 0;
static uint32_t motionBlockedUntil = 0;
static uint32_t lastObservedAt = 0;
static bool observedMoved = false;

// A schedule that has never seen a real clock would run against 1970, so an
// unsynced clock means the schedule does not apply rather than blocks recording.
bool motionArmed();

static bool withinSchedule() {
  if (!cfg.scheduleEnabled) return true;

  const int hour = clockHour();
  const int day = clockWeekday();
  if (hour < 0 || day < 0) return true;

  if (!(cfg.scheduleDays & (1 << day))) return false;

  if (cfg.scheduleFromHour == cfg.scheduleToHour) return true;  // all day
  if (cfg.scheduleFromHour < cfg.scheduleToHour) {
    return hour >= cfg.scheduleFromHour && hour < cfg.scheduleToHour;
  }
  // Crosses midnight: 22 to 6 means 22, 23, 0 through 5.
  return hour >= cfg.scheduleFromHour || hour < cfg.scheduleToHour;
}

// Runs on the frame the recorder already has, so keeping a recording alive costs
// no extra capture. Returns true while the scene is still changing.
static bool stillMoving(camera_fb_t *fb) {
  if (!cfg.motionEnabled) return false;
  return motionCheck(fb);
}

bool motionArmed() { return cfg.motionEnabled && withinSchedule(); }

// Reads the sensor's current image settings, since auto owns two of them and the
// rest stay wherever the user left them.
static ImageSettings imageFromConfig() {
  return ImageSettings{cfg.aeLevel,    cfg.gainCeiling, cfg.brightness,
                       cfg.contrast,   cfg.saturation,  cfg.wbMode,
                       cfg.grayscale,  cfg.hmirror,     cfg.vflip};
}

// One rung of the exposure ladder, if the scene has been asking for it. Nothing
// is written to flash: the rung is a reading of the current light, not a setting
// the user chose, and it finds its level again a few seconds after any boot.
static void autoExposure() {
  if (!cfg.autoImage) return;
  // The ladder's position is the sensor's, not the stored config's, so reloading
  // settings from flash cannot knock it back to where it was at boot.
  ImageSettings s = cameraCurrentImage();
  cameraAutoStep(motionBrightness(), s);
}

void motionObserve(camera_fb_t *fb) {
  if (!fb) return;
  const bool armed = motionArmed() && !recordingActive() &&
                     (int32_t)(millis() - motionBlockedUntil) >= 0;
  if (!armed && !cfg.autoImage) return;

  if (armed && cfg.prerollSeconds > 0 &&
      millis() - lastPrerollPush >= PREROLL_INTERVAL_MS) {
    lastPrerollPush = millis();
    prerollPush(fb);
  }

  // Detection and auto exposure both want the same decode, so they share it. With
  // detection off, the shared frame is sampled at the slower rate auto needs.
  if (millis() - lastMotionCheck >= (armed ? MOTION_INTERVAL_MS : AUTO_INTERVAL_MS)) {
    lastMotionCheck = millis();
    const bool moved = motionCheck(fb);
    if (moved && armed) observedMoved = true;
    autoExposure();
  }
  if (armed) lastObservedAt = millis();
}

// Auto exposure with nobody watching. A viewer's frames come through
// motionObserve() and cost nothing extra; this is the fallback for an idle
// camera, deliberately slow because a second consumer competes for the sensor.
static uint32_t lastAutoGrab = 0;
static uint32_t seenRevision = 0;
static void autoTick() {
  if (!cfg.autoImage || !cameraReady || portalMode) return;
  if (recordingActive() || motionArmed()) return;  // those paths already sample
  if (millis() - lastObservedAt < 2000) return;    // a viewer is feeding us
  if (millis() - lastAutoGrab < AUTO_INTERVAL_MS) return;
  lastAutoGrab = millis();

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;
  motionCheck(fb);
  esp_camera_fb_return(fb);
  autoExposure();
}

static void motionTick() {
  if (!cfg.motionEnabled || !cameraReady || portalMode) return;
  if (!withinSchedule()) return;
  if (recordingActive()) {
    // The recorder owns the camera and extends itself through the activity
    // check, so there is nothing to do here but hold the cooldown open.
    motionBlockedUntil = millis() + MOTION_COOLDOWN_MS;
    return;
  }
  if ((int32_t)(millis() - motionBlockedUntil) < 0) return;

  // A viewer feeds motionObserve() with the frames it is already fetching, so
  // grabbing here as well would put a second consumer on the camera. Only take
  // over when nobody has offered a frame recently.
  bool moved = observedMoved;
  observedMoved = false;

  if (millis() - lastObservedAt < 1000) {
    if (!moved) return;
  } else {
    const bool wantPreroll =
        cfg.prerollSeconds > 0 && millis() - lastPrerollPush >= PREROLL_INTERVAL_MS;
    const bool wantDetect = millis() - lastMotionCheck >= MOTION_INTERVAL_MS;
    if (!wantPreroll && !wantDetect) return;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return;

    if (wantPreroll) {
      lastPrerollPush = millis();
      prerollPush(fb);
    }
    if (wantDetect) {
      lastMotionCheck = millis();
      if (motionCheck(fb)) moved = true;
    }
    esp_camera_fb_return(fb);
    // This path owns the frame when motion is armed and nobody is watching, so
    // auto exposure has to be fed here too or it only runs while someone looks.
    if (wantDetect) autoExposure();
  }

  if (moved) {
    Serial.printf("motion: %u%% of the scene changed, recording\n", motionLastChange());
    // Before starting, not after: making room once the card is already full is
    // too late, and a recording that fails halfway is worse than one deferred.
    const int aged = sdAgeOut(cfg.keepFreeMb);
    if (aged) Serial.printf("aged out %d old recordings to make room\n", aged);

    recordingSetActivityCheck(stillMoving, cfg.quietSeconds);
    if (recordingStart(cfg.recordSeconds)) recordingMarkTriggered();
  }
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
  // Reset accounting runs first. None of it needs the serial port, and sitting
  // behind a two second delay meant a press only registered 2.4s after the
  // button, leaving barely half the window usable and no way to tell.
  statusLedInit();

  const esp_reset_reason_t why = esp_reset_reason();
  int presses = 0;
  if (why == ESP_RST_POWERON || why == ESP_RST_EXT) {
    presses = bumpBootCounter();
  } else {
    clearBootCounter();  // an OTA reboot or a panic is not someone at the button
  }
  const bool factoryReset = (presses >= RESET_PRESSES);
  webSetBootPress(presses, RESET_PRESSES);

  Serial.begin(115200);
  delay(2000);  // USB-TTL adapters routinely swallow the first output
  Serial.println();
  Serial.printf("reset reason %d, press %d of %d\n", (int)why, presses, RESET_PRESSES);

  const esp_partition_t *running = esp_ota_get_running_partition();
  const String slot = running ? running->label : "unknown";
  Serial.printf("firmware %s from partition %s\n", FIRMWARE_VERSION, slot.c_str());

  // Only an image that was uploaded and has not yet proven itself is a rollback
  // candidate. Firmware that has worked once stays trusted, so a failing power
  // supply cannot revert a perfectly good build.
  // The bootloader marks a freshly written image PENDING_VERIFY. If it reboots
  // without being confirmed, the previous slot is restored automatically, so a
  // hang or a crash recovers on its own rather than needing a cable.
  esp_ota_img_states_t otaState = ESP_OTA_IMG_VALID;
  if (running) esp_ota_get_state_partition(running, &otaState);
  onTrial = (otaState == ESP_OTA_IMG_PENDING_VERIFY);

  trialLoad(trial);
  if (!trial.pendingPartition.isEmpty() && trial.pendingPartition != slot) {
    // Booting a different slot than the one we wrote means the bootloader
    // reverted it. That is the only record of what failed, so keep it.
    Serial.printf("rolled back: %s did not confirm\n", trial.pendingVersion.c_str());
    trial.rolledBackFrom = trial.pendingVersion;
    trial.pendingPartition = "";
    trial.pendingVersion = "";
    trialSave(trial);
  }
  if (onTrial) Serial.println("firmware on trial, not yet confirmed");

  configLoad(cfg);

  if (factoryReset) {
    Serial.println("reset pressed three times, clearing stored settings");
    clearBootCounter();
    configClear();
    cfg = Config();
  }

  // No stored network means there is nothing to join and nobody to authenticate,
  // so the only useful thing the camera can do is ask. The camera itself stays
  // uninitialised: setup needs the radio and the memory, not frames.
  if (!cfg.configured) {
    portalMode = true;
    statusLedSet(Status::Searching);
    if (!startSetupPortal()) {
      Serial.println("could not start setup portal, restarting");
      delay(2000);
      ESP.restart();
    }
    return;
  }

  Serial.printf("PSRAM: %s (%u bytes free)\n",
                psramFound() ? "found" : "MISSING", ESP.getFreePsram());

  // A camera fault must not take the network down with it. Staying reachable is
  // what makes the difference between diagnosing this over the air and going
  // back to the cable with the SD card out.
  // Camera first. Mounting the card draws current and leaves the SDMMC
  // peripheral running, and the sensor's power-up is the thing that has been
  // failing. It is also the primary function: if only one of the two can come
  // up, it should be the camera.
  cameraApplySettings(cfg.frameSize, cfg.jpegQuality);
  cameraReady = cameraInit();
  cameraApplySettings(cfg.frameSize, cfg.jpegQuality);

  // Stored image settings are the sensor's defaults until someone changes them,
  // so an untouched camera behaves exactly as it did before these existed.
  const ImageSettings img = imageFromConfig();
  cameraApplyImage(img);
  flashSetLevel(cfg.flashLevel);

  // A megabyte and a half holds roughly five seconds at 640x480 and less at
  // larger sizes, which is why the buffer is bounded by bytes and reports how
  // many seconds it actually managed.
  prerollInit(1536 * 1024);
  prerollSetWindow(cfg.prerollSeconds);
  motionInit();
  motionSetSensitivity(cfg.motionSensitivity);

  sdInit();

  // After SDMMC, so GPIO4 ends up configured by us whatever the driver touched.
  flashInit();
  if (!cameraReady) {
    Serial.println("CAMERA FAULT: sensor did not respond. Check the ribbon cable.");
    Serial.println("Continuing without it so the camera stays reachable.");
  }

  startWifi();
  statusLedSet(Status::Searching);
  Serial.println("send 'p' for a frame, 'd' to force a wifi drop");
}

void loop() {
  // Running this long means nobody is at the pinhole, so the tap count resets.
  static bool windowClosed = false;
  if (!windowClosed && millis() > RESET_WINDOW_MS) {
    clearBootCounter();
    windowClosed = true;
  }

  if (portalMode) {
    portalLoop();
    statusLedTick();
    delay(5);
    return;
  }

  // Drives the DNS responder and closes the window when its time is up.
  portalLoop();
  statusLedTick();
  recordingTick();
  // The web server runs on its own task and saves to flash; without this the loop
  // would keep using the settings it read at boot.
  if (configRevision() != seenRevision) {
    seenRevision = configRevision();
    configLoad(cfg);
    motionSetSensitivity(cfg.motionSensitivity);
  }
  motionTick();
  autoTick();

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
  ArduinoOTA.handle();

  static unsigned long lastReport = 0;
  if (millis() - lastReport > 15000) {
    lastReport = millis();
    Serial.printf("up %lus  heap %u  psram %u  reconnects %u  wifi %s\n",
                  millis() / 1000, ESP.getFreeHeap(), ESP.getFreePsram(), reconnectCount,
                  WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "DOWN");
  }
  // The loop pace sets the recording ceiling: recordingTick() runs once per
  // iteration, so a fixed 50ms delay caps recording at 20 fps however fast the
  // card is. Idle, a slower loop costs nothing and leaves the CPU alone.
  delay(recordingActive() ? 1 : 50);
}
