#include <Arduino.h>
#include <DNSServer.h>
#include <WiFi.h>
#include "esp_http_server.h"
#include "esp_mac.h"

#include "config.h"
#include "httputil.h"
#include "portal.h"

static DNSServer dnsServer;
static httpd_handle_t portalServer = nullptr;
static bool portalRunning = false;
static bool windowRunning = false;
static uint32_t windowClosesAt = 0;
static int windowChannel = 0;
static bool openAp(int channel);
static uint32_t windowChannelCheckedAt = 0;
static uint32_t stationHealthySince = 0;

// The access point exists to get someone in when the network does not work.
// While the station is connected there is already a way in, and sharing one
// radio between both costs roughly seven times the latency. So the window runs
// its full length only when it is actually needed.
static constexpr uint32_t STATION_HEALTHY_MS = 60UL * 1000UL;

// Long enough to fetch a laptop from another room, short enough that the camera
// is not broadcasting an access point all day.
static constexpr uint32_t AP_WINDOW_MS = 15UL * 60UL * 1000UL;
static String apSsid;

// The ESP32 default is 192.168.4.1, chosen because it avoids the two ranges home
// routers use most. This is set to a common range deliberately; if a phone joins
// the setup network while holding a lease on an overlapping subnet, the setup
// page may be unreachable until it forgets the other network.
static const IPAddress AP_IP(192, 168, 0, 1);
static const IPAddress AP_NETMASK(255, 255, 255, 0);

static const char SETUP_CSS[] =
    "body{margin:0;background:#111;color:#eee;font:15px system-ui,sans-serif;"
    "color-scheme:dark;"
    "display:flex;justify-content:center;padding:24px}"
    "form{width:100%;max-width:380px}h1{font-size:20px;margin:0 0 4px}"
    "p.sub{color:#999;margin:0 0 20px;font-size:13px}"
    "label{display:block;margin:14px 0 4px;font-size:13px;color:#bbb}"
    "input,select{width:100%;box-sizing:border-box;padding:9px;border-radius:5px;"
    "border:1px solid #444;background:#1c1c1c;color:#eee;font-size:15px;"
    "font-family:inherit}"
    "select:focus,input:focus{outline:none;border-color:#2a7}"
    "option{background:#1c1c1c;color:#eee}"
    "button{width:100%;margin-top:22px;padding:11px;border:0;border-radius:5px;"
    "background:#2a7;color:#04140d;font-size:15px;font-weight:600}"
    "small{color:#888;display:block;margin-top:6px;font-size:12px}";

static esp_err_t sendPage(httpd_req_t *req, const String &body) {
  String page = "<!doctype html><meta charset=utf-8>"
                "<meta name=viewport content='width=device-width,initial-scale=1'>"
                "<title>Camera setup</title><style>";
  page += SETUP_CSS;
  page += "</style>";
  page += body;
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, page.c_str(), page.length());
}

// A name a person will actually keep, offered as the default at setup. "camera1"
// gets accepted and then nobody can tell which camera it is; a name has to be
// distinct before it is memorable. Drawn from the myth the aggregator is named
// for, and picked from the chip's own MAC so the same board always proposes the
// same name however many times setup is run.
static String suggestedName() {
  static const char *NAMES[] = {
      "argos",  "hermes", "iris",   "helios", "selene", "theia",  "eos",
      "atlas",  "hyperion", "kronos", "rhea",  "phoebe", "tethys", "nyx",
      "erebus", "aether", "chaos",  "gaia",   "pontus", "thalia", "clio",
      "urania", "calliope", "euterpe", "terpsi", "erato", "polymnia", "melpomene",
      "boreas", "notus",  "zephyr", "eurus",  "triton", "nereus", "proteus"};
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  const uint32_t seed = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
  return NAMES[seed % (sizeof(NAMES) / sizeof(NAMES[0]))];
}

