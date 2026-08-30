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
#include "motion.h"
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
  else if (!strcmp(name, "down"))
    d = "<path d='M8 3v10M4 9l4 4 4-4'/>";
  else if (!strcmp(name, "zoom"))
    d = "<circle cx='7' cy='7' r='4.5'/><path d='M10.5 10.5L14 14M5 7h4M7 5v4'/>";
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
    ".zoomwrap{position:relative;overflow:hidden;max-width:100%;border-radius:6px;"
    "background:#000;touch-action:none}"
    ".zoomwrap img{display:block;transform-origin:0 0;cursor:grab;"
    "user-select:none;-webkit-user-drag:none}"
    ".zoomwrap img.dragging{cursor:grabbing}"

    ".actions{display:flex;gap:10px;flex-wrap:wrap;margin:14px 0}"
    "button,.btn{padding:9px 15px;border:1px solid #3a3a3a;border-radius:6px;"
    "background:#222;color:#eee;font:inherit;font-size:14px;cursor:pointer;"
    "text-decoration:none;display:inline-block}"
    "button:hover,.btn:hover{background:#2b2b2b;border-color:#4a4a4a}"
    "button.on{background:#2a7;border-color:#2a7;color:#04140d;font-weight:600}"
    "button.primary{background:#2a7;border-color:#2a7;color:#04140d;font-weight:600}"
    "p.sub{color:#999;font-size:13px;max-width:460px}"
    ".err{color:#f77;font-size:13px}"
    // Tooltips on the element itself rather than the title attribute: title
    // waits a second before appearing and cannot be styled. Hidden on touch,
    // where there is no hover and the label has to carry the meaning.
    "[data-tip]{position:relative}"
    "[data-tip]::after{content:attr(data-tip);position:absolute;left:50%;"
    "bottom:calc(100% + 7px);transform:translateX(-50%);white-space:nowrap;"
    "background:#000;color:#eee;border:1px solid #3a3a3a;border-radius:5px;"
    "padding:5px 8px;font-size:12px;font-weight:400;opacity:0;pointer-events:none;"
    "transition:opacity .12s .35s;z-index:5}"
    "[data-tip]:hover::after{opacity:1}"
    "@media(hover:none){[data-tip]::after{display:none}}"
    "#rec svg{transition:color .2s}"
    ".dot-rec{color:#f55}.dot-armed{color:#2a7}.dot-off{color:#777}"
    "#recstats{color:#999;font-size:13px;margin:6px 0 0;"
    "font-variant-numeric:tabular-nums;min-height:18px}"
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
struct Route {
  const char *path;
  httpd_method_t method;
  esp_err_t (*fn)(httpd_req_t *);
};

static const Route *routeTable = nullptr;
static size_t routeCount = 0;

// Matches on the path alone; a query string is arguments, not identity.
static ImageSettings imgOf(const Config &c) {
  return ImageSettings{c.aeLevel,   c.gainCeiling, c.brightness, c.contrast,
                       c.saturation, c.wbMode,     c.grayscale,  c.hmirror,
                       c.vflip};
}

static esp_err_t dispatchHandler(httpd_req_t *req) {
  String path = req->uri;
  const int q = path.indexOf('?');
  if (q >= 0) path = path.substring(0, q);

  for (size_t i = 0; i < routeCount; i++) {
    if (routeTable[i].method == req->method && path == routeTable[i].path) {
      return routeTable[i].fn(req);
    }
  }

  httpd_resp_set_status(req, "404 Not Found");
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "no such page", HTTPD_RESP_USE_STRLEN);
}

static int failedRoutes = 0;

