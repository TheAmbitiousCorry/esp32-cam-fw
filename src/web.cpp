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
#include "clock.h"
#include "config.h"
#include "portal.h"
#include "recording.h"
#include "storage.h"
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
static String queryParam(httpd_req_t *req, const char *key, const String &fallback);

// Inline SVG rather than an icon font or image files: nothing extra to serve,
// nothing to fetch, and they inherit the surrounding text colour.
static String icon(const char *name) {
  const char *d = nullptr;
  if (!strcmp(name, "camera"))
    d = "<path d='M2 6h3l1-2h6l1 2h3v9H2z'/><circle cx='8' cy='10' r='3'/>";
  else if (!strcmp(name, "folder"))
    d = "<path d='M2 4h4l1.5 2H14v8H2z'/>";
  else if (!strcmp(name, "gauge"))
    d = "<circle cx='8' cy='8' r='6'/><path d='M8 8l3-2.5'/>";
  else if (!strcmp(name, "cog"))
    d = "<circle cx='8' cy='8' r='2.5'/><path d='M8 1v2M8 13v2M1 8h2M13 8h2"
        "M3 3l1.5 1.5M11.5 11.5L13 13M13 3l-1.5 1.5M4.5 11.5L3 13'/>";
  else if (!strcmp(name, "chip"))
    d = "<rect x='4' y='4' width='8' height='8' rx='1'/><path d='M6 1v3M10 1v3"
        "M6 12v3M10 12v3M1 6h3M1 10h3M12 6h3M12 10h3'/>";
  else if (!strcmp(name, "exit"))
    d = "<path d='M6 2H2v12h4M10 5l3 3-3 3M13 8H6'/>";
  else if (!strcmp(name, "play"))
    d = "<path d='M5 3l8 5-8 5z'/>";
  else if (!strcmp(name, "trash"))
    d = "<path d='M3 4h10M6 4V2h4v2M4 4l1 10h6l1-10'/>";
  else if (!strcmp(name, "bolt"))
    d = "<path d='M9 1L4 9h3l-1 6 5-8H8z'/>";
  else if (!strcmp(name, "dot"))
    d = "<circle cx='8' cy='8' r='4'/>";
  else if (!strcmp(name, "image"))
    d = "<rect x='2' y='3' width='12' height='10' rx='1'/><path d='M2 11l3-3 3 3 3-3 3 3'/>";
  else if (!strcmp(name, "up"))
    d = "<path d='M8 13V3M4 7l4-4 4 4'/>";
  else
    return "";

  return String("<svg viewBox='0 0 16 16' width='15' height='15' fill='none' "
                "stroke='currentColor' stroke-width='1.4' stroke-linecap='round' "
                "stroke-linejoin='round' aria-hidden='true'>") + d + "</svg>";
}

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
    // color-scheme tells the browser to render native controls dark, which is
    // what makes a select's arrow and its option list match rather than
    // appearing as a white box on a dark page.
    "body{margin:0;background:#111;color:#eee;font:15px system-ui,sans-serif;"
    "color-scheme:dark}"
    ".app{display:flex;min-height:100vh}"
    "aside{width:180px;flex:0 0 180px;background:#181818;border-right:1px solid #262626;"
    "padding:18px 0;display:flex;flex-direction:column;gap:2px}"
    ".brand{padding:0 18px 16px;font-weight:600;font-size:15px;color:#fff;"
    "overflow-wrap:anywhere}"
    "aside a{display:flex;align-items:center;gap:10px;padding:9px 18px;color:#aaa;"
    "text-decoration:none;font-size:14px;border-left:3px solid transparent}"
    "aside a svg{flex:0 0 15px;opacity:.75}"
    "aside a.on svg{opacity:1}"
    "button svg,.btn svg{vertical-align:-2px;margin-right:6px}"
    "td svg,th svg{vertical-align:-2px;margin-right:6px;opacity:.7}"
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
    "input,select{width:100%;padding:9px;border-radius:5px;border:1px solid #444;"
    "background:#1c1c1c;color:#eee;font-size:15px;font-family:inherit}"
    "select{appearance:none;-webkit-appearance:none;padding-right:32px;"
    // A background-drawn chevron rather than an image: no asset to serve, and it
    // recolours with the rest of the theme.
    "background-image:linear-gradient(45deg,transparent 50%,#888 50%),"
    "linear-gradient(135deg,#888 50%,transparent 50%);"
    "background-position:calc(100% - 17px) 52%,calc(100% - 12px) 52%;"
    "background-size:5px 5px,5px 5px;background-repeat:no-repeat}"
    "select:focus,input:focus{outline:none;border-color:#2a7}"
    "option{background:#1c1c1c;color:#eee}"
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
  struct Item { const char *href; const char *label; const char *icon; };
  static const Item items[] = {
      {"/", "Live view", "camera"}, {"/files", "Files", "folder"},
      {"/status", "Status", "gauge"}, {"/settings", "Settings", "cog"},
      {"/update", "Firmware", "chip"}};

  String nav = "<div class=app><aside><div class=brand>" + htmlEscape(cameraName) + "</div>";
  for (const Item &it : items) {
    nav += String("<a href=\"") + it.href + "\"";
    if (strcmp(it.href, active) == 0) nav += " class=on";
    nav += ">" + icon(it.icon) + "<span>" + String(it.label) + "</span></a>";
  }
  nav += "<div class=spacer></div><a href=\"/logout\">" + icon("exit") +
         "<span>Sign out</span></a></aside><main>";
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
const rb = document.getElementById('rec');
let recPoll = null;

