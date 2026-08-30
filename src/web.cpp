#include <Arduino.h>
#include <atomic>
#include <Update.h>
#include <WiFi.h>
#include "esp_http_server.h"
#include "esp_ota_ops.h"

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif

#include "auth.h"
#include "avi.h"
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
  else if (!strcmp(name, "disk"))
    d = "<ellipse cx='8' cy='4' rx='5.5' ry='2'/>"
        "<path d='M2.5 4v8c0 1.1 2.5 2 5.5 2s5.5-.9 5.5-2V4'/><path d='M2.5 8c0 "
        "1.1 2.5 2 5.5 2s5.5-.9 5.5-2'/>";
  else if (!strcmp(name, "zoom"))
    d = "<circle cx='7' cy='7' r='4.5'/><path d='M10.5 10.5L14 14M5 7h4M7 5v4'/>";
  else
    return "";

  return String("<svg viewBox='0 0 16 16' width='15' height='15' fill='none' "
                "stroke='currentColor' stroke-width='1.4' stroke-linecap='round' "
                "stroke-linejoin='round' aria-hidden='true'>") + d + "</svg>";
}

// Sizes here run from a few hundred bytes of index to gigabytes of footage, so
// any single unit is unreadable at one end or the other. One decimal below a
// hundred and none above it, which keeps the column about the same width
// whichever unit a row lands in.
static String formatSize(uint64_t bytes) {
  static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  int unit = 0;
  double value = (double)bytes;
  while (value >= 1024.0 && unit < 4) {
    value /= 1024.0;
    unit++;
  }
  return String(value, (unit == 0 || value >= 100.0) ? 0 : 1) + " " + units[unit];
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
    ".brand{display:flex;align-items:center;gap:9px}"
    ".brand .logo{flex:0 0 17px;line-height:0}"
    ".brand .logo svg{width:17px;height:auto;display:block}"
    ".loginmark{display:flex;justify-content:center;margin-bottom:14px}"
    ".loginmark svg{width:46px;height:auto;color:#2a7}"
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

// The Argus eye: one neck of the creature the aggregator is named for, since a
// camera is one of its eyes. Inline like every other icon here, so there is
// nothing extra to serve and it takes the colour of whatever it sits in.
static const char ARGUS_EYE[] =
    "<svg viewBox='0 0 84 116' fill='currentColor' fill-rule='nonzero' aria-hidden='true'>"
    "<path d='"
    "M44.2 80.7L45.2 77.8L46 74.7L46.6 71.7L47.1 68.6L47.3 65.5L47.3 62.5L47.1 59.4L46.6 56.3L46 53.3"
    "L45.2 50.2L44.2 47.3L43 45.9L41.3 45.8L39.9 47L39.8 48.7L40.8 51.6L41.5 54.4L42.1 57.1L42.5 59.9"
    "L42.7 62.6L42.7 65.4L42.5 68.1L42.1 70.9L41.5 73.6L40.8 76.4L39.8 79.3L41 77.9L42.7 77.8"
    "L44.1 79ZM19.7 33.5L24 28.6L28.2 24.9L32.2 22.1L36.2 20.3L40.1 19.4L43.9 19.4L47.8 20.3"
    "L51.8 22.1L55.8 24.9L60 28.6L63.7 32L60 35.4L55.8 39.1L51.8 41.9L47.8 43.7L43.9 44.6L40.1 44.6"
    "L36.2 43.7L32.2 41.9L28.2 39.1L24 35.4L19.7 30.5ZM16.3 33.5L20.7 38.6L25.3 42.7L30 45.9"
    "L34.7 48.1L39.6 49.2L44.4 49.2L49.3 48.1L54 45.9L58.7 42.7L63.3 38.6L68.3 32L63.3 25.4L58.7 21.3"
    "L54 18.1L49.3 15.9L44.4 14.8L39.6 14.8L34.7 15.9L30 18.1L25.3 21.3L20.7 25.4L16.3 30.5Z"
    "M47.2 31.6L46.9 33.8L46 35.3L44.6 36.5L42.9 37.1L41.1 37.1L39.4 36.5L38 35.3L37.1 33.8L36.8 32"
    "L37.1 30.2L38 28.7L39.4 27.5L41.1 26.9L42.9 26.9L44.6 27.5L46 28.7L47.1 30.6ZM51 28.3L49.5 25.7"
    "L46.9 23.5L43.7 22.3L40.3 22.3L37.1 23.5L34.5 25.7L32.8 28.6L32.2 32L32.8 35.4L34.5 38.3"
    "L37.1 40.5L40.3 41.7L43.7 41.7L46.9 40.5L49.5 38.3L51.2 35.4L51.8 32.4ZM26.9 18.7L12.7 10.2"
    "L13.7 11.6L13.5 13.4L12 14.4L10.3 14.1L24.5 22.7L26.2 22.9L27.7 21.9L27.9 20.1ZM34.6 11.8"
    "L26.5 -2.6L26.7 -0.9L25.6 0.5L23.9 0.7L22.5 -0.4L30.6 14L30.4 12.3L31.5 10.9L33.2 10.7Z"
    "M44.3 10.1L44.3 -6.5L43.6 -4.9L42 -4.2L40.4 -4.9L39.7 -6.5L39.7 10.1L40.4 11.7L42 12.3"
    "L43.6 11.7ZM53.4 14L61.5 -0.4L61.7 -2.1L60.7 -3.5L58.9 -3.7L57.5 -2.6L49.4 11.8L50.8 10.7"
    "L52.5 10.9L53.6 12.3ZM59.5 22.7L73.7 14.1L74.7 12.7L74.5 11L73.1 9.9L71.3 10.2L57.1 18.7"
    "L58.9 18.5L60.3 19.5L60.6 21.3ZM31.2 93.5L31.6 91.8L32.2 90.5L33.2 89.2L34.5 88.1L36 87.2"
    "L37.6 86.6L39.4 86.2L41.2 86.1L43 86.3L44.6 86.7L46.1 87.4L47.3 88.2L48.2 89.1L48.9 90.1"
    "L49.2 91.1L49.4 92.1L49.3 93L49 93.9L48.5 94.7L47.7 95.5L46.8 96.2L45.8 96.7L44.6 97L43.4 97.2"
    "L42.2 97.1L41.2 96.9L40.2 96.6L39.4 96.1L38.8 95.6L38.3 95L38.1 94.5L38 94.1L38 93.6L38.1 93.2"
    "L38.3 92.8L38.6 92.4L39 92.1L39.5 91.8L40.1 91.6L40.7 91.5L41.2 91.5L41.7 91.6L42.1 91.8"
    "L42.5 91.9L42.6 92.1L42.7 92.2L42.8 92.2L42.7 92.2L42.7 92.1L42.7 92.3L42.8 94.1L44.2 95.2"
    "L45.9 95L47.1 93.7L47.3 92.9L47.3 91.9L47.1 90.8L46.6 89.7L45.9 88.9L45 88.1L44 87.5L42.8 87.2"
    "L41.6 87L40.3 87L39 87.2L37.7 87.6L36.5 88.3L35.4 89.1L34.5 90.3L33.8 91.6L33.4 93L33.4 94.6"
    "L33.8 96.1L34.5 97.5L35.5 98.8L36.8 99.9L38.3 100.7L39.9 101.3L41.8 101.7L43.6 101.7L45.5 101.5"
    "L47.4 101L49.2 100.1L50.7 99L52.1 97.5L53.1 95.9L53.8 94L54 92L53.7 89.9L52.9 88L51.8 86.2"
    "L50.2 84.6L48.3 83.4L46.2 82.4L43.8 81.8L41.3 81.5L38.8 81.7L36.3 82.2L34 83.1L31.8 84.4"
    "L29.9 86.1L28.3 88.1L27.3 90.3L26.8 92.5L27.8 91.1L29.5 90.8L30.9 91.8Z"
    "'/>"
    "<path d='"
    "M45.6 32L45.2 33.7L44 35L42.4 35.6L40.7 35.4L39.3 34.4L38.5 32.9L38.5 31.1L39.3 29.6L40.7 28.6"
    "L42.4 28.4L44 29L45.2 30.3Z"
    "'/>"
    "</svg>";

// The favicon is the same drawing, handed over as a data URI so it costs one
// attribute rather than a route and a request.
static const char ARGUS_FAVICON[] =
    "<link rel=icon href=\"data:image/svg+xml,%3Csvg%20xmlns%3D%27http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%27%20viewBox%3D%270%200%2084%20116%27%20fill%3D%27%23e8e8e8%27%20fill-rule%3D%27nonzero%27%3E%3Cpath%20d%3D%27M44.2%2080.7L45.2%2077.8L46%2074.7L46.6%2071.7L47.1%2068.6L47.3%2065.5L47.3%2062.5L47.1%2059.4L46.6%2056.3L46%2053.3L45.2%2050.2L44.2%2047.3L43%2045.9L41.3%2045.8L39.9%2047L39.8%2048.7L40.8%2051.6L41.5%2054.4L42.1%2057.1L42.5%2059.9L42.7%2062.6L42.7%2065.4L42.5%2068.1L42.1%2070.9L41.5%2073.6L40.8%2076.4L39.8%2079.3L41%2077.9L42.7%2077.8L44.1%2079ZM19.7%2033.5L24%2028.6L28.2%2024.9L32.2%2022.1L36.2%2020.3L40.1%2019.4L43.9%2019.4L47.8%2020.3L51.8%2022.1L55.8%2024.9L60%2028.6L63.7%2032L60%2035.4L55.8%2039.1L51.8%2041.9L47.8%2043.7L43.9%2044.6L40.1%2044.6L36.2%2043.7L32.2%2041.9L28.2%2039.1L24%2035.4L19.7%2030.5ZM16.3%2033.5L20.7%2038.6L25.3%2042.7L30%2045.9L34.7%2048.1L39.6%2049.2L44.4%2049.2L49.3%2048.1L54%2045.9L58.7%2042.7L63.3%2038.6L68.3%2032L63.3%2025.4L58.7%2021.3L54%2018.1L49.3%2015.9L44.4%2014.8L39.6%2014.8L34.7%2015.9L30%2018.1L25.3%2021.3L20.7%2025.4L16.3%2030.5ZM47.2%2031.6L46.9%2033.8L46%2035.3L44.6%2036.5L42.9%2037.1L41.1%2037.1L39.4%2036.5L38%2035.3L37.1%2033.8L36.8%2032L37.1%2030.2L38%2028.7L39.4%2027.5L41.1%2026.9L42.9%2026.9L44.6%2027.5L46%2028.7L47.1%2030.6ZM51%2028.3L49.5%2025.7L46.9%2023.5L43.7%2022.3L40.3%2022.3L37.1%2023.5L34.5%2025.7L32.8%2028.6L32.2%2032L32.8%2035.4L34.5%2038.3L37.1%2040.5L40.3%2041.7L43.7%2041.7L46.9%2040.5L49.5%2038.3L51.2%2035.4L51.8%2032.4ZM26.9%2018.7L12.7%2010.2L13.7%2011.6L13.5%2013.4L12%2014.4L10.3%2014.1L24.5%2022.7L26.2%2022.9L27.7%2021.9L27.9%2020.1ZM34.6%2011.8L26.5%20-2.6L26.7%20-0.9L25.6%200.5L23.9%200.7L22.5%20-0.4L30.6%2014L30.4%2012.3L31.5%2010.9L33.2%2010.7ZM44.3%2010.1L44.3%20-6.5L43.6%20-4.9L42%20-4.2L40.4%20-4.9L39.7%20-6.5L39.7%2010.1L40.4%2011.7L42%2012.3L43.6%2011.7ZM53.4%2014L61.5%20-0.4L61.7%20-2.1L60.7%20-3.5L58.9%20-3.7L57.5%20-2.6L49.4%2011.8L50.8%2010.7L52.5%2010.9L53.6%2012.3ZM59.5%2022.7L73.7%2014.1L74.7%2012.7L74.5%2011L73.1%209.9L71.3%2010.2L57.1%2018.7L58.9%2018.5L60.3%2019.5L60.6%2021.3ZM31.2%2093.5L31.6%2091.8L32.2%2090.5L33.2%2089.2L34.5%2088.1L36%2087.2L37.6%2086.6L39.4%2086.2L41.2%2086.1L43%2086.3L44.6%2086.7L46.1%2087.4L47.3%2088.2L48.2%2089.1L48.9%2090.1L49.2%2091.1L49.4%2092.1L49.3%2093L49%2093.9L48.5%2094.7L47.7%2095.5L46.8%2096.2L45.8%2096.7L44.6%2097L43.4%2097.2L42.2%2097.1L41.2%2096.9L40.2%2096.6L39.4%2096.1L38.8%2095.6L38.3%2095L38.1%2094.5L38%2094.1L38%2093.6L38.1%2093.2L38.3%2092.8L38.6%2092.4L39%2092.1L39.5%2091.8L40.1%2091.6L40.7%2091.5L41.2%2091.5L41.7%2091.6L42.1%2091.8L42.5%2091.9L42.6%2092.1L42.7%2092.2L42.8%2092.2L42.7%2092.2L42.7%2092.1L42.7%2092.3L42.8%2094.1L44.2%2095.2L45.9%2095L47.1%2093.7L47.3%2092.9L47.3%2091.9L47.1%2090.8L46.6%2089.7L45.9%2088.9L45%2088.1L44%2087.5L42.8%2087.2L41.6%2087L40.3%2087L39%2087.2L37.7%2087.6L36.5%2088.3L35.4%2089.1L34.5%2090.3L33.8%2091.6L33.4%2093L33.4%2094.6L33.8%2096.1L34.5%2097.5L35.5%2098.8L36.8%2099.9L38.3%20100.7L39.9%20101.3L41.8%20101.7L43.6%20101.7L45.5%20101.5L47.4%20101L49.2%20100.1L50.7%2099L52.1%2097.5L53.1%2095.9L53.8%2094L54%2092L53.7%2089.9L52.9%2088L51.8%2086.2L50.2%2084.6L48.3%2083.4L46.2%2082.4L43.8%2081.8L41.3%2081.5L38.8%2081.7L36.3%2082.2L34%2083.1L31.8%2084.4L29.9%2086.1L28.3%2088.1L27.3%2090.3L26.8%2092.5L27.8%2091.1L29.5%2090.8L30.9%2091.8Z%27%2F%3E%3Cpath%20d%3D%27M45.6%2032L45.2%2033.7L44%2035L42.4%2035.6L40.7%2035.4L39.3%2034.4L38.5%2032.9L38.5%2031.1L39.3%2029.6L40.7%2028.6L42.4%2028.4L44%2029L45.2%2030.3Z%27%2F%3E%3C%2Fsvg%3E\">";

static esp_err_t sendHtml(httpd_req_t *req, const String &body) {
  String page = "<!doctype html><meta charset=utf-8>"
                "<meta name=viewport content='width=device-width,initial-scale=1'>";
  page += ARGUS_FAVICON;
  page += "<title>";
  page += htmlEscape(cameraName) + " Cam";
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

  String nav = "<div class=app><aside><div class=brand>";
  nav += String("<span class=logo>") + ARGUS_EYE + "</span>";
  nav += htmlEscape(cameraName) + " Cam</div>";
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
  String body = String("<div class=card><form method=post action=/login>"
                       "<div class=loginmark>") + ARGUS_EYE + "</div>"
                "<h2>" + htmlEscape(cameraName) + " Cam</h2>"
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

// Arming is a browser-side state: the device never holds the LED on between
// requests, so there is nothing on it to toggle. The capture link carries the
// choice instead.
const fb = document.getElementById('flash');
const cap = document.getElementById('cap');
if (fb && cap) fb.onclick = () => {
  const armed = fb.className !== 'on';
  fb.className = armed ? 'on' : '';
  cap.href = armed ? '/capture?flash=1' : '/capture';
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
  // The flash arms rather than switches on. It is built for a burst, so the only
  // thing that fires it is a capture, and it goes out again on its own.
  body += String("<button id=flash data-tip=\"Fire the flash for the next "
                 "capture. Brightness is under Image adjustments\" class=\"\">") +
          icon("bolt") + "</button>";
  body += "<a id=cap class=btn href=\"/capture\" target=_blank data-tip=\"Open a "
          "full resolution still in a new tab\">" + icon("image") + "</a>";
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

// Whether this camera can record at all, in the one word an aggregator needs.
//
// Reported rather than inferred. A camera with no card and a camera whose card
// went read-only both fail to record, and both look identical from outside to
// anything watching for recordings that never appear. Saying so plainly is what
// lets the service record on the camera's behalf instead of waiting.
//
// Mounted is asked first because sdMounted() re-checks the card, while
// sdWritable() reports a write test run at boot: a card pulled since then is
// missing, not unwritable.
static const char *storageState() {
  if (!sdMounted()) return "missing";
  return sdWritable() ? "ok" : "unwritable";
}

// Toggles rather than taking a state, so the button cannot disagree with the
// device when two browsers are open on the same camera.
// Lets the button reflect what the device is doing rather than what it was told
// to do. Without it the label reads "Recording..." indefinitely.
static esp_err_t recordStateHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;
  // Sized for every field at its longest. snprintf truncates in silence, and a
  // JSON response cut off mid-number is a parse error with no cause attached.
  // Sized for the widest each field can be at 32-bit maxima, plus the two
  // words cardless can be. Truncating a status document silently is how a
  // reader ends up parsing half a number.
  char out[416];
  uint32_t grab = 0, write = 0, index = 0;
  recordingTiming(&grab, &write, &index);
  Config c;
  configLoad(c);
  snprintf(out, sizeof(out),
           "{\"active\":%s,\"frames\":%lu,\"fps\":%.1f,"
           "\"grabMs\":%lu,\"writeMs\":%lu,\"indexMs\":%lu,"
           "\"triggered\":%s,\"motion\":%s,\"armed\":%s,\"change\":%u,"
           "\"threshold\":%u,\"preFrames\":%lu,\"preSecs\":%lu,"
           "\"lux\":%u,\"rung\":%u,\"ael\":%d,\"gc\":%u,\"storage\":\"%s\","
           "\"cardless\":%s}",
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
           (unsigned)cameraCurrentImage().gainCeiling, storageState(),
           recordingCardless() ? "true" : "false");
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


static esp_err_t captureHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;

  const bool withFlash = queryParam(req, "flash", "") == "1";
  camera_fb_t *fb = withFlash ? cameraGrabWithFlash() : esp_camera_fb_get();
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
static esp_err_t replayStream(httpd_req_t *req) {

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
          "<a class=btn data-tip=\"Download this recording as an AVI file\" "
          "href=\"/video?dir=" + htmlEscape(dir) + "\">" + icon("down") +
          "Download</a>"
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

// Motion detection keeps its state in a handful of statics written once per
// frame. One viewer feeding it is what it was built for; three feeding it at
// once would corrupt the comparison rather than speed it up, so the first
// stream to start claims detection and the rest just send pictures.
static portMUX_TYPE detectorLock = portMUX_INITIALIZER_UNLOCKED;
static bool detectorTaken = false;

static bool claimDetector() {
  bool got = false;
  portENTER_CRITICAL(&detectorLock);
  if (!detectorTaken) {
    detectorTaken = true;
    got = true;
  }
  portEXIT_CRITICAL(&detectorLock);
  return got;
}

static void releaseDetector() {
  portENTER_CRITICAL(&detectorLock);
  detectorTaken = false;
  portEXIT_CRITICAL(&detectorLock);
}

static esp_err_t liveStream(httpd_req_t *req) {

  esp_err_t res = httpd_resp_set_type(req, STREAM_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char partHeader[64];

  // While a recording runs, the recorder owns the camera and republishes each
  // frame. Grabbing here as well starves it.
  uint8_t *shared = nullptr;
  uint32_t sharedSeq = 0;
  const bool detector = claimDetector();

  while (true) {
    if (recordingOwnsCamera()) {
      if (!shared) shared = (uint8_t *)ps_malloc(200 * 1024);
      size_t len = 0;
      if (!shared || !recordingCopyLatest(shared, 200 * 1024, &len, &sharedSeq) ||
          !isCompleteJpegBuf(shared, len)) {
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
    if (detector) motionObserve(fb);

    // A frame the sensor tore is worse than a missed one: it draws as half a
    // picture over the last good one, which is what the glitches were. The
    // recorder has always dropped these; the stream was handing them straight to
    // the browser.
    if (!isCompleteJpeg(fb)) {
      esp_camera_fb_return(fb);
      if (updating) break;
      continue;
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
  if (detector) releaseDetector();
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
  // Asked of the driver now, not remembered from boot. A frame size change
  // rebuilds the camera and can fail, and reporting the boot-time answer after
  // that is how a camera that had stopped delivering frames went on calling
  // itself detected.
  row("Camera sensor", cameraIsReady() ? "detected" : "NOT DETECTED");
  row("Time", clockNow());
  row("Uptime", humanUptime());
  const bool online = WiFi.status() == WL_CONNECTED;
  row("Network", online ? WiFi.SSID() : String("disconnected"));
  row("Address", online ? WiFi.localIP().toString() : String("none"));
  row("Signal", online ? String(WiFi.RSSI()) + " dBm" : String("n/a"));
  row("Reconnects", String(reconnectTally));
  row("Viewers", String(streamViewerCount()) + " of " + String(streamViewerLimit()) +
                     " watching");
  if (sdMounted()) {
    row("SD card", sdCardType() + ", " + formatSize(sdTotalBytes()) + ", " +
                       formatSize(sdTotalBytes() - sdUsedBytes()) + " free" +
                       (sdWritable() ? "" : ", NOT WRITABLE"));
  } else {
    row("SD card",
        "not detected, or formatted exFAT. Cards over 32GB need FAT32.");
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
// Names a recording for when it was made. Every recording holds a file of the
// same name, so without this a morning's footage lands as video.mjpeg,
// video(1).mjpeg, and so on, with nothing to say which was which. The two
// directories above the file are the date and the time, which is exactly the
// missing information.
static String recordingFileName(const String &dir) {
  const int up = dir.lastIndexOf('/');
  if (up <= 0) return dir.substring(up + 1);
  const String parent = dir.substring(0, up);
  // A recording made before the clock reached an NTP server is numbered and
  // sits in the root, where there is no date to put in front of it.
  if (parent == "/rec") return dir.substring(up + 1);
  return parent.substring(parent.lastIndexOf('/') + 1) + "-" + dir.substring(up + 1);
}

// Hands over a recording as a file that plays.
//
// The frames go out exactly as they are stored, so nothing is decoded and the
// camera does no more work than it does for a plain download. What it adds is
// the timing, which lives in the index and which a bare run of JPEGs has nowhere
// to put: without it mpv treats a recording as a folder of photos at one frame a
// second and ffmpeg assumes twenty five, so a twelve second clip plays as
// eighty five seconds or as three.
//
// The index is read three times rather than held in memory, once to size the
// file, once to place the frames and once to write the index the container
// wants. A recording can run to thousands of frames, and reading a small file
// three times costs less than a table that grows with how long someone recorded.
static esp_err_t videoHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;

  const String dir = queryParam(req, "dir", "");
  const String videoPath = dir + "/video.mjpeg";
  const String indexPath = dir + "/index.txt";
  if (!sdPathIsSafe(dir) || !sdExists(videoPath) || !sdExists(indexPath)) {
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_send(req, "no such recording", HTTPD_RESP_USE_STRLEN);
  }

  AviInfo info = {};
  void *idx = nullptr;
  if (!sdIndexOpen(indexPath, &idx)) return httpd_resp_send_500(req);
  uint32_t off = 0, len = 0, atMs = 0;
  while (sdIndexNext(idx, &off, &len, &atMs)) {
    if (len == 0) continue;  // the later passes skip these too
    info.frames++;
    info.movieBytes += 8 + len + (len & 1);
    if (len > info.maxFrameLen) info.maxFrameLen = len;
    info.durMs = atMs;
  }
  sdIndexClose(idx);
  if (info.frames == 0) {
    httpd_resp_set_status(req, "404 Not Found");
    return httpd_resp_send(req, "recording has no frames", HTTPD_RESP_USE_STRLEN);
  }

  void *video = nullptr;
  if (!sdOpenRead(videoPath, &video)) return httpd_resp_send_500(req);

  uint8_t *buf = (uint8_t *)ps_malloc(info.maxFrameLen + AVI_HEADER_BYTES);
  if (!buf) {
    sdCloseRead(video);
    return httpd_resp_send_500(req);
  }

  // The recording says what size it was made at, rather than the camera saying
  // what size it is set to now: footage from before a resolution change is still
  // on the card, and would otherwise be described wrongly.
  info.width = 640;
  info.height = 480;
  const size_t head = sdReadAt(video, 0, buf, 1024);
  aviJpegSize(buf, head, &info.width, &info.height);

  const String name = recordingFileName(dir) + ".avi";
  const String disp = "attachment; filename=\"" + name + "\"";
  httpd_resp_set_type(req, "video/x-msvideo");
  httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());

  aviWriteHeader(buf, info);
  esp_err_t res = httpd_resp_send_chunk(req, (const char *)buf, AVI_HEADER_BYTES);

  if (res == ESP_OK && sdIndexOpen(indexPath, &idx)) {
    while (res == ESP_OK && sdIndexNext(idx, &off, &len, &atMs)) {
      if (len == 0) continue;
      aviWriteChunkHeader(buf, len);
      if (sdReadAt(video, off, buf + 8, len) != len) break;
      // The chunk header, the frame and its pad byte go out together: three
      // sends a frame would put three write calls on the wire for every frame of
      // a recording that can hold thousands.
      const size_t pad = len & 1;
      if (pad) buf[8 + len] = 0;
      res = httpd_resp_send_chunk(req, (const char *)buf, 8 + len + pad);
    }
    sdIndexClose(idx);
  }

  if (res == ESP_OK && sdIndexOpen(indexPath, &idx)) {
    aviWriteIndexHeader(buf, info.frames);
    size_t held = AVI_INDEX_HEADER_BYTES;
    uint32_t at = AVI_FIRST_FRAME_OFFSET;
    while (res == ESP_OK && sdIndexNext(idx, &off, &len, &atMs)) {
      if (len == 0) continue;
      aviWriteIndexEntry(buf + held, len, &at);
      held += AVI_INDEX_ENTRY_BYTES;
      // Entries are sixteen bytes each, so they are gathered into the frame
      // buffer and sent in batches rather than one packet per frame.
      if (held + AVI_INDEX_ENTRY_BYTES > info.maxFrameLen) {
        res = httpd_resp_send_chunk(req, (const char *)buf, held);
        held = 0;
      }
    }
    sdIndexClose(idx);
    if (res == ESP_OK && held > 0) {
      res = httpd_resp_send_chunk(req, (const char *)buf, held);
    }
  }

  free(buf);
  sdCloseRead(video);
  httpd_resp_send_chunk(req, nullptr, 0);
  return res;
}

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
  String name = cut >= 0 ? path.substring(cut + 1) : path;

  if (name == "video.mjpeg" && cut > 0) {
    name = recordingFileName(path.substring(0, cut)) + ".mjpeg";
  }
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

// Quotes and backslashes are the only characters that can break out of a JSON
// string here; the settings this serves are names and timezones, not free text.
static String jsonEscape(const String &in) {
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    const char c = in[i];
    if (c == '"' || c == '\\') out += '\\';
    if ((uint8_t)c < 0x20) continue;
    out += c;
  }
  return out;
}

// Every setting, as JSON.
//
// The pages render settings into HTML forms, which is fine for a person and
// useless for anything else: an aggregator setting one value across several
// cameras would have to scrape a form to learn the other values it must not
// disturb. It has to learn them, because the form handlers take a whole form,
// and a checkbox left out of a POST reads as unticked rather than unchanged.
// So read here, merge, and post the whole thing back.
static esp_err_t configJsonHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;

  Config c;
  configLoad(c);

  String json = "{";
  auto num = [&](const char *k, long v) {
    if (json.length() > 1) json += ",";
    json += "\"" + String(k) + "\":" + String(v);
  };
  auto boolean = [&](const char *k, bool v) {
    if (json.length() > 1) json += ",";
    json += "\"" + String(k) + "\":" + (v ? "true" : "false");
  };
  auto text = [&](const char *k, const String &v) {
    if (json.length() > 1) json += ",";
    json += "\"" + String(k) + "\":\"" + jsonEscape(v) + "\"";
  };

  text("camname", c.cameraName);
  text("tz", c.timezone);
  text("ssid", c.wifiSsid);
  boolean("apwin", c.apWindow);

  boolean("moten", c.motionEnabled);
  num("motsens", c.motionSensitivity);
  num("recsec", c.recordSeconds);
  num("presec", c.prerollSeconds);
  num("quietsec", c.quietSeconds);
  num("keepfree", c.keepFreeMb);

  boolean("schen", c.scheduleEnabled);
  num("schfrom", c.scheduleFromHour);
  num("schto", c.scheduleToHour);
  num("schdays", c.scheduleDays);

  num("fsize", c.frameSize);
  num("jq", c.jpegQuality);
  boolean("autoimg", c.autoImage);
  num("ael", c.aeLevel);
  num("gc", c.gainCeiling);
  num("bri", c.brightness);
  num("con", c.contrast);
  num("sat", c.saturation);
  num("wb", c.wbMode);
  boolean("gray", c.grayscale);
  boolean("hmir", c.hmirror);
  boolean("vflip", c.vflip);
  num("flashlvl", c.flashLevel);

  // Not a setting, but the one thing a caller reading this to decide what to do
  // with the camera has to know, and it is here so it does not cost a second
  // request to find out.
  text("storage", storageState());

  // What the sensor is actually doing, which under auto is not what is stored.
  num("aelnow", cameraCurrentImage().aeLevel);
  num("gcnow", cameraCurrentImage().gainCeiling);
  text("unsupported", cameraUnsupported());

  json += "}";
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json.c_str(), json.length());
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

// How many entries one page of the file listing holds. Shared, because the
// metadata loader fills one array per row and must not silently stop short of
// the page it was given.
static constexpr int LIST_PAGE = 50;

// A row of the listing, held in bulk while the whole directory is sorted.
struct FileRow {
  SdName entry;
  uint32_t durMs;
  uint32_t bytes;
  bool known;
};

// Sorting means holding a whole directory at once, so the table goes in PSRAM.
// A day of motion triggers runs to hundreds of recordings; this is generous
// enough that the cap is a backstop rather than a limit anyone meets.
static constexpr int LIST_MAX = 2000;

enum SortKey { SORT_NAME = 0, SORT_SIZE = 1, SORT_LENGTH = 2 };

// The comparator's key lives here rather than in an argument because qsort has
// nowhere to put one. Safe because the page server runs on a single task, so
// only one listing is ever being sorted.
static SortKey sortKey = SORT_NAME;
static bool sortDesc = false;

static int compareRows(const void *a, const void *b) {
  const FileRow *x = (const FileRow *)a;
  const FileRow *y = (const FileRow *)b;

  // Directories first whichever way the sort runs, the way a file manager does
  // it: reversing the order should not bury the way back up among the files.
  if (x->entry.isDir != y->entry.isDir) return x->entry.isDir ? -1 : 1;

  int c = 0;
  if (sortKey == SORT_SIZE) {
    // A recording's size is the footage inside it, not the directory entry,
    // which is zero for every directory and would sort them all equal.
    const uint32_t xs = x->known ? x->bytes : x->entry.size;
    const uint32_t ys = y->known ? y->bytes : y->entry.size;
    c = xs < ys ? -1 : xs > ys ? 1 : 0;
  } else if (sortKey == SORT_LENGTH) {
    c = x->durMs < y->durMs ? -1 : x->durMs > y->durMs ? 1 : 0;
  }
  // Name settles every other tie, so two recordings of the same length keep the
  // same order between one page load and the next.
  if (c == 0) c = strcmp(x->entry.name, y->entry.name);
  return sortDesc ? -c : c;
}

// A day directory is ten characters of YYYY-MM-DD. Checked character by
// character rather than by length alone, because /rec holds whatever else has
// been copied onto the card as well as the directories this firmware made.
static bool isDayName(const char *name) {
  if (strlen(name) != 10) return false;
  for (int i = 0; i < 10; i++) {
    if (i == 4 || i == 7) {
      if (name[i] != '-') return false;
    } else if (!isdigit((unsigned char)name[i])) return false;
  }
  return true;
}

// Recordings are named for the time they started, so an all-digit name is what
// separates one from the day directories it sits beside in /rec.
static bool isAllDigits(const char *name) {
  if (!*name) return false;
  for (const char *c = name; *c; c++) {
    if (!isdigit((unsigned char)*c)) return false;
  }
  return true;
}

// Fills in the length and size of every recording in a directory.
//
// The day summary answers all of them in one read. Anything missing from it is
// a recording that never wrote its line, because it predates the summary or
// because the power went before it stopped; that falls back to the tail of its
// own index, which is written per frame and so survives both. What the fallback
// finds is appended, including a zero for a recording that never got a frame, so
// no recording is ever read the slow way twice.
static void loadRecMeta(const String &dirPath, FileRow *rows, int count) {
  // Anything that is not a recording is marked known straight away: there is
  // nothing to look up for it, and nothing to retry on the next listing.
  auto isRecording = [&](const FileRow &row) {
    return row.entry.isDir && isAllDigits(row.entry.name);
  };
  for (int i = 0; i < count; i++) {
    rows[i].durMs = 0;
    rows[i].bytes = 0;
    rows[i].known = !isRecording(rows[i]);
  }

  // Recording names are digits, so the summary parses as three numbers and the
  // index reader already knows how to do that.
  void *fh = nullptr;
  if (sdIndexOpen(dirPath + "/.day", &fh)) {
    uint32_t at = 0, durMs = 0, bytes = 0;
    while (sdIndexNext(fh, &at, &durMs, &bytes)) {
      for (int i = 0; i < count; i++) {
        if (rows[i].known) continue;
        if ((uint32_t)strtoul(rows[i].entry.name, nullptr, 10) != at) continue;
        rows[i].durMs = durMs;
        rows[i].bytes = bytes;
        rows[i].known = true;
        break;
      }
    }
    sdIndexClose(fh);
  }

  int recovered = 0;
  for (int i = 0; i < count; i++) {
    if (rows[i].known) continue;

    // The last index line carries the last frame's offset, length and time,
    // which is the whole answer: the recording ran that long and is that big.
    const String recPath = dirPath + "/" + rows[i].entry.name;
    String tail = sdReadTail(recPath + "/index.txt", 96);
    while (tail.endsWith("\n") || tail.endsWith("\r")) tail.remove(tail.length() - 1);
    const int nl = tail.lastIndexOf('\n');
    const String last = nl >= 0 ? tail.substring(nl + 1) : tail;

    unsigned long off = 0, len = 0, ms = 0;
    if (tail.isEmpty() || sscanf(last.c_str(), "%lu %lu %lu", &off, &len, &ms) != 3) {
      // Nothing readable in there. Recorded as zero rather than left unknown, so
      // a recording the power cut off is not reopened on every listing forever.
      off = len = ms = 0;
    }
    rows[i].durMs = (uint32_t)ms;
    rows[i].bytes = (uint32_t)(off + len);
    rows[i].known = true;
    recovered++;

    char line[112];
    snprintf(line, sizeof(line), "%s %lu %lu\n", rows[i].entry.name, ms, off + len);
    sdAppendSmall(dirPath + "/.day", line);
  }
  if (recovered > 0) {
    Serial.printf("files: recovered %d recording(s) from their index in %s\n",
                  recovered, dirPath.c_str());
  }
}

// The recordings on this card, as JSON, for the aggregator that pulls them off
// it. The file listing already knows all of this and renders it for a person to
// look at; these are the same answers without a page around them, so nothing has
// to scrape HTML to find out what a camera is holding.

// Over a year of days. /rec grows by one directory a day and ageing out removes
// them from the far end, so this is a backstop rather than a limit anyone meets.
static constexpr int DAYS_MAX = 400;

static int compareNames(const void *a, const void *b) {
  return strcmp(((const SdName *)a)->name, ((const SdName *)b)->name);
}

// The days that exist, so the service does not have to guess dates.
//
// A recording made before the clock reached an NTP server is numbered rather
// than dated, and sits in /rec beside the day directories. Those are reported
// under their own key: calling one a day would invent a date for footage whose
// date is exactly what is not known, and leaving it out would hide footage that
// is on the card. The name is what /video wants back either way.
static esp_err_t recordingsDaysHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;
  httpd_resp_set_type(req, "application/json");

  // No card is an empty card, not an error. A caller learns why from `storage`
  // on /config, which says so in a word.
  if (!sdMounted()) {
    return httpd_resp_send(req, "{\"days\":[],\"loose\":[],\"more\":false}",
                           HTTPD_RESP_USE_STRLEN);
  }

  SdName *names = (SdName *)ps_malloc(sizeof(SdName) * DAYS_MAX);
  if (!names) return httpd_resp_send_500(req);

  int total = 0, tooLong = 0;
  const int held = sdScan("/rec", names, DAYS_MAX, &total, &tooLong);
  qsort(names, held, sizeof(SdName), compareNames);

  // Both lists hold names this firmware wrote, filtered to digits and dashes on
  // the way out, so there is nothing here that could need escaping.
  String json = "{\"days\":[";
  bool first = true;
  for (int i = 0; i < held; i++) {
    if (!names[i].isDir || !isDayName(names[i].name)) continue;
    json += first ? "\"" : ",\"";
    json += names[i].name;
    json += "\"";
    first = false;
  }
  json += "],\"loose\":[";
  first = true;
  for (int i = 0; i < held; i++) {
    if (!names[i].isDir || !isAllDigits(names[i].name)) continue;
    json += first ? "\"" : ",\"";
    json += names[i].name;
    json += "\"";
    first = false;
  }
  // Truncation says so. A caller that silently never hears about the oldest day
  // on the card would have no way to tell that from the day not existing.
  json += "],\"more\":";
  json += (total > held) ? "true" : "false";
  json += "}";

  free(names);
  return httpd_resp_send(req, json.c_str(), json.length());
}

// A busy day here ran to 183 recordings. This is a ceiling on one response, not
// on the card, and it is set well above what a day of motion triggering has
// actually produced.
static constexpr int DAY_RECORDINGS_MAX = 500;

struct DayRow {
  uint32_t at;
  uint32_t durMs;
  uint32_t bytes;
  uint32_t frames;
};

static int compareDayRows(const void *a, const void *b) {
  const uint32_t x = ((const DayRow *)a)->at;
  const uint32_t y = ((const DayRow *)b)->at;
  return x < y ? -1 : x > y ? 1 : 0;
}

// One day's recordings, read from that day's summary and nothing else.
//
// The summary holds a line per recording, so a whole day costs one file open.
// Asking each recording directory instead costs about 150ms an open on this
// card, which is 27 seconds for the day of 183 recordings sitting on it now.
// That is the measurement the summary was written against, and it is not a
// price to pay again for a caller that cannot see it being paid.
//
// So this reads only the summary, and reads it whole: the file is appended to,
// not sorted, and a recording recovered from its own index lands at the end
// however early it started. Sorting here is what makes a capped response the
// first N recordings of the day rather than an arbitrary N of them.
//
// Nothing is written back either. The HTML listing repairs a missing line from
// the recording's own index, which it can afford because it has already read the
// directory; this has not, and a GET that rewrites the card to answer a question
// is a surprise. The cost is that a recording the power cut off before it wrote
// its line is missing here until someone opens the day in the browser once.
//
// The summary can also run the other way and name a recording that has been
// deleted, because ageing out removes the directory and leaves the line. The
// listing never notices, since it matches lines against directories it has
// already read. A caller here does, as a 404 from /video, and that is the
// honest answer: the alternative is a file open per line to check, which is the
// 150ms charge this endpoint exists to avoid.
static esp_err_t recordingsJsonHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;
  httpd_resp_set_type(req, "application/json");

  const String day = queryParam(req, "day", "");
  // Checked to the letter rather than handed to sdPathIsSafe: ten characters of
  // YYYY-MM-DD cannot walk anywhere, whatever was in the query string.
  if (!isDayName(day.c_str())) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "{\"error\":\"day must be YYYY-MM-DD\"}",
                           HTTPD_RESP_USE_STRLEN);
  }

  const String dir = String("/rec/") + day;
  void *fh = nullptr;
  if (!sdIndexOpen(dir + "/.day", &fh)) {
    // A day with no summary and a day that is not there answer differently, and
    // only the second is a mistake worth reporting. The extra open is on the
    // path that already found nothing, not on the one that does the work.
    if (!sdExists(dir)) {
      httpd_resp_set_status(req, "404 Not Found");
      return httpd_resp_send(req, "{\"error\":\"no such day\"}", HTTPD_RESP_USE_STRLEN);
    }
    const String empty =
        String("{\"day\":\"") + day + "\",\"recordings\":[],\"more\":false}";
    return httpd_resp_send(req, empty.c_str(), empty.length());
  }

  DayRow *rows = (DayRow *)ps_malloc(sizeof(DayRow) * DAY_RECORDINGS_MAX);
  if (!rows) {
    sdIndexClose(fh);
    return httpd_resp_send_500(req);
  }

  int held = 0, total = 0;
  uint32_t at = 0, durMs = 0, bytes = 0, frames = 0;
  while (sdIndexNext(fh, &at, &durMs, &bytes, &frames)) {
    total++;
    if (held >= DAY_RECORDINGS_MAX) continue;
    rows[held].at = at;
    rows[held].durMs = durMs;
    rows[held].bytes = bytes;
    rows[held].frames = frames;
    held++;
  }
  sdIndexClose(fh);

  qsort(rows, held, sizeof(DayRow), compareDayRows);

  String json = String("{\"day\":\"") + day + "\",\"recordings\":[";
  json.reserve(64 * (held + 2));
  char piece[96];
  for (int i = 0; i < held; i++) {
    // The start time goes back out as the six digits it is on the card, because
    // that is what /video wants for a directory name: 041541 is a recording and
    // 41541 is nothing. It is a string for the same reason.
    //
    // frames is zero for anything recorded before the summary carried a frame
    // count, which is every recording already on a card today. A caller telling
    // "not counted" from "counted none" has bytes to go on: no frames means no
    // bytes.
    const int n = snprintf(piece, sizeof(piece),
                           "%s{\"at\":\"%06lu\",\"durMs\":%lu,\"bytes\":%lu,"
                           "\"frames\":%lu}",
                           i ? "," : "", (unsigned long)rows[i].at,
                           (unsigned long)rows[i].durMs, (unsigned long)rows[i].bytes,
                           (unsigned long)rows[i].frames);
    json.concat(piece, n);
  }
  free(rows);

  json += "],\"more\":";
  json += (total > held) ? "true" : "false";
  json += "}";
  return httpd_resp_send(req, json.c_str(), json.length());
}

static esp_err_t sendFiles(httpd_req_t *req, const String &notice) {
  String path = queryParam(req, "path", "/");
  if (!sdPathIsSafe(path)) path = "/";
  const int startAt = max(0L, queryParam(req, "start", "0").toInt());

  const String sortParam = queryParam(req, "sort", "name");
  const String orderParam = queryParam(req, "order", "asc");
  sortKey = sortParam == "size"     ? SORT_SIZE
            : sortParam == "length" ? SORT_LENGTH
                                    : SORT_NAME;
  sortDesc = (orderParam == "desc");
  // Carried by every link on the page, so paging and stepping into a directory
  // keep the order the person chose instead of snapping back to the default.
  const String sortQS = "&sort=" + String(sortKey == SORT_SIZE     ? "size"
                                          : sortKey == SORT_LENGTH ? "length"
                                                                   : "name") +
                        "&order=" + String(sortDesc ? "desc" : "asc");

  // /rec holds a directory per day; a day holds recordings. Both are rendered
  // with the punctuation put back, since the names are stored without it.
  const bool atRecRoot = (path == "/rec");
  const bool inDay = path.startsWith("/rec/") && path.length() == 15;
  // A recording normally sits in a day directory, but one made before the clock
  // reached an NTP server is numbered and sits in /rec beside the days.
  const bool holdsRecordings = inDay || atRecRoot;

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
    nav += "<a class=btn href=\"/files?path=" + htmlEscape(parent) + sortQS + "\">" +
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

  FileRow *rows = (FileRow *)ps_malloc(sizeof(FileRow) * LIST_MAX);
  if (!rows) {
    body += "<p class=err>Not enough memory to list this directory.</p>";
    return sendShell(req, "/files", body);
  }

  SdName *names = (SdName *)ps_malloc(sizeof(SdName) * LIST_MAX);
  if (!names) {
    free(rows);
    body += "<p class=err>Not enough memory to list this directory.</p>";
    return sendShell(req, "/files", body);
  }

  int total = 0, tooLong = 0;
  const int held = sdScan(path, names, LIST_MAX, &total, &tooLong);
  for (int i = 0; i < held; i++) rows[i].entry = names[i];
  free(names);

  if (total == 0) {
    free(rows);
    body += "<p class=sub>Nothing here.</p>";
    if (tooLong > 0) {
      body += "<p class=err>" + String(tooLong) +
              " item(s) have names too long to list.</p>";
    }
    return sendShell(req, "/files", body);
  }

  // Metadata for the whole directory rather than for one page, because sorting
  // by length or size has to compare rows that are not on the page yet.
  if (holdsRecordings) {
    loadRecMeta(path, rows, held);
  } else {
    for (int i = 0; i < held; i++) {
      rows[i].durMs = 0;
      rows[i].bytes = 0;
      rows[i].known = false;
    }
  }

  qsort(rows, held, sizeof(FileRow), compareRows);

  static constexpr int PAGE = LIST_PAGE;
  const int shown = min(PAGE, max(0, held - startAt));
  const String prefix = (path == "/" ? String("/") : path + "/");

  // Sorting is a GET, so the order is in the address bar: it survives a reload,
  // it can be bookmarked, and the Back button undoes it.
  body += "<form method=get action=/files class=actions>"
          "<input type=hidden name=path value=\"" + htmlEscape(path) + "\">"
          "<label style=\"margin:0;align-self:center\">Sort by</label>"
          "<select name=sort onchange=\"this.form.submit()\" style=\"width:auto\">";
  const char *keys[] = {"name", "size", "length"};
  const char *labels[] = {"Name", "Size", "Length"};
  for (int k = 0; k < (holdsRecordings ? 3 : 2); k++) {
    body += String("<option value=") + keys[k] + (sortKey == k ? " selected" : "") +
            ">" + labels[k] + "</option>";
  }
  body += "</select>"
          "<select name=order onchange=\"this.form.submit()\" style=\"width:auto\">"
          "<option value=asc" + String(sortDesc ? "" : " selected") + ">Ascending</option>"
          "<option value=desc" + String(sortDesc ? " selected" : "") + ">Descending</option>"
          "</select></form>";

  body += "<form method=post action=/files><table>";
  for (int i = startAt; i < startAt + shown; i++) {
    const FileRow &row = rows[i];
    const String name = row.entry.name;
    const String full = prefix + name;
    String label = name;
    String action;

    if (inDay && label.length() == 6) {
      // 041541 is a time of day once the colons are put back.
      label = label.substring(0, 2) + ":" + label.substring(2, 4) + ":" +
              label.substring(4, 6);
    }

    if (holdsRecordings && row.known && row.bytes > 0) {
      const long durMs = row.durMs;

      String ends;
      // Start comes from the directory name; the end is start plus duration.
      if (name.length() == 6 && durMs > 0) {
        const int h = name.substring(0, 2).toInt();
        const int m = name.substring(2, 4).toInt();
        const int sec = name.substring(4, 6).toInt();
        long endSec = h * 3600L + m * 60L + sec + (durMs + 500) / 1000;
        endSec %= 86400L;
        char buf[16];
        snprintf(buf, sizeof(buf), "%02ld:%02ld:%02ld", endSec / 3600,
                 (endSec % 3600) / 60, endSec % 60);
        ends = String(" to ") + buf;
      }

      action = "<span class=sub>" + String(durMs / 1000.0, 1) + "s" + ends + "</span> " +
               "<span class=sub>" + icon("disk") + formatSize(row.bytes) + "</span> ";
      action += "<a class=btn style=\"padding:3px 9px\" data-tip=\"Play this "
                "recording in the browser\" href=\"/play?dir=" + htmlEscape(full) +
                "\">" + icon("play") + "Play</a> ";
      // The video is inside the directory, so without this the only way to keep
      // a recording was to open it and find the file by hand.
      action += "<a class=btn style=\"padding:3px 9px\" data-tip=\"Download this "
                "recording as an AVI file\" href=\"/video?dir=" + htmlEscape(full) +
                "\">" + icon("down") + "</a>";
    } else if (inDay && row.entry.isDir) {
      // A directory in a day with nothing readable in it is a recording that was
      // cut off before it wrote a frame. Saying so beats an empty cell.
      action = "<span class=sub>no frames</span>";
    }

    String cell = icon(row.entry.isDir ? "folder" : "image") + htmlEscape(label);
    if (row.entry.isDir) {
      cell = "<a href=\"/files?path=" + htmlEscape(full) + sortQS + "\">" + cell + "</a>";
    }

    String size;
    if (row.entry.isDir) {
      size = action;
    } else {
      size = "<span class=sub>" + icon("disk") + formatSize(row.entry.size) +
             "</span> "
             "<a class=btn style=\"padding:3px 9px\" data-tip=\"Download this "
             "file\" href=\"/download?path=" + htmlEscape(full) + "\">" +
             icon("down") + "</a>";
    }

    body += "<tr><td style=\"padding-right:12px\">"
            "<input type=checkbox name=f value=\"" + htmlEscape(full) +
            "\" style=\"width:auto\"></td>"
            "<th>" + cell + "</th><td>" + size + "</td></tr>";
  }
  body += "</table>";
  free(rows);

  // Paging rather than a silent cap. A truncated list that does not say so reads
  // as missing footage.
  if (held > PAGE) {
    body += "<div class=actions>";
    if (startAt > 0) {
      body += "<a class=btn href=\"/files?path=" + htmlEscape(path) + sortQS +
              "&start=" + String(max(0, startAt - PAGE)) + "\">Previous</a>";
    }
    if (startAt + shown < held) {
      body += "<a class=btn href=\"/files?path=" + htmlEscape(path) + sortQS +
              "&start=" + String(startAt + PAGE) + "\">Next</a>";
    }
    body += "<span class=sub style=\"align-self:center\">" + String(startAt + 1) +
            " to " + String(startAt + shown) + " of " + String(held) + "</span></div>";
  }
  if (held < total) {
    body += "<p class=err>" + String(total - held) +
            " more items are here than this page can sort at once.</p>";
  }
  if (tooLong > 0) {
    body += "<p class=err>" + String(tooLong) +
            " item(s) have names too long to list.</p>";
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

  // Bounded by what the list above actually offers rather than by what the enum
  // allows, so nothing can store a size the camera was never meant to run at.
  const int fsize = formField(body, "fsize").toInt();
  if (fsize >= (int)FRAMESIZE_QVGA && fsize <= (int)FRAMESIZE_UXGA) {
    stored.frameSize = (uint8_t)fsize;
  }
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
// Matches formatSize on the device: one decimal below a hundred, none above.
const fmt = b => {
  const units = ['B', 'KB', 'MB', 'GB'];
  let u = 0;
  while (b >= 1024 && u < 3) { b /= 1024; u++; }
  return b.toFixed(u === 0 || b >= 100 ? 0 : 1) + ' ' + units[u];
};

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

// Streaming holds a connection open for as long as someone is watching, and an
// esp_http_server handler runs on its server's own task. A handler that never
// returns is a server that never serves again: with one viewer connected a
// second got no response at all, not even headers, and a viewer that vanished
// without closing cleanly wedged the port until the camera was rebooted, which
// reads as the camera going offline.
//
// So the handler no longer streams. It hands the request to a worker and
// returns, which is what httpd_req_async_handler_begin exists for, and the
// server task goes straight back to accepting connections.
//
// Three workers, because the sensor's frame rate is shared between whoever is
// watching and dividing it much further stops being a live view. That is enough
// for the aggregator, a browser, and one spare.
static constexpr int STREAM_WORKERS = 3;
static constexpr uint32_t STREAM_WORKER_STACK = 6144;

struct StreamJob {
  httpd_req_t *req;
  bool replay;
};

static QueueHandle_t streamJobs = nullptr;

// Atomic, not volatile: the count goes up on the server task and down on three
// worker tasks, and a plain increment is a read, an add and a write. Losing one
// decrement would leak a viewer slot for the life of the boot, which is the same
// shape of failure this whole change exists to remove.
static std::atomic<int> streamViewers{0};

static void streamWorker(void *) {
  for (;;) {
    StreamJob job = {};
    if (xQueueReceive(streamJobs, &job, portMAX_DELAY) != pdTRUE) continue;
    if (job.replay) {
      replayStream(job.req);
    } else {
      liveStream(job.req);
    }
    // Without this the server keeps the socket and eventually stops accepting
    // connections altogether, which is the failure this whole change exists to
    // remove. It runs on every path out of a stream, including a failed one.
    httpd_req_async_handler_complete(job.req);
    streamViewers--;
  }
}

static esp_err_t queueStream(httpd_req_t *req, bool replay) {
  if (!streamJobs || streamViewers >= STREAM_WORKERS) {
    // Said plainly rather than left to time out. A viewer that is turned away
    // knows to try again; one left hanging looks like a camera that has died.
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "all viewer slots are in use", HTTPD_RESP_USE_STRLEN);
  }

  httpd_req_t *copy = nullptr;
  if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
    return httpd_resp_send_500(req);
  }
  // Counted here rather than in the worker: three requests could otherwise all
  // pass the check above before the first of them was counted.
  streamViewers++;
  const StreamJob job = {copy, replay};
  if (xQueueSend(streamJobs, &job, 0) != pdTRUE) {
    streamViewers--;
    httpd_req_async_handler_complete(copy);
    return httpd_resp_send_500(req);
  }
  return ESP_OK;
}

static esp_err_t streamHandler(httpd_req_t *req) {
  // The browser sends the session cookie with the <img> request, because cookies
  // are scoped to the host and ignore the port this server listens on.
  if (!authGuardResource(req)) return ESP_OK;
  return queueStream(req, false);
}

static esp_err_t playStreamHandler(httpd_req_t *req) {
  if (!authGuardResource(req)) return ESP_OK;
  return queueStream(req, true);
}

// How many people are watching, for the status page: a viewer count is the
// first thing to check when a stream will not start.
int streamViewerCount() { return streamViewers; }
int streamViewerLimit() { return STREAM_WORKERS; }

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
      {"/config", HTTP_GET, configJsonHandler},
      {"/restart", HTTP_GET, restartHandler},
      {"/retrycam", HTTP_GET, cameraRetryHandler},
      {"/settings", HTTP_GET, settingsPageHandler},
      {"/settings", HTTP_POST, settingsPostHandler},
      {"/networks", HTTP_GET, networksHandler},
      {"/recording", HTTP_GET, recordingsPageHandler},
      {"/recording", HTTP_POST, recordingsPostHandler},
      {"/recordings", HTTP_GET, recordingsJsonHandler},
      {"/recordings/days", HTTP_GET, recordingsDaysHandler},
      {"/record", HTTP_GET, recordStateHandler},
      {"/record", HTTP_POST, recordHandler},
      {"/image", HTTP_POST, imageHandler},
      {"/files", HTTP_GET, filesPageHandler},
      {"/files", HTTP_POST, filesDeleteHandler},
      {"/download", HTTP_GET, downloadHandler},
      {"/video", HTTP_GET, videoHandler},
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
  // A viewer that cannot take a frame within ten seconds has gone, not slowed
  // down: twenty kilobytes at two a second is nobody watching. Bounding the wait
  // is what stops one dead connection holding a worker for half a minute.
  cfg.send_wait_timeout = 10;
  if (httpd_start(&streamServer, &cfg) != ESP_OK) {
    Serial.println("stream server failed to start");
    return false;
  }
  httpd_uri_t stream = {"/stream", HTTP_GET, streamHandler, nullptr};
  httpd_uri_t replay = {"/playstream", HTTP_GET, playStreamHandler, nullptr};
  registerUri(streamServer, &stream);
  registerUri(streamServer, &replay);

  streamJobs = xQueueCreate(STREAM_WORKERS, sizeof(StreamJob));
  if (!streamJobs) {
    Serial.println("stream workers: queue could not be created");
    return false;
  }
  for (int i = 0; i < STREAM_WORKERS; i++) {
    char name[16];
    snprintf(name, sizeof(name), "stream%d", i);
    if (xTaskCreate(streamWorker, name, STREAM_WORKER_STACK, nullptr, 4, nullptr) != pdPASS) {
      Serial.printf("stream workers: only %d of %d could be started\n", i, STREAM_WORKERS);
      break;
    }
  }
  return true;
}

void webBeginUpdate() { updating = true; }