static void registerUri(httpd_handle_t server, const httpd_uri_t *uri) {
  const esp_err_t err = httpd_register_uri_handler(server, uri);
  if (err != ESP_OK) {
    failedRoutes++;
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
      {"/", "Live view", "camera"}, {"/recording", "Recording", "dot"},
      {"/files", "Files", "folder"},
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

static const char INDEX_BODY[] = R"HTML(<div class=zoomwrap id=zw><img id="v" alt="live view"></div>
<div class=actions>
  <button type=button onclick="zoomOut()" data-tip="Zoom out">&minus;</button>
  <button type=button onclick="zoomIn()" data-tip="Zoom in. Click the picture to zoom towards that point">+</button>
  <button type=button onclick="zoomReset()" data-tip="Fit the whole frame">Fit</button>
  <span id=zlabel class=sub style="align-self:center"></span>
</div>
<script>
const rb = document.getElementById('rec');
let recPoll = null;

const dot = document.getElementById('recdot');
const pct = document.getElementById('recpct');
const stats = document.getElementById('recstats');

async function refreshRec() {
  const s = await (await fetch('/record')).json();

  // Colour carries the state; the numbers carry the detail. A sentence needed
  // reading, and the thing you want at a glance is whether it is recording.
  if (s.active) {
    dot.className = 'dot-rec';
    rb.className = 'on';
  } else if (s.motion && s.armed) {
    dot.className = 'dot-armed';
    rb.className = '';
  } else {
    dot.className = 'dot-off';
    rb.className = '';
  }

  // Change against the threshold, so the number means something without
  // remembering what was set on another page.
  pct.textContent = s.motion ? ' ' + s.change + '/' + s.threshold + '%' : ' Record';

  // Show where auto has settled, so a disabled slider still reads as working.
  const auto = document.getElementById('autoimg');
  if (auto && auto.checked && s.rung !== undefined) {
    const ael = document.getElementById('ael'), gc = document.getElementById('gc');
    if (ael) ael.value = s.ael;
    if (gc) gc.value = s.gc;
    const lab = document.getElementById('v_ael');
    if (lab) lab.textContent = s.ael + ' (auto, scene ' + s.lux + '/255)';
  }

  if (s.active) {
    stats.textContent = s.frames + ' frames  ' + s.fps.toFixed(1) + ' fps' +
                        (s.triggered ? '  motion triggered' : '  started by hand');
  } else if (!s.motion) {
    stats.textContent = 'Motion recording off';
  } else if (!s.armed) {
    stats.textContent = 'Outside schedule  ' + s.preSecs + 's history';
  } else {
    stats.textContent = 'Watching  ' + s.preSecs + 's history';
  }
}

// Always polling, not only after pressing the button: a recording that starts on
// motion should be visible without having pressed anything.
if (rb) {
  rb.onclick = async () => { await fetch('/record', {method: 'POST'}); refreshRec(); };
  refreshRec();
  setInterval(refreshRec, 2000);
}

// Applied on change: every one of these is a register write, so the next frame
// shows it. Debounced because dragging a slider fires continuously and the
// camera has better things to do.
const imgFields = ['ael','gc','flashlvl','bri','con','sat','wb'];
const imgChecks = ['autoimg','gray','hmir','vflip'];
let imgTimer = null;

// Exposure and gain belong to the loop while auto is on. Leaving them live would
// let a slider fight a control loop, and the loop always wins.
function autoOwns() {
  const on = document.getElementById('autoimg');
  for (const f of ['ael','gc']) {
    const el = document.getElementById(f);
    if (el) { el.disabled = on && on.checked; el.style.opacity = el.disabled ? '.5' : '1'; }
  }
}

function sendImage() {
  autoOwns();
  const parts = [];
  for (const f of imgFields) {
    const el = document.getElementById(f);
    if (!el) continue;
    parts.push(f + '=' + encodeURIComponent(el.value));
    const lab = document.getElementById('v_' + f);
    if (lab) lab.textContent = el.value;
  }
  for (const f of imgChecks) {
    const el = document.getElementById(f);
    if (el) parts.push(f + '=' + (el.checked ? '1' : '0'));
  }
  fetch('/image', {method: 'POST',
                   headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                   body: parts.join('&')})
    .then(r => r.text())
    .then(t => {
      // The device replies with the controls this sensor refused. Mark them, so
      // a slider that cannot do anything says why instead of feeling broken.
      if (t === 'ok') return;
      for (const name of t.split(',')) {
        const lab = document.getElementById('v_' + name);
        if (lab) lab.textContent = 'not supported by this sensor';
        const el = document.getElementById(name);
        if (el) { el.disabled = true; el.style.opacity = '.4'; }
      }
    });
}

for (const f of imgFields.concat(imgChecks)) {
  const el = document.getElementById(f);
  if (!el) continue;
  el.addEventListener('input', () => {
    const lab = document.getElementById('v_' + f);
    if (lab) lab.textContent = el.value;
    clearTimeout(imgTimer);
    imgTimer = setTimeout(sendImage, 250);
  });
}

const fb = document.getElementById('flash');
if (fb) fb.onclick = async () => {
  const state = await (await fetch('/flash', {method: 'POST'})).text();
  fb.className = state === 'on' ? 'on' : '';
};

// The stream lives on its own port, so build the URL from wherever this page was
// served rather than hardcoding an address.
document.getElementById('v').src = 'http://' + location.hostname + ':81/stream';

// Digital zoom in the browser. The device sends the same pixels either way, so
// this buys framing rather than detail; cropping at the sensor would buy detail
// and is a separate job. Free here, and it works on recordings too.
function attachZoom(wrapId, imgId, labelId) {
  const wrap = document.getElementById(wrapId);
  const img = document.getElementById(imgId);
  const label = document.getElementById(labelId);
  if (!wrap || !img) return;

  let scale = 1, tx = 0, ty = 0, dragging = false, moved = 0, lastX = 0, lastY = 0;

  // Where the buttons zoom towards. Set by clicking, so zooming in on a corner
  // does not mean zooming to the middle and then dragging to the corner.
  let focusX = null, focusY = null;
  const fx = () => (focusX === null ? wrap.clientWidth / 2 : focusX);
  const fy = () => (focusY === null ? wrap.clientHeight / 2 : focusY);

  const marker = document.createElement('div');
  marker.style.cssText = 'position:absolute;width:22px;height:22px;margin:-11px 0 0 -11px;' +
      'border:2px solid #2a7;border-radius:50%;pointer-events:none;opacity:0;' +
      'transition:opacity .4s';
  wrap.appendChild(marker);

  const showMarker = (x, y) => {
    marker.style.left = x + 'px';
    marker.style.top = y + 'px';
    marker.style.opacity = '1';
    setTimeout(() => { marker.style.opacity = '0'; }, 700);
  };

  const clamp = () => {
    // Keep the visible area inside the picture, or panning wanders off into
    // blank space and looks broken.
    const maxX = wrap.clientWidth * (scale - 1);
    const maxY = wrap.clientHeight * (scale - 1);
    tx = Math.min(0, Math.max(-maxX, tx));
    ty = Math.min(0, Math.max(-maxY, ty));
  };
  const apply = () => {
    clamp();
    img.style.transform = 'translate(' + tx + 'px,' + ty + 'px) scale(' + scale + ')';
    img.style.cursor = scale > 1 ? 'grab' : 'default';
    if (label) {
      label.textContent = scale.toFixed(1) + 'x' +
                          (scale > 1 ? '  drag to pan, double-click to fit' : '');
    }
  };

  const zoomAt = (factor, cx, cy) => {
    const next = Math.min(8, Math.max(1, scale * factor));
    // Zoom about the given point rather than the corner, so whatever is under it
    // stays under it.
    tx = cx - (cx - tx) * (next / scale);
    ty = cy - (cy - ty) * (next / scale);
    scale = next;
    if (scale === 1) { tx = 0; ty = 0; focusX = focusY = null; }
    apply();
  };

  wrap.addEventListener('wheel', e => {
    e.preventDefault();
    const r = wrap.getBoundingClientRect();
    focusX = e.clientX - r.left;
    focusY = e.clientY - r.top;
    zoomAt(e.deltaY < 0 ? 1.15 : 1 / 1.15, focusX, focusY);
  }, {passive: false});

  // Without this the browser begins its own image drag and the pointermove
  // events never arrive, so panning silently does nothing.
  img.draggable = false;
  wrap.addEventListener('dragstart', e => e.preventDefault());

  wrap.addEventListener('pointerdown', e => {
    e.preventDefault();
    dragging = true; moved = 0; lastX = e.clientX; lastY = e.clientY;
    wrap.setPointerCapture(e.pointerId);
  });
  wrap.addEventListener('pointermove', e => {
    if (!dragging) return;
    const dx = e.clientX - lastX, dy = e.clientY - lastY;
    moved += Math.abs(dx) + Math.abs(dy);
    // Only pan once zoomed in; below that there is nothing to pan to.
    if (scale > 1) {
      tx += dx; ty += dy;
      img.classList.add('dragging');
      apply();
    }
    lastX = e.clientX; lastY = e.clientY;
  });
  const end = e => {
    if (!dragging) return;
    dragging = false;
    img.classList.remove('dragging');

    // A press that did not travel is a click: set the focus there and zoom in.
    // Five pixels of slop, because a finger never holds perfectly still.
    if (moved < 5) {
      const r = wrap.getBoundingClientRect();
      focusX = e.clientX - r.left;
      focusY = e.clientY - r.top;
      showMarker(focusX, focusY);
      zoomAt(1.6, focusX, focusY);
    }
  };
  wrap.addEventListener('pointerup', end);
  wrap.addEventListener('pointercancel', () => { dragging = false; });
  wrap.addEventListener('dblclick', e => {
    e.preventDefault();
    scale = 1; tx = 0; ty = 0; focusX = focusY = null;
    apply();
  });

  window.zoomIn = () => zoomAt(1.4, fx(), fy());
  window.zoomOut = () => zoomAt(1 / 1.4, fx(), fy());
  window.zoomReset = () => { scale = 1; tx = 0; ty = 0; focusX = focusY = null; apply(); };
  apply();
}

attachZoom('zw', 'v', 'zlabel');
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
  body += String("<button id=flash data-tip=\"White LED. Brightness is under "
                 "Image adjustments\" class=\"") + (flashIsOn() ? "on" : "") + "\">" +
          icon("bolt") + "</button>";
  body += "<a class=btn href=\"/capture\" target=_blank data-tip=\"Open a full "
          "resolution still in a new tab\">" + icon("image") + "</a>";
  body += String("<button id=rec data-tip=\"Start or stop recording. The dot is "
                 "red while recording, green while watching for motion\" class=\"") +
          (recordingActive() ? "on" : "") + "\">" +
          "<span id=recdot class=dot-off>" + icon("dot") + "</span>"
          "<span id=recpct></span></button></div>";
  body += "<p id=recstats></p>";

  {
    Config c;
    configLoad(c);
    auto range = [&body](const char *name, const char *label, int lo, int hi, int val) {
      body += "<label style=\"margin:10px 0 2px\">" + String(label) +
              " <span class=sub id=v_" + name + ">" + String(val) + "</span></label>"
              "<input type=range name=" + String(name) + " id=" + String(name) +
              " min=" + String(lo) + " max=" + String(hi) + " value=" + String(val) + ">";
    };
    auto check = [&body](const char *name, const char *label, bool on) {
      body += String("<label style=\"display:flex;gap:6px;align-items:center;margin:8px 0\">"
                     "<input type=checkbox id=") + name + " name=" + name +
              (on ? " checked" : "") + " style=\"width:auto\">" + label + "</label>";
    };

    body += "<details style=\"max-width:340px;margin-top:8px\">"
            "<summary class=sub style=\"cursor:pointer\">Image adjustments</summary>";
    check("autoimg", "Auto exposure", c.autoImage);
    body += "<small class=sub>Reads the scene every few seconds and sets exposure "
            "and gain to suit it. Turn it off to hold a value by hand.</small>";
    range("ael", "Exposure", -2, 2, c.aeLevel);
    body += "<small class=sub>Raise it for scenes with a bright window behind the "
            "subject.</small>";
    body += "<label style=\"margin:10px 0 2px\">Gain limit</label>"
            "<select id=gc name=gc>";
    static const char *GAINS[] = {"2x", "4x", "8x", "16x", "32x", "64x", "128x"};
    for (int i = 0; i < 7; i++) {
      body += String("<option value=") + i + (c.gainCeiling == i ? " selected" : "") +
              ">" + GAINS[i] + "</option>";
    }
    body += "</select><small class=sub>Lower means darker but cleaner. Noise in the "
            "dark is what makes motion detection fire at nothing.</small>";
    range("flashlvl", "Flash brightness", 0, 255, c.flashLevel);
    range("bri", "Brightness", -2, 2, c.brightness);
    range("con", "Contrast", -2, 2, c.contrast);
    range("sat", "Saturation", -2, 2, c.saturation);
    body += "<label style=\"margin:10px 0 2px\">White balance</label>"
            "<select id=wb name=wb>";
    static const char *WB[] = {"Auto", "Sunny", "Cloudy", "Office", "Home"};
    for (int i = 0; i < 5; i++) {
      body += String("<option value=") + i + (c.wbMode == i ? " selected" : "") +
              ">" + WB[i] + "</option>";
    }
    body += "</select>";
    check("gray", "Grayscale", c.grayscale);
    check("hmir", "Mirror horizontally", c.hmirror);
    check("vflip", "Flip vertically", c.vflip);
    body += "</details>";
  }
  body += INDEX_BODY;
  return sendShell(req, "/", body);
}

// Toggles rather than taking a state, so the button cannot disagree with the
// device when two browsers are open on the same camera.
// Lets the button reflect what the device is doing rather than what it was told
// to do. Without it the label reads "Recording..." indefinitely.
static esp_err_t recordStateHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;
  char out[320];
  uint32_t grab = 0, write = 0, index = 0;
  recordingTiming(&grab, &write, &index);
  Config c;
  configLoad(c);
  snprintf(out, sizeof(out),
           "{\"active\":%s,\"frames\":%lu,\"fps\":%.1f,"
           "\"grabMs\":%lu,\"writeMs\":%lu,\"indexMs\":%lu,"
           "\"triggered\":%s,\"motion\":%s,\"armed\":%s,\"change\":%u,"
           "\"threshold\":%u,\"preFrames\":%lu,\"preSecs\":%lu,"
           "\"lux\":%u,\"rung\":%u,\"ael\":%d,\"gc\":%u}",
           recordingActive() ? "true" : "false",
           (unsigned long)recordingFrames(), recordingFps(),
           (unsigned long)grab, (unsigned long)write, (unsigned long)index,
           recordingWasTriggered() ? "true" : "false",
           c.motionEnabled ? "true" : "false",
           motionArmed() ? "true" : "false", motionLastChange(),
           c.motionSensitivity,
           (unsigned long)prerollFrames(), (unsigned long)prerollSeconds(),
           motionBrightness(), cameraAutoPosition(cameraCurrentImage()),
           (int)cameraCurrentImage().aeLevel,
           (unsigned)cameraCurrentImage().gainCeiling);
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

// Every value here is a sensor register, so applying on change is honest: the
// next frame shows the result. Saved at the same time, since a control you have
// to remember to save is a control people leave wrong.
static esp_err_t imageHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;

  String body;
  if (!readBody(req, body, 512)) return httpd_resp_send_500(req);

  Config c;
  if (!configLoad(c)) return httpd_resp_send_500(req);

  auto clampi = [](const String &v, int lo, int hi, int fallback) {
    if (v.isEmpty()) return fallback;
    const int n = v.toInt();
    return n < lo ? lo : (n > hi ? hi : n);
  };

  c.autoImage = formField(body, "autoimg") == "1";
  if (!c.autoImage) {
    // Only accept these while they are the user's to set. Under auto the loop owns
    // them, and a form posted from a page that loaded minutes ago would otherwise
    // drag the exposure back to whatever it looked like then.
    c.aeLevel = (int8_t)clampi(formField(body, "ael"), -2, 2, c.aeLevel);
    c.gainCeiling = (uint8_t)clampi(formField(body, "gc"), 0, 6, c.gainCeiling);
  } else {
    c.aeLevel = cameraCurrentImage().aeLevel;
    c.gainCeiling = cameraCurrentImage().gainCeiling;
  }
  c.brightness = (int8_t)clampi(formField(body, "bri"), -2, 2, c.brightness);
  c.contrast = (int8_t)clampi(formField(body, "con"), -2, 2, c.contrast);
  c.saturation = (int8_t)clampi(formField(body, "sat"), -2, 2, c.saturation);
  c.wbMode = (uint8_t)clampi(formField(body, "wb"), 0, 4, c.wbMode);
  c.flashLevel = (uint8_t)clampi(formField(body, "flashlvl"), 0, 255, c.flashLevel);
  c.grayscale = formField(body, "gray") == "1";
  c.hmirror = formField(body, "hmir") == "1";
  c.vflip = formField(body, "vflip") == "1";

  cameraApplyImage(imgOf(c));
  flashSetLevel(c.flashLevel);
  configSave(c);

  // The page marks whatever came back as refused, so a control that does nothing
  // says so rather than looking broken.
  const String refused = cameraUnsupported();
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, refused.isEmpty() ? "ok" : refused.c_str(),
                         HTTPD_RESP_USE_STRLEN);
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

  const long fromFrame = queryParam(req, "from", "0").toInt();

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
  uint32_t offset = 0, length = 0, atMs = 0;

  // Skip to the starting frame before the clock starts, so resuming from the
  // middle does not replay the skipped time as a pause.
  uint32_t baseMs = 0;
  for (long i = 0; i < fromFrame; i++) {
    if (!sdIndexNext(index, &offset, &length, &atMs)) break;
    baseMs = atMs;
  }
  const uint32_t playStart = millis();

  while (res == ESP_OK && sdIndexNext(index, &offset, &length, &atMs)) {
    if (length == 0 || length > MAX_FRAME) continue;

    // Wait until this frame is due. A gap in the recording replays as a gap.
    const int32_t due = (int32_t)(playStart + (atMs - baseMs) - millis());
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

// The index as JSON, so the player can seek. Timestamps come with it, which is
// what lets playback run at the speed it was recorded rather than a fixed rate.
static esp_err_t recIndexHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;

  const String dir = queryParam(req, "dir", "");
  if (!sdPathIsSafe(dir) || !sdExists(dir + "/index.txt")) {
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
  }

  void *index = nullptr;
  if (!sdIndexOpen(dir + "/index.txt", &index)) return httpd_resp_send_500(req);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send_chunk(req, "[", 1);

  uint32_t offset = 0, length = 0, atMs = 0;
  bool first = true;
  char piece[48];
  while (sdIndexNext(index, &offset, &length, &atMs)) {
    const int n = snprintf(piece, sizeof(piece), "%s[%lu,%lu,%lu]", first ? "" : ",",
                           (unsigned long)atMs, (unsigned long)offset,
                           (unsigned long)length);
    httpd_resp_send_chunk(req, piece, n);
    first = false;
  }
  sdIndexClose(index);

  httpd_resp_send_chunk(req, "]", 1);
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

// Reads a frame at the offset the client supplies. The client has the index
// already, so making the server walk it again cost 360ms for the first frame and
// 1384ms for the hundredth, which is what made scrubbing feel stuck.
static esp_err_t frameHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;

  const String dir = queryParam(req, "dir", "");
  const long offset = queryParam(req, "off", "-1").toInt();
  const long length = queryParam(req, "len", "0").toInt();

  if (!sdPathIsSafe(dir) || offset < 0 || length <= 0 || length > 200 * 1024 ||
      !sdExists(dir + "/video.mjpeg")) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "bad frame request", HTTPD_RESP_USE_STRLEN);
  }

  void *video = nullptr;
  if (!sdOpenRead(dir + "/video.mjpeg", &video)) return httpd_resp_send_500(req);

  uint8_t *buf = (uint8_t *)ps_malloc(length);
  if (!buf) {
    sdCloseRead(video);
    return httpd_resp_send_500(req);
  }
  const size_t got = sdReadAt(video, (uint32_t)offset, buf, (size_t)length);
  sdCloseRead(video);

  if (got == 0) {
    free(buf);
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_send(req, "no frame there", HTTPD_RESP_USE_STRLEN);
  }

  httpd_resp_set_type(req, "image/jpeg");
  const esp_err_t res = httpd_resp_send(req, (const char *)buf, got);
  free(buf);
  return res;
}

