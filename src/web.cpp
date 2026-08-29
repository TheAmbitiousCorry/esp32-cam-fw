#include <Arduino.h>
#include <Update.h>
#include <WiFi.h>
#include "esp_http_server.h"
#include "esp_ota_ops.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif

#include "auth.h"
#include "camera.h"
#include "config.h"
#include "portal.h"
#include "httputil.h"
#include "web.h"

// An MJPEG stream is one HTTP response that never ends: a multipart body where
// each part is a whole JPEG. The boundary string just has to be something that
// cannot appear in the payload.
#define PART_BOUNDARY "espcamframeboundary"
static const char *STREAM_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// Cached at startup so page requests do not each hit NVS for a value that only
// changes when the camera is reconfigured and rebooted.
static String cameraName = "camera";
static bool cameraAvailable = false;
static uint32_t reconnectTally = 0;

// Set while firmware is being written. The stream handler polls it and returns,
// which is the only way out of a handler that otherwise loops until the client
// disconnects. Calling httpd_stop() on that server instead deadlocks: it waits
// for the handler, and the handler is waiting for a client that is still there.
static volatile bool updating = false;
static int bootPress = 0;
static int bootPressNeeded = 3;

static httpd_handle_t pageServer = nullptr;
static httpd_handle_t streamServer = nullptr;

static const char SHARED_CSS[] =
    "*{box-sizing:border-box}"
    "body{margin:0;background:#111;color:#eee;font:15px system-ui,sans-serif}"
    ".app{display:flex;min-height:100vh}"
    "aside{width:180px;flex:0 0 180px;background:#181818;border-right:1px solid #262626;"
    "padding:18px 0;display:flex;flex-direction:column;gap:2px}"
    ".brand{padding:0 18px 16px;font-weight:600;font-size:15px;color:#fff;"
    "overflow-wrap:anywhere}"
    "aside a{display:block;padding:9px 18px;color:#aaa;text-decoration:none;font-size:14px;"
    "border-left:3px solid transparent}"
    "aside a:hover{background:#1f1f1f;color:#eee}"
    "aside a.on{color:#fff;background:#1f1f1f;border-left-color:#2a7}"
    "aside .spacer{flex:1}"
    "main{flex:1;padding:22px;min-width:0}"
    "h1{font-size:19px;margin:0 0 16px}h2{font-size:17px;margin:0 0 6px}"
    "img{max-width:100%;border-radius:6px;background:#000;display:block}"
    ".actions{display:flex;gap:10px;flex-wrap:wrap;margin:14px 0}"
    "button,.btn{padding:9px 15px;border:1px solid #3a3a3a;border-radius:6px;"
    "background:#222;color:#eee;font:inherit;font-size:14px;cursor:pointer;"
    "text-decoration:none;display:inline-block}"
    "button:hover,.btn:hover{background:#2b2b2b;border-color:#4a4a4a}"
    "button.on{background:#2a7;border-color:#2a7;color:#04140d;font-weight:600}"
    "button.primary{background:#2a7;border-color:#2a7;color:#04140d;font-weight:600}"
    "p.sub{color:#999;font-size:13px;max-width:460px}"
    ".err{color:#f77;font-size:13px}"
    "table{border-collapse:collapse;font-size:14px}"
    "th{text-align:left;color:#999;font-weight:400;padding:6px 22px 6px 0;white-space:nowrap}"
    "td{padding:6px 0;font-variant-numeric:tabular-nums}"
    "input{width:100%;padding:9px;border-radius:5px;border:1px solid #444;"
    "background:#1c1c1c;color:#eee;font-size:15px}"
    "label{display:block;margin:14px 0 4px;font-size:13px;color:#bbb}"
    ".card{max-width:320px;margin:12vh auto;padding:0 20px}"
    "@media(max-width:640px){.app{flex-direction:column}"
    "aside{width:auto;flex:none;flex-direction:row;overflow-x:auto;padding:0;"
    "border-right:0;border-bottom:1px solid #262626}"
    ".brand{display:none}aside .spacer{display:none}"
    "aside a{border-left:0;border-bottom:3px solid transparent;white-space:nowrap}"
    "aside a.on{border-left:0;border-bottom-color:#2a7}}";