async function refreshRec() {
  const s = await (await fetch('/record')).json();
  if (s.active) {
    rb.textContent = 'Recording  ' + s.frames + ' frames  ' + s.fps.toFixed(1) + ' fps';
    rb.className = 'on';
  } else {
    rb.textContent = 'Record 10s';
    rb.className = '';
    clearInterval(recPoll);
    recPoll = null;
  }
}

if (rb) rb.onclick = async () => {
  await fetch('/record', {method: 'POST'});
  if (!recPoll) recPoll = setInterval(refreshRec, 1000);
  refreshRec();
};

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
  body += String("<button id=flash class=\"") + (flashIsOn() ? "on" : "") + "\">" +
          icon("bolt") + "Flash " + (flashIsOn() ? "on" : "off") + "</button>";
  body += "<a class=btn href=\"/capture\" target=_blank>" + icon("image") +
          "Still image</a>";
  body += String("<button id=rec class=\"") + (recordingActive() ? "on" : "") + "\">" +
          icon("dot") + (recordingActive() ? "Recording..." : "Record 10s") +
          "</button></div>";
  body += INDEX_BODY;
  return sendShell(req, "/", body);
}

// Toggles rather than taking a state, so the button cannot disagree with the
// device when two browsers are open on the same camera.
// Lets the button reflect what the device is doing rather than what it was told
// to do. Without it the label reads "Recording..." indefinitely.
static esp_err_t recordStateHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;
  char out[200];
  uint32_t grab = 0, write = 0, index = 0;
  recordingTiming(&grab, &write, &index);
  snprintf(out, sizeof(out),
           "{\"active\":%s,\"frames\":%lu,\"fps\":%.1f,"
           "\"grabMs\":%lu,\"writeMs\":%lu,\"indexMs\":%lu}",
           recordingActive() ? "true" : "false",
           (unsigned long)recordingFrames(), recordingFps(),
           (unsigned long)grab, (unsigned long)write, (unsigned long)index);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t recordHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;

  httpd_resp_set_type(req, "text/plain");
  if (recordingActive()) {
    recordingStop();
    return httpd_resp_send(req, "stopped", HTTPD_RESP_USE_STRLEN);
  }
  if (!recordingStart(10)) {
    return httpd_resp_send(req, "could not start: no writable card, or no camera",
                           HTTPD_RESP_USE_STRLEN);
  }
  return httpd_resp_send(req, "recording", HTTPD_RESP_USE_STRLEN);
}

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