static String parentOf(const String &path) {
  const int cut = path.lastIndexOf('/');
  return cut <= 0 ? "/" : path.substring(0, cut);
}

static esp_err_t playPageHandler(httpd_req_t *req) {
  if (!authGuardPage(req)) return ESP_OK;

  const String dir = queryParam(req, "dir", "");
  if (!sdPathIsSafe(dir) || !sdExists(dir + "/video.mjpeg")) {
    return sendShell(req, "/files", "<h1>Playback</h1><p class=err>No recording there.</p>");
  }

  String body = "<h1>" + htmlEscape(dir) + "</h1>";
  body += "<div class=zoomwrap id=zw><img id=p alt=\"recording\"></div>";
  body += "<input type=range id=scrub min=0 max=0 value=0 style=\"margin:14px 0\">";
  body += "<div class=actions>"
          "<button type=button onclick=\"zoomOut()\" data-tip=\"Zoom out\">&minus;</button>"
          "<button type=button onclick=\"zoomIn()\" data-tip=\"Zoom in. Click the "
          "picture to zoom towards that point\">+</button>"
          "<button type=button onclick=\"zoomReset()\" data-tip=\"Fit the whole "
          "frame\">" + icon("zoom") + "</button>"
          "<span id=zlabel class=sub style=\"align-self:center\"></span></div>";
  body += "<div class=actions>"
          "<button id=pp class=primary>" + icon("play") + "Play</button>"
          "<span id=pos class=sub style=\"align-self:center\"></span>"
          "<a class=btn href=\"/files?path=" + htmlEscape(parentOf(dir)) +
          "\">" + icon("up") + "Back</a></div>";
  body += "<script>const DIR='" + dir + "';</script>";
  body += R"HTML(<script>
const img = document.getElementById('p');
const bar = document.getElementById('scrub');
const pos = document.getElementById('pos');
const pp  = document.getElementById('pp');

// Each entry is [timestampMs, byteOffset, byteLength].
let idx = [], at = 0, timer = null, playing = false;

// Two ways to show a frame, because they are good at different things. Playing
// uses one long-lived multipart connection, which runs at the speed it was
// recorded. Scrubbing fetches single frames, which can jump anywhere but costs a
// round trip each, so it tops out near seven a second.
let inFlight = false, wanted = null;

const label = n =>
  pos.textContent = (idx[n][0] / 1000).toFixed(1) + 's  /  ' +
                    (idx[idx.length - 1][0] / 1000).toFixed(1) + 's   frame ' +
                    (n + 1) + ' of ' + idx.length;

const loadFrame = n => {
  wanted = n;
  if (inFlight) return;
  inFlight = true;
  const target = wanted;
  wanted = null;
  img.src = '/frame?dir=' + encodeURIComponent(DIR) +
            '&off=' + idx[target][1] + '&len=' + idx[target][2];
  label(target);
};

const settled = () => {
  if (playing) return;
  inFlight = false;
  if (wanted !== null) loadFrame(wanted);
};
img.onload = settled;
img.onerror = settled;

const show = n => {
  at = Math.max(0, Math.min(n, idx.length - 1));
  bar.value = at;
  loadFrame(at);
};

const stopPlaying = () => {
  playing = false;
  clearInterval(timer);
  timer = null;
  pp.textContent = 'Play';
  inFlight = false;
  loadFrame(at);           // back to a still at wherever playback reached
};

const startPlaying = () => {
  if (at >= idx.length - 1) at = 0;
  playing = true;
  pp.textContent = 'Pause';

  const startFrame = at;
  const startedAt = Date.now();
  img.src = 'http://' + location.hostname + ':81/playstream?dir=' +
            encodeURIComponent(DIR) + '&from=' + startFrame + '&t=' + startedAt;

  // The slider is advanced from the index rather than from the stream, which
  // does not report where it has reached. Same timestamps, so it tracks.
  timer = setInterval(() => {
    const elapsed = Date.now() - startedAt + idx[startFrame][0];
    let n = at;
    while (n < idx.length - 1 && idx[n + 1][0] <= elapsed) n++;
    at = n;
    bar.value = at;
    label(at);
    if (at >= idx.length - 1) stopPlaying();
  }, 100);
};

pp.onclick = () => (playing ? stopPlaying() : startPlaying());
bar.oninput = () => { if (playing) stopPlaying(); show(+bar.value); };


// Digital zoom in the browser. The device sends the same pixels either way, so
// this buys framing rather than detail; cropping at the sensor would buy detail
// and is a separate job. Free here, and it works on recordings too.
function attachZoom(wrapId, imgId, labelId) {
  const wrap = document.getElementById(wrapId);
  const img = document.getElementById(imgId);
  const label = document.getElementById(labelId);
  if (!wrap || !img) return;

  let scale = 1, tx = 0, ty = 0, dragging = false, moved = 0, lastX = 0, lastY = 0;

  // Where the buttons zoom towards. Set by clicking, so zooming in on a corner
  // does not mean zooming to the middle and then dragging to the corner.
  let focusX = null, focusY = null;
  const fx = () => (focusX === null ? wrap.clientWidth / 2 : focusX);
  const fy = () => (focusY === null ? wrap.clientHeight / 2 : focusY);

  const marker = document.createElement('div');
  marker.style.cssText = 'position:absolute;width:22px;height:22px;margin:-11px 0 0 -11px;' +
      'border:2px solid #2a7;border-radius:50%;pointer-events:none;opacity:0;' +
      'transition:opacity .4s';
  wrap.appendChild(marker);

  const showMarker = (x, y) => {
    marker.style.left = x + 'px';
    marker.style.top = y + 'px';
    marker.style.opacity = '1';
    setTimeout(() => { marker.style.opacity = '0'; }, 700);
  };

  const clamp = () => {
    // Keep the visible area inside the picture, or panning wanders off into
    // blank space and looks broken.
    const maxX = wrap.clientWidth * (scale - 1);
    const maxY = wrap.clientHeight * (scale - 1);
    tx = Math.min(0, Math.max(-maxX, tx));
    ty = Math.min(0, Math.max(-maxY, ty));
  };
  const apply = () => {
    clamp();
    img.style.transform = 'translate(' + tx + 'px,' + ty + 'px) scale(' + scale + ')';
    img.style.cursor = scale > 1 ? 'grab' : 'default';
    if (label) {
      label.textContent = scale.toFixed(1) + 'x' +
                          (scale > 1 ? '  drag to pan, double-click to fit' : '');
    }
  };

  const zoomAt = (factor, cx, cy) => {
    const next = Math.min(8, Math.max(1, scale * factor));
    // Zoom about the given point rather than the corner, so whatever is under it
    // stays under it.
    tx = cx - (cx - tx) * (next / scale);
    ty = cy - (cy - ty) * (next / scale);
    scale = next;
    if (scale === 1) { tx = 0; ty = 0; focusX = focusY = null; }
    apply();
  };

  wrap.addEventListener('wheel', e => {
    e.preventDefault();
    const r = wrap.getBoundingClientRect();
    focusX = e.clientX - r.left;
    focusY = e.clientY - r.top;
    zoomAt(e.deltaY < 0 ? 1.15 : 1 / 1.15, focusX, focusY);
  }, {passive: false});

  // Without this the browser begins its own image drag and the pointermove
  // events never arrive, so panning silently does nothing.
  img.draggable = false;
  wrap.addEventListener('dragstart', e => e.preventDefault());

  wrap.addEventListener('pointerdown', e => {
    e.preventDefault();
    dragging = true; moved = 0; lastX = e.clientX; lastY = e.clientY;
    wrap.setPointerCapture(e.pointerId);
  });
  wrap.addEventListener('pointermove', e => {
    if (!dragging) return;
    const dx = e.clientX - lastX, dy = e.clientY - lastY;
    moved += Math.abs(dx) + Math.abs(dy);
    // Only pan once zoomed in; below that there is nothing to pan to.
    if (scale > 1) {
      tx += dx; ty += dy;
      img.classList.add('dragging');
      apply();
    }
    lastX = e.clientX; lastY = e.clientY;
  });
  const end = e => {
    if (!dragging) return;
    dragging = false;
    img.classList.remove('dragging');

    // A press that did not travel is a click: set the focus there and zoom in.
    // Five pixels of slop, because a finger never holds perfectly still.
    if (moved < 5) {
      const r = wrap.getBoundingClientRect();
      focusX = e.clientX - r.left;
      focusY = e.clientY - r.top;
      showMarker(focusX, focusY);
      zoomAt(1.6, focusX, focusY);
    }
  };
  wrap.addEventListener('pointerup', end);
  wrap.addEventListener('pointercancel', () => { dragging = false; });
  wrap.addEventListener('dblclick', e => {
    e.preventDefault();
    scale = 1; tx = 0; ty = 0; focusX = focusY = null;
    apply();
  });

  window.zoomIn = () => zoomAt(1.4, fx(), fy());
  window.zoomOut = () => zoomAt(1 / 1.4, fx(), fy());
  window.zoomReset = () => { scale = 1; tx = 0; ty = 0; focusX = focusY = null; apply(); };
  apply();
}

attachZoom('zw', 'p', 'zlabel');

(async () => {
  idx = await (await fetch('/recindex?dir=' + encodeURIComponent(DIR))).json();
  if (!idx.length) { pos.textContent = 'No frames.'; return; }
  bar.max = idx.length - 1;
  show(0);
})();
</script>)HTML";
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

    // Hand the frame to history and detection before sending it. They then need
    // no camera access of their own for as long as anyone is watching.
    motionObserve(fb);

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
  {
    Config c;
    configLoad(c);
    row("Motion trigger", c.motionEnabled
                              ? "on at " + String(c.motionSensitivity) + "%, last frame " +
                                    String(motionLastChange()) + "%"
                              : "off");
  }
  if (failedRoutes > 0) {
    row("Routes", String(failedRoutes) + " FAILED TO REGISTER");
  }
  row("Free heap", String(ESP.getFreeHeap()) + " bytes");
  row("Free PSRAM", String(ESP.getFreePsram()) + " bytes");


  Config stored;
  if (configLoad(stored)) {
    row("Setup access point", apWindowOpen()
                                ? "open, " + String(apWindowSecondsLeft() / 60) +
                                      " min left"
                                : "closed");
  row("Reset presses", String(bootPress) + " of " + String(bootPressNeeded) + " at last boot");
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

// Streams any file off the card in chunks. Recordings run to tens of megabytes,
// so the whole file never exists in memory at once.
static esp_err_t downloadHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;

  const String path = queryParam(req, "path", "");
  if (!sdPathIsSafe(path) || !sdExists(path)) {
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_send(req, "no such file", HTTPD_RESP_USE_STRLEN);
  }

  void *fh = nullptr;
  if (!sdOpenRead(path, &fh)) return httpd_resp_send_500(req);

  static constexpr size_t CHUNK = 8192;
  uint8_t *buf = (uint8_t *)ps_malloc(CHUNK);
  if (!buf) {
    sdCloseRead(fh);
    return httpd_resp_send_500(req);
  }

  // The name after the last slash, so a browser saves it as something meaningful
  // rather than as the query string.
  const int cut = path.lastIndexOf('/');
  const String name = cut >= 0 ? path.substring(cut + 1) : path;
  const String disp = "attachment; filename=\"" + name + "\"";

  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());

  esp_err_t res = ESP_OK;
  while (res == ESP_OK) {
    const size_t got = sdReadNext(fh, buf, CHUNK);
    if (got == 0) break;
    res = httpd_resp_send_chunk(req, (const char *)buf, got);
  }
  free(buf);
  sdCloseRead(fh);
  httpd_resp_send_chunk(req, nullptr, 0);
  return res;
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
  const int startAt = max(0L, queryParam(req, "start", "0").toInt());

  // /rec holds a directory per day; a day holds recordings. Both are rendered
  // with the punctuation put back, since the names are stored without it.
  const bool atRecRoot = (path == "/rec");
  const bool inDay = path.startsWith("/rec/") && path.length() == 15;

  String body = "<h1>Files</h1>";
  if (!notice.isEmpty()) body += "<p class=sub>" + htmlEscape(notice) + "</p>";
  body += "<p class=sub>" + htmlEscape(path) + "</p>";

  if (!sdMounted()) {
    body += "<p class=err>No SD card detected.</p>";
    return sendShell(req, "/files", body);
  }

  String nav;
  if (path != "/") {
    int cut = path.lastIndexOf('/');
    const String parent = cut <= 0 ? "/" : path.substring(0, cut);
    nav += "<a class=btn href=\"/files?path=" + htmlEscape(parent) + "\">" +
           icon("up") + "Up</a>";
  }
  if (!nav.isEmpty()) body += "<div class=actions>" + nav + "</div>";

  // Jumping to a date is navigation, not filtering: days are directories.
  if (atRecRoot) {
    body += "<form method=get action=/files class=actions>"
            "<input type=hidden name=path value=/rec>"
            "<input type=date id=day style=\"width:auto\">"
            "<button type=button id=go>Go to date</button></form>"
            "<script>document.getElementById('go').onclick = () => {"
            "const d = document.getElementById('day').value;"
            "if (d) location = '/files?path=/rec/' + d; };</script>";
  }

  static constexpr int PAGE = 50;
  SdEntry entries[PAGE];
  int total = 0;
  const int shown = sdList(path, entries, PAGE, &total, startAt);

  if (total == 0) {
    body += "<p class=sub>Nothing here.</p>";
    return sendShell(req, "/files", body);
  }

  body += "<form method=post action=/files><table>";
  for (int i = 0; i < shown; i++) {
    String label = entries[i].name;
    String action;

    if (inDay && label.length() == 6) {
      // 041541 is a time of day once the colons are put back.
      label = label.substring(0, 2) + ":" + label.substring(2, 4) + ":" +
              label.substring(4, 6);
    }

    if (entries[i].isDir && sdExists(entries[i].path + "/video.mjpeg")) {
      // "<frames> <milliseconds> <bytes>", written when the recording stopped.
      const String meta = sdReadSmall(entries[i].path + "/meta.txt", 96);
      if (meta.length() > 2) {
        const int sp1 = meta.indexOf(' ');
        const int sp2 = meta.indexOf(' ', sp1 + 1);
        const long durMs = meta.substring(sp1 + 1, sp2).toInt();
        const long kb = meta.substring(sp2 + 1).toInt() / 1024;

        String ends;
        // Start comes from the directory name; the end is start plus duration.
        if (entries[i].name.length() == 6 && durMs > 0) {
          const int h = entries[i].name.substring(0, 2).toInt();
          const int m = entries[i].name.substring(2, 4).toInt();
          const int sec = entries[i].name.substring(4, 6).toInt();
          long endSec = h * 3600L + m * 60L + sec + (durMs + 500) / 1000;
          endSec %= 86400L;
          char buf[16];
          snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld", endSec / 3600,
                   (endSec % 3600) / 60, endSec % 60);
          ends = String(" to ") + buf;
        }

        action = "<span class=sub>" + String(durMs / 1000.0, 1) + "s" + ends +
                 ", " + String(kb) + " KB</span> ";
      }
      action += "<a class=btn style=\"padding:3px 9px\" href=\"/play?dir=" +
                htmlEscape(entries[i].path) + "\">" + icon("play") + "Play</a>";
    }

    String cell = icon(entries[i].isDir ? "folder" : "image") + htmlEscape(label);
    if (entries[i].isDir) {
      cell = "<a href=\"/files?path=" + htmlEscape(entries[i].path) + "\">" + cell + "</a>";
    }

    String size;
    if (entries[i].isDir) {
      size = action;
    } else {
      size = String((uint32_t)(entries[i].size / 1024)) + " KB "
             "<a class=btn style=\"padding:3px 9px\" data-tip=\"Download this "
             "file\" href=\"/download?path=" + htmlEscape(entries[i].path) +
             "\">" + icon("down") + "</a>";
    }

    body += "<tr><td style=\"padding-right:12px\">"
            "<input type=checkbox name=f value=\"" + htmlEscape(entries[i].path) +
            "\" style=\"width:auto\"></td>"
            "<th>" + cell + "</th><td>" + size + "</td></tr>";
  }
  body += "</table>";

  // Paging rather than a silent cap. A truncated list that does not say so reads
  // as missing footage.
  if (total > PAGE) {
    body += "<div class=actions>";
    if (startAt > 0) {
      body += "<a class=btn href=\"/files?path=" + htmlEscape(path) + "&start=" +
              String(max(0, startAt - PAGE)) + "\">Previous</a>";
    }
    if (startAt + shown < total) {
      body += "<a class=btn href=\"/files?path=" + htmlEscape(path) + "&start=" +
              String(startAt + PAGE) + "\">Next</a>";
    }
    body += "<span class=sub style=\"align-self:center\">" + String(startAt + 1) +
            " to " + String(startAt + shown) + " of " + String(total) + "</span></div>";
  }

  body += "<div class=actions>"
          "<button type=button id=all>Select all</button>"
          "<button type=submit id=del>" + icon("trash") + "Delete selected</button>"
          "</div></form>";

  body += R"HTML(<script>