// Wraps registration so a full handler table is loud rather than a mystery 404.
static void registerUri(httpd_handle_t server, const httpd_uri_t *uri) {
  const esp_err_t err = httpd_register_uri_handler(server, uri);
  if (err != ESP_OK) {
    Serial.printf("failed to register %s: %s\n", uri->uri, esp_err_to_name(err));
  }
}

static esp_err_t sendHtml(httpd_req_t *req, const String &body) {
  String page = "<!doctype html><meta charset=utf-8>"
                "<meta name=viewport content='width=device-width,initial-scale=1'>"
                "<title>";
  page += htmlEscape(cameraName);
  page += "</title><style>";
  page += SHARED_CSS;
  page += "</style>";
  page += body;
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, page.c_str(), page.length());
}

// Every signed-in page shares the sidebar; `active` marks the current entry.
static esp_err_t sendShell(httpd_req_t *req, const char *active, const String &main) {
  struct Item { const char *href; const char *label; };
  static const Item items[] = {
      {"/", "Live view"}, {"/status", "Status"}, {"/settings", "Settings"},
      {"/update", "Firmware"}};

  String nav = "<div class=app><aside><div class=brand>" + htmlEscape(cameraName) + "</div>";
  for (const Item &it : items) {
    nav += String("<a href=\"") + it.href + "\"";
    if (strcmp(it.href, active) == 0) nav += " class=on";
    nav += ">" + String(it.label) + "</a>";
  }
  nav += "<div class=spacer></div><a href=\"/logout\">Sign out</a></aside><main>";
  return sendHtml(req, nav + main + "</main></div>");
}

static esp_err_t sendLoginPage(httpd_req_t *req, const String &error) {
  String body = "<div class=card><form method=post action=/login>"
                "<h2>" + htmlEscape(cameraName) + "</h2>"
                "<p class=sub>Sign in to view this camera.</p>"
                "<label>Username</label><input name=user autofocus required>"
                "<label>Password</label><input type=password name=pass required>";
  if (!error.isEmpty()) body += "<p class=err>" + htmlEscape(error) + "</p>";
  body += "<button type=submit class=primary style=\"width:100%;margin-top:20px\">"
          "Sign in</button></form></div>";
  return sendHtml(req, body);
}

static esp_err_t loginPageHandler(httpd_req_t *req) {
  if (authIsSignedIn(req)) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, "", 0);
  }
  return sendLoginPage(req, "");
}

static esp_err_t loginPostHandler(httpd_req_t *req) {
  String body;
  if (!readBody(req, body)) return sendLoginPage(req, "Bad request.");

  Config cfg;
  if (!configLoad(cfg)) return sendLoginPage(req, "This camera has not been set up.");

  const String user = formField(body, "user");
  const String pass = formField(body, "pass");

  if (user != cfg.adminUser || !passwordMatches(cfg, pass)) {
    // Deliberate pause. PBKDF2 makes offline guessing slow; this makes online
    // guessing slow too, without needing to track attempts per address.
    delay(1200);
    Serial.println("failed sign-in attempt");
    return sendLoginPage(req, "Wrong username or password.");
  }

  const String token = authCreateSession();

  // httpd_resp_set_hdr stores the pointer rather than copying, so the value has
  // to outlive the send. A temporary here dangles and the cookie never arrives.
  const String cookie = authSessionCookie(token);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_set_hdr(req, "Set-Cookie", cookie.c_str());
  Serial.printf("signed in as %s\n", cfg.adminUser.c_str());
  return httpd_resp_send(req, "", 0);
}

static esp_err_t logoutHandler(httpd_req_t *req) {
  authEndSession(req);
  const String cookie = authClearCookie();
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/login");
  httpd_resp_set_hdr(req, "Set-Cookie", cookie.c_str());
  return httpd_resp_send(req, "", 0);
}

static const char INDEX_BODY[] = R"HTML(<img id="v" alt="live view">
<script>
const fb = document.getElementById('flash');
if (fb) fb.onclick = async () => {
  const state = await (await fetch('/flash', {method: 'POST'})).text();
  fb.textContent = 'Flash ' + state;
  fb.className = state === 'on' ? 'on' : '';
};

// The stream lives on its own port, so build the URL from wherever this page was
// served rather than hardcoding an address.
document.getElementById('v').src = 'http://' + location.hostname + ':81/stream';
</script>
)HTML";