// Replays a recording as the same multipart stream the live view uses, so an
// ordinary <img> plays it with no client-side decoding. Paced from the
// timestamps in the index, so it runs at the speed it was recorded rather than
// as fast as the card can read.
static esp_err_t playStreamHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;

  const String dir = queryParam(req, "dir", "");
  if (!sdPathIsSafe(dir) || !sdExists(dir + "/index.txt")) {
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_send(req, "no such recording", HTTPD_RESP_USE_STRLEN);
  }

  void *index = nullptr;
  void *video = nullptr;
  if (!sdIndexOpen(dir + "/index.txt", &index)) return httpd_resp_send_500(req);
  if (!sdOpenRead(dir + "/video.mjpeg", &video)) {
    sdIndexClose(index);
    return httpd_resp_send_500(req);
  }

  // One buffer for the whole replay, from PSRAM. Allocating per frame would
  // fragment the heap over a few hundred frames.
  static constexpr size_t MAX_FRAME = 200 * 1024;
  uint8_t *buf = (uint8_t *)ps_malloc(MAX_FRAME);
  if (!buf) {
    sdIndexClose(index);
    sdCloseRead(video);
    return httpd_resp_send_500(req);
  }

  esp_err_t res = httpd_resp_set_type(req, STREAM_TYPE);
  char partHeader[64];
  const uint32_t playStart = millis();
  uint32_t offset = 0, length = 0, atMs = 0;

  while (res == ESP_OK && sdIndexNext(index, &offset, &length, &atMs)) {
    if (length == 0 || length > MAX_FRAME) continue;

    // Wait until this frame is due. A gap in the recording replays as a gap.
    const int32_t due = (int32_t)(playStart + atMs - millis());
    if (due > 0) delay(due > 2000 ? 2000 : due);

    const size_t got = sdReadAt(video, offset, buf, length);
    if (got != length) break;

    const size_t hlen = snprintf(partHeader, sizeof(partHeader), STREAM_PART, got);
    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, partHeader, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)buf, got);
  }

  free(buf);
  sdIndexClose(index);
  sdCloseRead(video);
  httpd_resp_send_chunk(req, nullptr, 0);
  return res;
}

static esp_err_t playPageHandler(httpd_req_t *req) {
  if (!authGuardPage(req)) return ESP_OK;

  const String dir = queryParam(req, "dir", "");
  if (!sdPathIsSafe(dir) || !sdExists(dir + "/video.mjpeg")) {
    return sendShell(req, "/files", "<h1>Playback</h1><p class=err>No recording there.</p>");
  }

  String body = "<h1>" + htmlEscape(dir) + "</h1>";
  body += "<img id=p alt=\"recording\">";
  body += "<div class=actions><a class=btn href=\"/files?path=/rec\">Back to recordings</a>"
          "<button id=again>Replay</button></div>";
  body += "<script>"
          "const src = () => 'http://' + location.hostname + ':81/playstream?dir=" +
          htmlEscape(dir) + "&t=' + Date.now();"
          "const p = document.getElementById('p');"
          "p.src = src();"
          "document.getElementById('again').onclick = () => { p.src = src(); };"
          "</script>";
  return sendShell(req, "/files", body);
}