static esp_err_t setupPageHandler(httpd_req_t *req) {
  // Scanning needs the station interface, which is why the portal runs AP_STA
  // rather than plain AP.
  const int found = WiFi.scanNetworks();

  String body = "<form method=post action=/setup>"
                "<h1>Camera setup</h1>"
                "<p class=sub>Name the camera, choose a network, and set the "
                "password you will use to sign in from now on.</p>"
                "<label>Camera name</label><input name=camname value=\"" +
                htmlEscape(suggestedName()) + "\" required>"
                "<small>Becomes the address you visit, so keep it simple. "
                "Letters, numbers and hyphens.</small>"
                "<label>Network</label><select name=ssid>";
  for (int i = 0; i < found; i++) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    body += "<option value=\"" + htmlEscape(ssid) + "\">" + htmlEscape(ssid) +
            "  (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  body += "</select>";
  if (found <= 0) body += "<small>No networks found. The camera is 2.4GHz only.</small>";

  body += "<label>Network password</label><input type=password name=wifipass>"
          "<label>Admin username</label><input name=user value=admin required>"
          "<label>Admin password</label><input type=password name=pass required minlength=8>"
          "<label>Confirm password</label><input type=password name=pass2 required minlength=8>"
          "<small>At least 8 characters. There is no recovery: if you lose it, "
          "double-tap the reset button to start over.</small>"
          "<button type=submit>Save and restart</button></form>";

  WiFi.scanDelete();
  return sendPage(req, body);
}

static esp_err_t saveHandler(httpd_req_t *req) {
  String body;
  if (!readBody(req, body)) return ESP_FAIL;

  const String camname = formField(body, "camname");
  const String ssid = formField(body, "ssid");
  const String wifipass = formField(body, "wifipass");
  const String user = formField(body, "user");
  const String pass = formField(body, "pass");
  const String pass2 = formField(body, "pass2");
  const String otapw = formField(body, "otapw");

  String problem;
  if (camname.isEmpty()) problem = "Give the camera a name.";
  else if (ssid.isEmpty()) problem = "Pick a network.";
  else if (user.isEmpty()) problem = "Username cannot be empty.";
  else if (pass.length() < 8) problem = "Password must be at least 8 characters.";
  else if (pass != pass2) problem = "The two passwords do not match.";

  if (!problem.isEmpty()) {
    return sendPage(req, "<h1>Not saved</h1><p class=sub>" + htmlEscape(problem) +
                             " <a href=/>Back</a></p>");
  }

  Config cfg;
  cfg.cameraName = sanitizeHostname(camname);
  cfg.wifiSsid = ssid;
  cfg.wifiPass = wifipass;
  cfg.adminUser = user;
  cfg.otaPassword = otapw.isEmpty() ? makeSalt().substring(0, 12) : otapw;
  cfg.adminSalt = makeSalt();
  cfg.adminHash = derivePasswordHash(cfg.adminSalt, pass);
  cfg.otaMd5 = md5Hex(pass);
  cfg.configured = true;

  if (cfg.adminHash.isEmpty() || !configSave(cfg)) {
    return sendPage(req, "<h1>Could not save</h1><p class=sub>Storage write failed. "
                         "<a href=/>Try again</a></p>");
  }

  Serial.printf("setup saved: name [%s], ssid [%s], user [%s]\n",
                cfg.cameraName.c_str(), ssid.c_str(), user.c_str());
  const esp_err_t res = sendPage(
      req, "<h1>Saved</h1><p class=sub>The camera is restarting and will join <b>" +
               htmlEscape(ssid) +
               "</b>. This network will disappear.</p><p class=sub>Find it at "
               "<b>http://" + htmlEscape(cfg.cameraName) + ".local</b> and sign in "
               "with the password you just set.</p>");

  // Give the browser time to render before the interface drops out from under it.
  delay(1500);
  ESP.restart();
  return res;
}

// Captive portals work by answering every lookup with the device's own address,
// so a phone's connectivity check fails in the way that pops the sign-in sheet.
static esp_err_t redirectHandler(httpd_req_t *req) {
  const String target = "http://" + WiFi.softAPIP().toString() + "/";
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", target.c_str());
  return httpd_resp_send(req, "", 0);
}

String portalApSsid() { return apSsid; }

