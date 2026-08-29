#include <Arduino.h>
#include "esp_http_server.h"

#include "camera.h"
#include "web.h"

// An MJPEG stream is one HTTP response that never ends: a multipart body where
// each part is a whole JPEG. The boundary string just has to be something that
// cannot appear in the payload.
#define PART_BOUNDARY "espcamframeboundary"
static const char *STREAM_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static httpd_handle_t pageServer = nullptr;
static httpd_handle_t streamServer = nullptr;

static const char INDEX_HTML[] = R"HTML(<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-CAM</title>
<style>
  body { margin:0; background:#111; color:#eee; font:14px system-ui,sans-serif;
         display:flex; flex-direction:column; align-items:center; gap:12px; padding:16px; }
  img  { max-width:100%; border-radius:6px; background:#000; }
  a    { color:#7cf; }
</style>
<h1>ESP32-CAM</h1>
<img id="v" alt="live view">
<p><a id="snap" href="/capture">still image</a></p>
<script>
  // The stream lives on its own port, so build the URL from wherever this page
  // was served rather than hardcoding an address.
  document.getElementById('v').src = 'http://' + location.hostname + ':81/stream';
</script>
)HTML";

static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captureHandler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return httpd_resp_send_500(req);

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  const esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t streamHandler(httpd_req_t *req) {
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

    if (res != ESP_OK) break;  // client went away, which is the normal exit
  }
  return res;
}

bool startWebServers() {
  httpd_uri_t index   = {"/",        HTTP_GET, indexHandler,   nullptr};
  httpd_uri_t capture = {"/capture", HTTP_GET, captureHandler, nullptr};
  httpd_uri_t stream  = {"/stream",  HTTP_GET, streamHandler,  nullptr};

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port = 32768;
  cfg.lru_purge_enable = true;
  if (httpd_start(&pageServer, &cfg) != ESP_OK) {
    Serial.println("page server failed to start");
    return false;
  }
  httpd_register_uri_handler(pageServer, &index);
  httpd_register_uri_handler(pageServer, &capture);

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
  httpd_register_uri_handler(streamServer, &stream);
  return true;
}