const boxes = () => [...document.querySelectorAll('input[name=f]')];
document.getElementById('all').onclick = () => {
  const target = !boxes().every(b => b.checked);
  boxes().forEach(b => { b.checked = target; });
};
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

static esp_err_t sendRecordings(httpd_req_t *req, const String &notice) {
  Config stored;
  configLoad(stored);

  String body = "<h1>Recording</h1>";
  if (!notice.isEmpty()) body += "<p class=sub>" + htmlEscape(notice) + "</p>";

  body += "<form method=post action=/recording style=\"max-width:340px\">";
  body += String("<label><input type=checkbox name=moten value=1 style=\"width:auto\"") +
          (stored.motionEnabled ? " checked" : "") +
          "> Record automatically when the scene changes</label>";
  body += "<label>Sensitivity, percent of the scene</label>"
          "<input type=number name=motsens min=1 max=100 value=" +
          String(stored.motionSensitivity) + ">";
  body += "<small class=sub>Lower triggers more easily. Right now the camera sees "
          "<b>" + String(motionLastChange()) + "%</b> changing between frames. "
          "Watch that with the scene still, then while something moves, and pick a "
          "number between the two.</small>";
  body += "<label>Minimum recording length, seconds</label>"
          "<input type=number name=recsec min=2 max=120 value=" +
          String(stored.recordSeconds) + ">";
  body += "<label>Seconds of history to keep before a trigger</label>"
          "<input type=number name=presec min=0 max=20 value=" +
          String(stored.prerollSeconds) + ">";
  body += "<label>Seconds of stillness before a recording ends</label>"
          "<input type=number name=quietsec min=1 max=60 value=" +
          String(stored.quietSeconds) + ">";
  body += "<small class=sub>A motion recording runs at least the minimum, then "
          "keeps going while the scene keeps changing. History is limited by "
          "memory as well as by this number: larger frames buy fewer seconds."
          "</small>";
  body += "<h2 style=\"margin-top:24px\">When</h2>";
  body += String("<label><input type=checkbox name=schen value=1 style=\"width:auto\"") +
          (stored.scheduleEnabled ? " checked" : "") +
          "> Only record during these hours</label>";
  body += "<div class=actions>"
          "<input type=number name=schfrom min=0 max=23 value=" +
          String(stored.scheduleFromHour) + " style=\"width:70px\">"
          "<span class=sub style=\"align-self:center\">to</span>"
          "<input type=number name=schto min=0 max=23 value=" +
          String(stored.scheduleToHour) + " style=\"width:70px\">"
          "<span class=sub style=\"align-self:center\">o'clock</span></div>";
  body += "<small class=sub>A start later than the end crosses midnight, so 22 to "
          "6 means overnight. Equal values mean all day.</small>";

  static const char *DAY_NAMES[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  body += "<div class=actions style=\"gap:14px\">";
  for (int d = 0; d < 7; d++) {
    body += String("<label style=\"margin:0;display:flex;gap:5px;align-items:center\">"
                   "<input type=checkbox name=schday value=") + d +
            (stored.scheduleDays & (1 << d) ? " checked" : "") +
            " style=\"width:auto\">" + DAY_NAMES[d] + "</label>";
  }
  body += "</div>";

  body += "<h2 style=\"margin-top:24px\">Image</h2>";
  // Symbols, not numbers: framesize_t values move between SDK versions, and
  // writing them out by hand mislabelled every entry in this list once already.
  struct Size { int value; const char *label; };
  static const Size SIZES[] = {
      {FRAMESIZE_QVGA, "320x240"},   {FRAMESIZE_CIF, "400x296"},
      {FRAMESIZE_VGA, "640x480"},    {FRAMESIZE_SVGA, "800x600"},
      {FRAMESIZE_XGA, "1024x768"},   {FRAMESIZE_SXGA, "1280x1024"},
      {FRAMESIZE_UXGA, "1600x1200"}};
  body += "<label>Frame size</label><select name=fsize>";
  for (const Size &s : SIZES) {
    body += String("<option value=") + s.value +
            (stored.frameSize == s.value ? " selected" : "") + ">" + s.label + "</option>";
  }
  body += "</select>";
  body += "<label>JPEG quality</label>"
          "<input type=number name=jq min=10 max=63 value=" + String(stored.jpegQuality) + ">";
  body += "<small class=sub>10 is best and largest, 63 is worst and smallest. "
          "Bigger frames and better quality both cost card space and Wi-Fi: "
          "playback is limited by the link before it is limited by the device."
          "</small>";

  body += "<h2 style=\"margin-top:24px\">Storage</h2>";
  body += "<label>Keep this much free, MB</label>"
          "<input type=number name=keepfree min=0 max=32000 value=" +
          String(stored.keepFreeMb) + ">";
  body += "<small class=sub>Oldest recordings are deleted to stay above this "
          "before a new one starts. Zero never deletes anything, and the card "
          "eventually fills.</small>";

  body += "<div class=actions><button type=submit class=primary>Save</button>"
          "<a class=btn href=\"/files?path=/rec\">" + icon("folder") +
          "Saved recordings</a></div></form>";

  return sendShell(req, "/recording", body);
}

static esp_err_t recordingsPageHandler(httpd_req_t *req) {
  if (!authGuardPage(req)) return ESP_OK;
  return sendRecordings(req, "");
}

static esp_err_t recordingsPostHandler(httpd_req_t *req) {
  if (!authGuardPage(req)) return ESP_OK;

  String body;
  if (!readBody(req, body)) return sendRecordings(req, "Bad request.");

  Config stored;
  if (!configLoad(stored)) return sendRecordings(req, "No stored configuration.");

  stored.motionEnabled = !formField(body, "moten").isEmpty();
  const int sens = formField(body, "motsens").toInt();
  if (sens >= 1 && sens <= 100) stored.motionSensitivity = (uint8_t)sens;
  const int secs = formField(body, "recsec").toInt();
  if (secs >= 2 && secs <= 120) stored.recordSeconds = (uint8_t)secs;
  const int pre = formField(body, "presec").toInt();
  if (pre >= 0 && pre <= 20) stored.prerollSeconds = (uint8_t)pre;
  const int quiet = formField(body, "quietsec").toInt();
  if (quiet >= 1 && quiet <= 60) stored.quietSeconds = (uint8_t)quiet;

  stored.scheduleEnabled = !formField(body, "schen").isEmpty();
  const int fromH = formField(body, "schfrom").toInt();
  const int toH = formField(body, "schto").toInt();
  if (fromH >= 0 && fromH <= 23) stored.scheduleFromHour = (uint8_t)fromH;
  if (toH >= 0 && toH <= 23) stored.scheduleToHour = (uint8_t)toH;

  // One checkbox per day posts one value per checked day, so the mask is built
  // by walking the pairs rather than reading a single field.
  uint8_t days = 0;
  int pos = 0;
  while (pos < (int)body.length()) {
    int amp = body.indexOf('&', pos);
    if (amp < 0) amp = body.length();
    const String pair = body.substring(pos, amp);
    const int eq = pair.indexOf('=');
    if (eq > 0 && urlDecode(pair.substring(0, eq)) == "schday") {
      const int d = urlDecode(pair.substring(eq + 1)).toInt();
      if (d >= 0 && d <= 6) days |= (1 << d);
    }
    pos = amp + 1;
  }
  stored.scheduleDays = days;

  const int fsize = formField(body, "fsize").toInt();
  if (fsize >= 0 && fsize <= (int)FRAMESIZE_UXGA) stored.frameSize = (uint8_t)fsize;
  const int jq = formField(body, "jq").toInt();
  if (jq >= 10 && jq <= 63) stored.jpegQuality = (uint8_t)jq;

  const int keep = formField(body, "keepfree").toInt();
  if (keep >= 0 && keep <= 32000) stored.keepFreeMb = (uint16_t)keep;

  if (!configSave(stored)) return sendRecordings(req, "Could not save.");

  // Applied immediately: this is the one setting people tune by trying it, and
  // a reboot for each attempt would make that miserable.
  motionSetSensitivity(stored.motionSensitivity);
  cameraApplySettings(stored.frameSize, stored.jpegQuality);
  prerollSetWindow(stored.prerollSeconds);
  return sendRecordings(req, "Saved.");
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
<h2 style="margin-top:24px">Update</h2>
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

  const esp_partition_t *running = esp_ota_get_running_partition();

  String body = "<h1>Firmware</h1><table>";
  auto row = [&body](const char *k, const String &v) {
    body += "<tr><th>" + String(k) + "</th><td>" + htmlEscape(v) + "</td></tr>";
  };

  row("Version", String(FIRMWARE_VERSION));
  row("Built", String(__DATE__) + " " + __TIME__);
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
  body += "</table>";
  body += UPDATE_BODY;
  return sendShell(req, "/update", body);
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

  // One wildcard handler and an internal table, rather than one registration per
  // route. esp_http_server allocates a fixed array of handler slots at
  // httpd_start() and never grows it, so registering per route means a ceiling
  // that silently 404s whatever sits past it. That has bitten twice, at 8 and
  // again at 24, and each time looked like a failed firmware upload. The library
  // now sees two registrations however many routes exist, and adding a route is
  // a line in this table.
  static const Route PAGE_ROUTES[] = {
      {"/", HTTP_GET, indexHandler},
      {"/login", HTTP_GET, loginPageHandler},
      {"/login", HTTP_POST, loginPostHandler},
      {"/logout", HTTP_GET, logoutHandler},
      {"/capture", HTTP_GET, captureHandler},
      {"/status", HTTP_GET, statusHandler},
      {"/restart", HTTP_GET, restartHandler},
      {"/retrycam", HTTP_GET, cameraRetryHandler},
      {"/settings", HTTP_GET, settingsPageHandler},
      {"/settings", HTTP_POST, settingsPostHandler},
      {"/networks", HTTP_GET, networksHandler},
      {"/recording", HTTP_GET, recordingsPageHandler},
      {"/recording", HTTP_POST, recordingsPostHandler},
      {"/record", HTTP_GET, recordStateHandler},
      {"/record", HTTP_POST, recordHandler},
      {"/flash", HTTP_POST, flashHandler},
      {"/image", HTTP_POST, imageHandler},
      {"/files", HTTP_GET, filesPageHandler},
      {"/files", HTTP_POST, filesDeleteHandler},
      {"/download", HTTP_GET, downloadHandler},
      {"/play", HTTP_GET, playPageHandler},
      {"/recindex", HTTP_GET, recIndexHandler},
      {"/frame", HTTP_GET, frameHandler},
      {"/update", HTTP_GET, updatePageHandler},
      {"/update", HTTP_POST, updatePostHandler},
      {"/sdbench", HTTP_GET, benchHandler},
  };
  routeTable = PAGE_ROUTES;
  routeCount = sizeof(PAGE_ROUTES) / sizeof(PAGE_ROUTES[0]);

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port = 32768;
  cfg.lru_purge_enable = true;
  cfg.stack_size = 8192;
  cfg.recv_wait_timeout = 30;
  cfg.send_wait_timeout = 30;

  // Only the two wildcards are registered, so the default of eight is ample.
  cfg.uri_match_fn = httpd_uri_match_wildcard;

  if (httpd_start(&pageServer, &cfg) != ESP_OK) {
    Serial.println("page server failed to start");
    return false;
  }

  httpd_uri_t getAll = {"/*", HTTP_GET, dispatchHandler, nullptr};
  httpd_uri_t postAll = {"/*", HTTP_POST, dispatchHandler, nullptr};
  registerUri(pageServer, &getAll);
  registerUri(pageServer, &postAll);

  // The stream gets its own server because a handler that never returns occupies
  // its server's only worker task. Sharing one server would mean the page and
  // /capture stop responding for as long as anyone is watching. Two routes, so
  // they are registered directly.
  cfg.server_port = 81;
  cfg.ctrl_port = 32769;  // must differ, or the second server refuses to start
  cfg.uri_match_fn = nullptr;
  if (httpd_start(&streamServer, &cfg) != ESP_OK) {
    Serial.println("stream server failed to start");
    return false;
  }
  httpd_uri_t stream = {"/stream", HTTP_GET, streamHandler, nullptr};
  httpd_uri_t replay = {"/playstream", HTTP_GET, playStreamHandler, nullptr};
  registerUri(streamServer, &stream);
  registerUri(streamServer, &replay);
  return true;
}

void webBeginUpdate() { updating = true; }