static esp_err_t streamHandler(httpd_req_t *req) {
  // The browser sends the session cookie with the <img> request, because cookies
  // are scoped to the host and ignore the port this server listens on.
  if (!authGuardResource(req)) return ESP_OK;

  esp_err_t res = httpd_resp_set_type(req, STREAM_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char partHeader[64];

  // While a recording runs, the recorder owns the camera and republishes each
  // frame. Grabbing here as well starves it.
  uint8_t *shared = nullptr;
  uint32_t sharedSeq = 0;

  while (true) {
    if (recordingActive()) {
      if (!shared) shared = (uint8_t *)ps_malloc(200 * 1024);
      size_t len = 0;
      if (!shared || !recordingCopyLatest(shared, 200 * 1024, &len, &sharedSeq)) {
        delay(10);
        if (updating) break;
        continue;
      }
      const size_t hlen = snprintf(partHeader, sizeof(partHeader), STREAM_PART, len);
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, partHeader, hlen);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)shared, len);
      if (res != ESP_OK || updating) break;
      continue;
    }

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
  if (shared) free(shared);
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
  row("Time", clockNow());
  row("Uptime", humanUptime());
  const bool online = WiFi.status() == WL_CONNECTED;
  row("Network", online ? WiFi.SSID() : String("disconnected"));
  row("Address", online ? WiFi.localIP().toString() : String("none"));
  row("Signal", online ? String(WiFi.RSSI()) + " dBm" : String("n/a"));
  row("Reconnects", String(reconnectTally));
  if (sdMounted()) {
    const uint64_t freeMb = (sdTotalBytes() - sdUsedBytes()) / (1024ULL * 1024ULL);
    row("SD card", sdCardType() + ", " +
                       String((uint32_t)(sdTotalBytes() / (1024ULL * 1024ULL))) +
                       " MB, " + String((uint32_t)freeMb) + " MB free" +
                       (sdWritable() ? "" : ", NOT WRITABLE"));
  } else {
    row("SD card", "not detected");
  }
  if (recordingActive()) {
    row("Recording", recordingDir() + ", " + String(recordingFrames()) + " frames, " +
                         String(recordingFps(), 1) + " fps");
  }
  row("Free heap", String(ESP.getFreeHeap()) + " bytes");
  row("Free PSRAM", String(ESP.getFreePsram()) + " bytes");
  // "app0" is an ESP-IDF partition label, which tells a reader nothing. There
  // are two slots and the useful fact is which one is executing.
  String slot = "unknown";
  if (running) {
    const String label = running->label;
    if (label.startsWith("app") && label.length() == 4) {
      slot = label.substring(3) + "/1";
    } else {
      slot = label;
    }
  }
  row("Running slot", slot);
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
  body += "</table>";

  body += "<div class=actions><a class=btn href=\"/restart\">Restart camera</a></div>";
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
  body += "<label>Timezone</label>"
          "<select id=tzlist style=\"margin-bottom:6px\">"
          "<option value=''>Choose a zone...</option>"
          "<option value='UTC0'>UTC</option>"
          "<option value='SAST-2'>South Africa</option>"
          "<option value='GMT0BST,M3.5.0/1,M10.5.0'>United Kingdom</option>"
          "<option value='CET-1CEST,M3.5.0,M10.5.0/3'>Central Europe</option>"
          "<option value='EET-2EEST,M3.5.0/3,M10.5.0/4'>Eastern Europe</option>"
          "<option value='EST5EDT,M3.2.0,M11.1.0'>US Eastern</option>"
          "<option value='CST6CDT,M3.2.0,M11.1.0'>US Central</option>"
          "<option value='PST8PDT,M3.2.0,M11.1.0'>US Pacific</option>"
          "<option value='IST-5:30'>India</option>"
          "<option value='JST-9'>Japan</option>"
          "<option value='AEST-10AEDT,M10.1.0,M4.1.0/3'>Australia Eastern</option>"
          "</select>"
          "<input name=tz id=tz value=\"" + htmlEscape(stored.timezone) + "\" "
          "placeholder='UTC0'>";
  body += "<small class=sub>A POSIX timezone string. Pick one above to fill it in, "
          "or type your own. Recordings are named from this clock.</small>";
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
const tzl = document.getElementById('tzlist');
const tzb = document.getElementById('tz');
if (tzl) tzl.onchange = () => { if (tzl.value) tzb.value = tzl.value; };

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
static esp_err_t benchHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;

  const SdBench b = sdBenchmark();
  httpd_resp_set_type(req, "text/plain");
  if (!b.ok) return httpd_resp_send(req, "benchmark failed", HTTPD_RESP_USE_STRLEN);

  const uint32_t total = b.bytesEach * b.fileCount;
  char out[400];
  snprintf(out, sizeof(out),
           "%d files of %lu bytes\n"
           "  separate files : %lu ms  (%.0f KB/s, %.1f ms per file)\n"
           "  one file       : %lu ms  (%.0f KB/s)\n"
           "  per-file cost  : %.1f ms\n",
           b.fileCount, (unsigned long)b.bytesEach,
           (unsigned long)b.manyFilesMs, total / 1024.0 / (b.manyFilesMs / 1000.0),
           (float)b.manyFilesMs / b.fileCount,
           (unsigned long)b.oneFileMs, total / 1024.0 / (b.oneFileMs / 1000.0),
           (float)(b.manyFilesMs - b.oneFileMs) / b.fileCount);
  return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

// Clears an intermittent sensor fault without a reboot, which matters once the
// camera is mounted somewhere awkward.
static esp_err_t cameraRetryHandler(httpd_req_t *req) {
  if (!authGuardPage(req)) return ESP_OK;
  const bool ok = cameraRetry();
  cameraAvailable = ok;
  return sendShell(req, "/status",
                   String("<h1>Camera</h1><p class=") + (ok ? "sub" : "err") + ">" +
                       (ok ? "Sensor detected. Live view should work now."
                           : "Sensor still not responding after three attempts.") +
                       "</p><div class=actions><a class=btn href=\"/status\">Status</a>"
                       "</div>");
}

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

// Reads a single query parameter. esp_http_server hands over the raw string and
// leaves the parsing to the handler.
static String queryParam(httpd_req_t *req, const char *key, const String &fallback) {
  const size_t len = httpd_req_get_url_query_len(req);
  if (len == 0 || len > 256) return fallback;

  char raw[257];
  if (httpd_req_get_url_query_str(req, raw, sizeof(raw)) != ESP_OK) return fallback;

  char value[257];
  if (httpd_query_key_value(raw, key, value, sizeof(value)) != ESP_OK) return fallback;
  return urlDecode(String(value));
}