static esp_err_t indexHandler(httpd_req_t *req) {
  if (!authGuardPage(req)) return ESP_OK;

  if (!webCameraReady()) {
    return sendShell(req, "/",
                     "<h1>Live view</h1><p class=err>Camera sensor not detected. "
                     "Check the ribbon cable connector on the module.</p>");
  }

  String body = "<h1>Live view</h1><div class=actions>";
  body += String("<button id=flash class=\"") + (flashIsOn() ? "on" : "") + "\">Flash "
          + (flashIsOn() ? "on" : "off") + "</button>";
  body += "<a class=btn href=\"/capture\" target=_blank>Still image</a></div>";
  body += INDEX_BODY;
  return sendShell(req, "/", body);
}

// Toggles rather than taking a state, so the button cannot disagree with the
// device when two browsers are open on the same camera.
static esp_err_t flashHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;
  flashSet(!flashIsOn());
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, flashIsOn() ? "on" : "off", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captureHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "camera unavailable", HTTPD_RESP_USE_STRLEN);
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  const esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t streamHandler(httpd_req_t *req) {
  // The browser sends the session cookie with the <img> request, because cookies
  // are scoped to the host and ignore the port this server listens on.
  if (!authGuardResource(req)) return ESP_OK;

  esp_err_t res = httpd_resp_set_type(req, STREAM_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char partHeader[64];
  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
      break;
    }

    const size_t hlen = snprintf(partHeader, sizeof(partHeader), STREAM_PART, fb->len);
    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, partHeader, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);

    // Return the buffer before deciding whether to stop, or a disconnect leaks
    // one and the driver stalls after fb_count frames.
    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;   // client went away, which is the normal exit
    if (updating) break;        // firmware is being written; release the camera
  }
  return res;
}

bool webCameraReady() { return cameraAvailable; }

void webSetReconnects(uint32_t n) { reconnectTally = n; }

void webSetBootPress(int presses, int needed) {
  bootPress = presses;
  bootPressNeeded = needed;
}

static String humanUptime() {
  const uint32_t s = millis() / 1000;
  char buf[32];
  snprintf(buf, sizeof(buf), "%lud %02lu:%02lu:%02lu", (unsigned long)(s / 86400),
           (unsigned long)((s % 86400) / 3600), (unsigned long)((s % 3600) / 60),
           (unsigned long)(s % 60));
  return buf;
}

static esp_err_t statusHandler(httpd_req_t *req) {
  if (!authGuardPage(req)) return ESP_OK;

  const esp_partition_t *running = esp_ota_get_running_partition();
  String body = "<h1>Status</h1><table>";

  auto row = [&body](const char *k, const String &v) {
    body += "<tr><th>" + String(k) + "</th><td>" + htmlEscape(v) + "</td></tr>";
  };
  row("Camera sensor", cameraAvailable ? "detected" : "NOT DETECTED");
  row("Uptime", humanUptime());
  const bool online = WiFi.status() == WL_CONNECTED;
  row("Network", online ? WiFi.SSID() : String("disconnected"));
  row("Address", online ? WiFi.localIP().toString() : String("none"));
  row("Signal", online ? String(WiFi.RSSI()) + " dBm" : String("n/a"));
  row("Reconnects", String(reconnectTally));
  row("Free heap", String(ESP.getFreeHeap()) + " bytes");
  row("Free PSRAM", String(ESP.getFreePsram()) + " bytes");
  row("Running slot", running ? running->label : "unknown");
  row("Setup access point", apWindowOpen()
                                ? "open, " + String(apWindowSecondsLeft() / 60) +
                                      " min left"
                                : "closed");
  row("Reset presses", String(bootPress) + " of " + String(bootPressNeeded) + " at last boot");
  row("Firmware", String(FIRMWARE_VERSION));
  row("Built", String(__DATE__) + " " + __TIME__);

  esp_ota_img_states_t otaState = ESP_OTA_IMG_VALID;
  if (running) esp_ota_get_state_partition(running, &otaState);
  row("Update status", otaState == ESP_OTA_IMG_PENDING_VERIFY
                           ? "on trial, reverts if it reboots unconfirmed"
                           : "confirmed");

  TrialState trial;
  trialLoad(trial);
  if (!trial.rolledBackFrom.isEmpty()) {
    row("Last rollback", "reverted from " + trial.rolledBackFrom);
  }

  Config stored;
  if (configLoad(stored)) {
    row("Update password", stored.otaPassword.isEmpty() ? "none set" : stored.otaPassword);
  }
  body += "</table><div class=actions>"
          "<a class=btn href=\"/restart\">Restart camera</a></div>";
  return sendShell(req, "/status", body);
}