bool startSetupPortal() {
  // Read from efuse rather than the interface: this is available before the
  // radio starts, where WiFi.macAddress() returns zeros.
  const uint64_t chipId = ESP.getEfuseMac();
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02X%02X",
           (uint8_t)((chipId >> 32) & 0xFF), (uint8_t)((chipId >> 40) & 0xFF));
  apSsid = String("ESP32CAM-Setup-") + suffix;

  WiFi.mode(WIFI_AP_STA);

  // Must precede softAP: configuring the interface after it is up has no effect.
  if (!WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK)) {
    Serial.println("softAPConfig failed");
    return false;
  }
  if (!WiFi.softAP(apSsid.c_str())) {
    Serial.println("softAP failed to start");
    return false;
  }

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port = 32768;
  cfg.lru_purge_enable = true;
  cfg.stack_size = 8192;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  if (httpd_start(&portalServer, &cfg) != ESP_OK) {
    Serial.println("portal server failed to start");
    return false;
  }

  httpd_uri_t root  = {"/",      HTTP_GET,  setupPageHandler, nullptr};
  httpd_uri_t save  = {"/setup", HTTP_POST, saveHandler,      nullptr};
  httpd_uri_t catch_all = {"/*", HTTP_GET,  redirectHandler,  nullptr};
  httpd_register_uri_handler(portalServer, &root);
  httpd_register_uri_handler(portalServer, &save);
  httpd_register_uri_handler(portalServer, &catch_all);

  portalRunning = true;
  Serial.printf("setup portal up: join \"%s\" then open http://%s\n",
                apSsid.c_str(), WiFi.softAPIP().toString().c_str());
  return true;
}

void portalLoop() {
  if (portalRunning || windowRunning) dnsServer.processNextRequest();
  if (!windowRunning) return;

  if (apWindowSecondsLeft() == 0) {
    stopApWindow();
    return;
  }

  // Close early once the station has held a connection for a minute. A drop
  // resets the clock, and a station that never recovers takes the camera through
  // its offline reboot, which opens a fresh window.
  if (WiFi.status() == WL_CONNECTED) {
    if (stationHealthySince == 0) stationHealthySince = millis();
    if (millis() - stationHealthySince > STATION_HEALTHY_MS) {
      Serial.println("station healthy, closing the window early to free the radio");
      stopApWindow();
      return;
    }
  } else {
    stationHealthySince = 0;
  }

  // A router that changes channel takes the station with it and leaves the
  // access point stranded on the old one, still broadcasting but unreachable.
  // Follow it, so the recovery path stays a recovery path.
  if (millis() - windowChannelCheckedAt < 3000) return;
  windowChannelCheckedAt = millis();

  if (WiFi.status() != WL_CONNECTED) return;
  const int staChannel = WiFi.channel();
  if (staChannel == windowChannel || staChannel <= 0) return;

  Serial.printf("station moved to ch%d, reopening the access point there\n", staChannel);
  if (!openAp(staChannel)) {
    Serial.println("could not follow the channel change, closing the window");
    stopApWindow();
  }
}

static String apName() {
  const uint64_t chipId = ESP.getEfuseMac();
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02X%02X",
           (uint8_t)((chipId >> 32) & 0xFF), (uint8_t)((chipId >> 40) & 0xFF));
  return String("ESP32CAM-Setup-") + suffix;
}

// One radio means one channel. Letting softAP pick its own drags the station off
// the router's channel and drops the connection, which is the opposite of what a
// recovery feature should do. Always follow the station.
static bool openAp(int channel) {
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK)) return false;
  if (!WiFi.softAP(apSsid.c_str(), nullptr, channel)) return false;
  windowChannel = channel;
  return true;
}

bool startApWindow() {
  if (windowRunning || portalRunning) return false;

  apSsid = apName();
  // Only meaningful when associated. Disconnected, the channel reads stale, so
  // pick a legal default rather than an arbitrary one.
  const int staChannel = WiFi.status() == WL_CONNECTED ? WiFi.channel() : 1;
  if (!openAp(staChannel)) {
    Serial.println("could not open the maintenance window");
    return false;
  }
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());

  windowRunning = true;
  windowClosesAt = millis() + AP_WINDOW_MS;
  stationHealthySince = 0;
  Serial.printf("maintenance window open for %lu minutes on ch%d: join \"%s\", "
                "then http://%s and sign in\n",
                (unsigned long)(AP_WINDOW_MS / 60000), staChannel, apSsid.c_str(),
                WiFi.softAPIP().toString().c_str());
  return true;
}

void stopApWindow() {
  if (!windowRunning) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  windowRunning = false;
  Serial.println("maintenance window closed");
}

bool apWindowOpen() { return windowRunning; }

uint32_t apWindowSecondsLeft() {
  if (!windowRunning) return 0;
  const int32_t left = (int32_t)(windowClosesAt - millis());
  return left > 0 ? (uint32_t)(left / 1000) : 0;
}