static esp_err_t sendFiles(httpd_req_t *req, const String &notice) {
  String path = queryParam(req, "path", "/");
  if (!sdPathIsSafe(path)) path = "/";

  String body = "<h1>Files</h1>";
  body += "<p class=sub>" + htmlEscape(path) + "</p>";
  if (path != "/") {
    int cut = path.lastIndexOf('/');
    const String parent = cut <= 0 ? "/" : path.substring(0, cut);
    body += "<div class=actions><a class=btn href=\"/files?path=" +
            htmlEscape(parent) + "\">Up</a></div>";
  }
  if (!notice.isEmpty()) body += "<p class=sub>" + htmlEscape(notice) + "</p>";

  if (!sdMounted()) {
    body += "<p class=err>No SD card detected.</p>";
    return sendShell(req, "/files", body);
  }

  static constexpr int MAX_LISTED = 64;
  SdEntry entries[MAX_LISTED];
  int total = 0;
  const int shown = sdList(path, entries, MAX_LISTED, &total);

  const uint64_t freeMb = (sdTotalBytes() - sdUsedBytes()) / (1024ULL * 1024ULL);
  body += "<p class=sub>" + String((uint32_t)(sdTotalBytes() / (1024ULL * 1024ULL))) +
          " MB card, " + String((uint32_t)freeMb) + " MB free.</p>";

  if (total == 0) {
    body += "<p class=sub>Nothing here.</p>";
    return sendShell(req, "/files", body);
  }

  body += "<form method=post action=/files><table>";
  for (int i = 0; i < shown; i++) {
    const String size = entries[i].isDir
                            ? String("")
                            : String((uint32_t)(entries[i].size / 1024)) + " KB";
    String label = icon(entries[i].isDir ? "folder" : "image") +
                   htmlEscape(entries[i].name);
    if (entries[i].isDir) {
      label = "<a href=\"/files?path=" + htmlEscape(entries[i].path) + "\">" + label + "</a>";
    }

    String extra;
    // A directory holding a video file is a recording, so offer to play it.
    if (entries[i].isDir && sdExists(entries[i].path + "/video.mjpeg")) {
      extra = " <a class=btn style=\"padding:3px 9px\" href=\"/play?dir=" +
              htmlEscape(entries[i].path) + "\">" + icon("play") + "Play</a>";
    }

    body += "<tr><td style=\"padding-right:12px\">"
            "<input type=checkbox name=f value=\"" + htmlEscape(entries[i].path) +
            "\" style=\"width:auto\"></td>"
            "<th>" + label + "</th>"
            "<td>" + size + extra + "</td></tr>";
  }
  body += "</table>";
  if (total > shown) {
    body += "<p class=sub>" + String(total - shown) + " more not listed.</p>";
  }
  body += "<div class=actions>"
          "<button type=button id=all>Select all</button>"
          "<button type=submit id=del>" + icon("trash") +
          "Delete selected</button></div></form>";

  body += R"HTML(<script>
const boxes = () => [...document.querySelectorAll('input[name=f]')];
document.getElementById('all').onclick = () => {
  const target = !boxes().every(b => b.checked);
  boxes().forEach(b => { b.checked = target; });
};
// Not protection, just a guard against a misclick on a page that will later
// list footage rather than test files.
document.getElementById('del').onclick = e => {
  const n = boxes().filter(b => b.checked).length;
  if (n === 0) { e.preventDefault(); return; }
  if (!confirm('Delete ' + n + ' item' + (n === 1 ? '' : 's') + '? This cannot be undone.')) {
    e.preventDefault();
  }
};
</script>)HTML";
  return sendShell(req, "/files", body);
}

static esp_err_t filesPageHandler(httpd_req_t *req) {
  if (!authGuardPage(req)) return ESP_OK;
  return sendFiles(req, "");
}