static esp_err_t sendSettings(httpd_req_t *req, const String &notice) {
  Config stored;
  configLoad(stored);

  String body = "<h1>Settings</h1><form method=post action=/settings "
                "style=\"max-width:340px\">";
  body += "<label>Camera name</label><input name=camname value=\"" +
          htmlEscape(stored.cameraName) + "\" required>";
  body += "<small class=sub>Changing this changes the address you visit.</small>";
  // Scanning blocks and hops channels, which would drop both the station and
  // anyone on the maintenance access point mid-request. Kick it off
  // asynchronously and let the page collect the result when it is ready.
  if (WiFi.scanComplete() == WIFI_SCAN_FAILED) WiFi.scanNetworks(true, true);

  body += "<label>Wi-Fi network</label>"
          "<select id=scanlist style=\"margin-bottom:6px\">"
          "<option value=\"\">Scanning...</option></select>"
          "<input name=ssid id=ssid value=\"" + htmlEscape(stored.wifiSsid) +
          "\" required>";
  body += "<small class=sub>Pick one from the list, or type it if the network is "
          "hidden. The box below is what gets saved.</small>";
  body += "<label>Wi-Fi password</label><input type=password name=wifipass "
          "placeholder=\"leave blank to keep the current one\">";
  body += "<small class=sub>Getting this wrong takes the camera off the network. "
          "It keeps retrying, and offers a recovery access point if it cannot get "
          "back on.</small>";
  body += "<label>Firmware update password</label><input name=otapw value=\"" +
          htmlEscape(stored.otaPassword) + "\" required>";
  body += String("<label><input type=checkbox name=apwin value=1 style=\"width:auto\"") +
          (stored.apWindow ? " checked" : "") +
          "> Open a setup access point for 15 minutes after each restart</label>";
  body += "<small class=sub>How you get back in if this camera's network stops "
          "working. It closes on its own, and still requires your sign-in "
          "password.</small>";
  body += "<small class=sub>Used by command line tools. Separate from your sign-in "
          "password on purpose: the update protocol stores it weakly, and your "
          "login should not inherit that.</small>";
  if (!notice.isEmpty()) body += "<p class=sub>" + htmlEscape(notice) + "</p>";
  body += "<div class=actions><button type=submit class=primary>Save and restart"
          "</button></div></form>";
  body += R"HTML(<script>
const list = document.getElementById('scanlist');
const box = document.getElementById('ssid');
// Selecting from the list fills the text box rather than replacing it, so a
// hidden network typed by hand is never clobbered by a scan finishing late.
list.onchange = () => { if (list.value) box.value = list.value; };
(async function poll(tries) {
  const nets = await (await fetch('/networks')).json();
  if (nets.length === 0 && tries > 0) return setTimeout(() => poll(tries - 1), 1200);
  list.innerHTML = '<option value="">' +
    (nets.length ? 'Choose a network...' : 'No networks found') + '</option>';
  for (const n of nets) {
    const o = document.createElement('option');
    o.value = n.s;
    o.textContent = n.s + '  (' + n.r + ' dBm)';
    list.appendChild(o);
  }
})(8);
</script>)HTML";
  return sendShell(req, "/settings", body);
}

// Returns whatever the asynchronous scan has produced, or an empty list while it
// is still running, so the page can poll without ever blocking the device.
// A way back from a wedged service without pulling the power. Costs one handler
// and removes the only remaining reason to reach for the plug.
static esp_err_t restartHandler(httpd_req_t *req) {
  if (!authGuardPage(req)) return ESP_OK;
  const esp_err_t res = sendShell(req, "/status",
                                  "<h1>Restarting</h1><p class=sub>Back in about "
                                  "fifteen seconds. You will need to sign in "
                                  "again.</p>");
  Serial.println("restart requested from the web interface");
  delay(1200);
  ESP.restart();
  return res;
}

static esp_err_t networksHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;

  const int found = WiFi.scanComplete();
  String json = "[";
  if (found > 0) {
    for (int i = 0; i < found; i++) {
      const String ssid = WiFi.SSID(i);
      if (ssid.isEmpty()) continue;
      if (json.length() > 1) json += ",";
      String escaped;
      for (size_t c = 0; c < ssid.length(); c++) {
        const char ch = ssid[c];
        if (ch == '"' || ch == '\\') escaped += '\\';
        escaped += ch;
      }
      json += "{\"s\":\"" + escaped + "\",\"r\":" + String(WiFi.RSSI(i)) + "}";
    }
    WiFi.scanDelete();
  }
  json += "]";

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json.c_str(), json.length());
}

static esp_err_t settingsPageHandler(httpd_req_t *req) {
  if (!authGuardPage(req)) return ESP_OK;
  return sendSettings(req, "");
}

static esp_err_t settingsPostHandler(httpd_req_t *req) {
  if (!authGuardPage(req)) return ESP_OK;

  String body;
  if (!readBody(req, body)) return sendSettings(req, "Bad request.");

  Config stored;
  if (!configLoad(stored)) return sendSettings(req, "No stored configuration.");

  const String camname = formField(body, "camname");
  const String otapw = formField(body, "otapw");
  const String ssid = formField(body, "ssid");
  const String wifipass = formField(body, "wifipass");
  if (camname.isEmpty() || otapw.isEmpty() || ssid.isEmpty()) {
    return sendSettings(req, "Name, network and update password are all required.");
  }

  stored.cameraName = sanitizeHostname(camname);
  stored.otaPassword = otapw;
  stored.wifiSsid = ssid;
  stored.apWindow = !formField(body, "apwin").isEmpty();
  // Blank means unchanged: echoing a stored password back into a form only to
  // have it submitted again is a good way to lose it to a typo.
  if (!wifipass.isEmpty()) stored.wifiPass = wifipass;
  if (!configSave(stored)) return sendSettings(req, "Could not write settings.");

  // A restart rather than applying in place: mDNS and ArduinoOTA both bind their
  // names at startup, and re-registering them live is more moving parts than a
  // three second reboot is worth.
  String body2 = "<h1>Saved</h1><p class=sub>Restarting. The camera will be at "
                 "<b>http://" + htmlEscape(stored.cameraName) + ".local</b>.</p>";
  const esp_err_t res = sendShell(req, "/settings", body2);
  delay(1200);
  ESP.restart();
  return res;
}

static const char UPDATE_BODY[] = R"HTML(
<h1>Firmware</h1>
<p class=sub>Upload a firmware.bin. The camera reboots into it and signs you out.
If it fails, the running firmware is untouched.</p>
<div class=actions><input type=file id=f accept=".bin" style="max-width:280px">
<button id=go class=primary>Upload</button></div>
<p id=msg class=sub></p>
<script>
const msg = document.getElementById('msg');
document.getElementById('go').onclick = async () => {
  const file = document.getElementById('f').files[0];
  if (!file) { msg.textContent = 'Pick a file first.'; return; }
  msg.textContent = 'Uploading ' + file.size + ' bytes...';
  try {
    // Sent as a raw body rather than multipart: parsing multipart on the device
    // would cost more code than the whole upload path.
    const r = await fetch('/update', {method: 'POST', body: file});
    msg.textContent = await r.text();
  } catch (e) { msg.textContent = 'Upload failed: ' + e; }
};
</script>
)HTML";

static esp_err_t updatePageHandler(httpd_req_t *req) {
  if (!authGuardPage(req)) return ESP_OK;
  return sendShell(req, "/update", UPDATE_BODY);
}

static esp_err_t updatePostHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;

  const size_t total = req->content_len;
  if (total < 1024) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "That file is too small to be firmware.", HTTPD_RESP_USE_STRLEN);
  }

  // Ask any live stream to stop, then give it a moment to notice. The camera
  // itself is left initialised: Update brings its own buffer, and tearing the
  // driver down while a handler might still hold a frame buffer trades one hang
  // for another.
  Serial.printf("web update starting, %u bytes\n", total);
  updating = true;
  delay(400);

  if (!Update.begin(total, U_FLASH)) {
    updating = false;
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, Update.errorString(), HTTPD_RESP_USE_STRLEN);
  }

  uint8_t buf[1460];
  size_t remaining = total;
  while (remaining > 0) {
    const size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
    const int got = httpd_req_recv(req, (char *)buf, want);
    if (got <= 0) {
      Update.abort();
      updating = false;
      httpd_resp_set_status(req, "400 Bad Request");
      return httpd_resp_send(req, "Upload interrupted.", HTTPD_RESP_USE_STRLEN);
    }
    if (Update.write(buf, got) != (size_t)got) {
      const String err = Update.errorString();
      Update.abort();
      updating = false;
      httpd_resp_set_status(req, "500 Internal Server Error");
      return httpd_resp_send(req, err.c_str(), HTTPD_RESP_USE_STRLEN);
    }
    remaining -= got;
  }

  if (!Update.end(true)) {
    const String err = Update.errorString();
    updating = false;
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, err.c_str(), HTTPD_RESP_USE_STRLEN);
  }

  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  if (target) {
    TrialState trial;
    trialLoad(trial);
    trial.pendingPartition = target->label;
    trial.pendingVersion = "uploaded " + String(__DATE__) + " " + __TIME__;
    trialSave(trial);
  }

  httpd_resp_send(req, "Written. Rebooting into the new firmware. If it cannot reach "
                       "the network it will revert on its own.", HTTPD_RESP_USE_STRLEN);
  Serial.println("web update complete, rebooting");
  delay(1200);
  ESP.restart();
  return ESP_OK;
}