static esp_err_t filesDeleteHandler(httpd_req_t *req) {
  if (!authGuardPage(req)) return ESP_OK;

  String body;
  if (!readBody(req, body, 8192)) return sendFiles(req, "Nothing to do.");

  // formField returns the first match only, and a multi-select posts one f= per
  // item, so walk the pairs directly.
  int removed = 0, failed = 0;
  int pos = 0;
  while (pos < (int)body.length()) {
    int amp = body.indexOf('&', pos);
    if (amp < 0) amp = body.length();
    const String pair = body.substring(pos, amp);
    const int eq = pair.indexOf('=');
    if (eq > 0 && urlDecode(pair.substring(0, eq)) == "f") {
      const String path = urlDecode(pair.substring(eq + 1));
      if (sdRemove(path)) {
        removed++;
      } else {
        failed++;
        Serial.printf("could not delete %s\n", path.c_str());
      }
    }
    pos = amp + 1;
  }

  String notice = String(removed) + " item" + (removed == 1 ? "" : "s") + " deleted";
  if (failed > 0) notice += ", " + String(failed) + " could not be removed";
  notice += ".";
  Serial.printf("%s\n", notice.c_str());
  return sendFiles(req, notice);
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
  stored.timezone = formField(body, "tz");
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
const fmt = b => b >= 1048576 ? (b / 1048576).toFixed(1) + ' MB'
                              : (b / 1024).toFixed(0) + ' KB';

document.getElementById('go').onclick = () => {
  const file = document.getElementById('f').files[0];
  if (!file) { msg.textContent = 'Pick a file first.'; return; }

  // XHR rather than fetch: fetch cannot report upload progress, and a silent
  // twenty second transfer is how someone decides it has hung and pulls the
  // power partway through writing flash.
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/update');
  xhr.upload.onprogress = e => {
    if (!e.lengthComputable) return;
    const pct = Math.round((e.loaded / e.total) * 100);
    msg.textContent = 'Uploading ' + fmt(e.loaded) + ' of ' + fmt(e.total) + '  (' + pct + '%)';
  };
  xhr.onload = () => { msg.textContent = xhr.responseText || 'Done.'; };
  xhr.onerror = () => { msg.textContent = 'Upload failed. The running firmware is untouched.'; };
  msg.textContent = 'Starting ' + fmt(file.size) + ' upload...';
  xhr.send(file);
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
  httpd_uri_t record  = {"/record",  HTTP_POST, recordHandler,     nullptr};
  httpd_uri_t recstate = {"/record", HTTP_GET,  recordStateHandler, nullptr};
  httpd_uri_t setpage = {"/settings", HTTP_GET,  settingsPageHandler, nullptr};
  httpd_uri_t setpost = {"/settings", HTTP_POST, settingsPostHandler, nullptr};
  httpd_uri_t nets    = {"/networks", HTTP_GET,  networksHandler,     nullptr};
  httpd_uri_t restart = {"/restart",  HTTP_GET,  restartHandler,      nullptr};
  httpd_uri_t bench   = {"/sdbench",  HTTP_GET,  benchHandler,        nullptr};
  httpd_uri_t play    = {"/play",     HTTP_GET,  playPageHandler,     nullptr};
  httpd_uri_t retrycam = {"/retrycam", HTTP_GET, cameraRetryHandler,  nullptr};
  httpd_uri_t files   = {"/files",    HTTP_GET,  filesPageHandler,    nullptr};
  httpd_uri_t filesdel = {"/files",   HTTP_POST, filesDeleteHandler,  nullptr};
  httpd_uri_t capture = {"/capture", HTTP_GET, captureHandler, nullptr};
  httpd_uri_t stream  = {"/stream",  HTTP_GET, streamHandler,  nullptr};
  httpd_uri_t replay  = {"/playstream", HTTP_GET, playStreamHandler, nullptr};

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port = 32768;
  cfg.lru_purge_enable = true;
  cfg.stack_size = 8192;
  cfg.recv_wait_timeout = 30;
  cfg.send_wait_timeout = 30;

  // Defaults to 8. Exceeding it makes httpd_register_uri_handler fail quietly,
  // and the page simply 404s with nothing in the log to say why.
  cfg.max_uri_handlers = 24;
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
  registerUri(pageServer, &record);
  registerUri(pageServer, &recstate);
  registerUri(pageServer, &setpage);
  registerUri(pageServer, &setpost);
  registerUri(pageServer, &nets);
  registerUri(pageServer, &restart);
  registerUri(pageServer, &bench);
  registerUri(pageServer, &play);
  registerUri(pageServer, &retrycam);
  registerUri(pageServer, &files);
  registerUri(pageServer, &filesdel);
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
  registerUri(streamServer, &replay);
  return true;
}

void webBeginUpdate() { updating = true; }