bool startWebServers(bool cameraOk) {
  cameraAvailable = cameraOk;

  Config stored;
  configLoad(stored);
  if (!stored.cameraName.isEmpty()) cameraName = stored.cameraName;

  httpd_uri_t index   = {"/",        HTTP_GET,  indexHandler,     nullptr};
  httpd_uri_t login   = {"/login",   HTTP_GET,  loginPageHandler, nullptr};
  httpd_uri_t signin  = {"/login",   HTTP_POST, loginPostHandler, nullptr};
  httpd_uri_t logout  = {"/logout",  HTTP_GET,  logoutHandler,    nullptr};
  httpd_uri_t status  = {"/status",  HTTP_GET,  statusHandler,    nullptr};
  httpd_uri_t updpage = {"/update",  HTTP_GET,  updatePageHandler, nullptr};
  httpd_uri_t updpost = {"/update",  HTTP_POST, updatePostHandler, nullptr};
  httpd_uri_t flash   = {"/flash",   HTTP_POST, flashHandler,      nullptr};
  httpd_uri_t setpage = {"/settings", HTTP_GET,  settingsPageHandler, nullptr};
  httpd_uri_t setpost = {"/settings", HTTP_POST, settingsPostHandler, nullptr};
  httpd_uri_t nets    = {"/networks", HTTP_GET,  networksHandler,     nullptr};
  httpd_uri_t restart = {"/restart",  HTTP_GET,  restartHandler,      nullptr};
  httpd_uri_t capture = {"/capture", HTTP_GET, captureHandler, nullptr};
  httpd_uri_t stream  = {"/stream",  HTTP_GET, streamHandler,  nullptr};

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port = 32768;
  cfg.lru_purge_enable = true;
  cfg.stack_size = 8192;
  cfg.recv_wait_timeout = 30;
  cfg.send_wait_timeout = 30;

  // Defaults to 8. Exceeding it makes httpd_register_uri_handler fail quietly,
  // and the page simply 404s with nothing in the log to say why.
  cfg.max_uri_handlers = 16;
  if (httpd_start(&pageServer, &cfg) != ESP_OK) {
    Serial.println("page server failed to start");
    return false;
  }
  registerUri(pageServer, &index);
  registerUri(pageServer, &login);
  registerUri(pageServer, &signin);
  registerUri(pageServer, &logout);
  registerUri(pageServer, &status);
  registerUri(pageServer, &updpage);
  registerUri(pageServer, &updpost);
  registerUri(pageServer, &flash);
  registerUri(pageServer, &setpage);
  registerUri(pageServer, &setpost);
  registerUri(pageServer, &nets);
  registerUri(pageServer, &restart);
  registerUri(pageServer, &capture);

  // The stream gets its own server because a handler that never returns occupies
  // its server's only worker task. Sharing one server would mean the page and
  // /capture stop responding for as long as anyone is watching.
  cfg.server_port = 81;
  cfg.ctrl_port = 32769;  // must differ, or the second server refuses to start
  cfg.stack_size = 8192;
  if (httpd_start(&streamServer, &cfg) != ESP_OK) {
    Serial.println("stream server failed to start");
    return false;
  }
  registerUri(streamServer, &stream);
  return true;
}

void stopWebServers() {
  if (streamServer) {
    httpd_stop(streamServer);
    streamServer = nullptr;
  }
  if (pageServer) {
    httpd_stop(pageServer);
    pageServer = nullptr;
  }
}
