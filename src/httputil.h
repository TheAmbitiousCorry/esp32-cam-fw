#pragma once

#include <Arduino.h>
#include "esp_http_server.h"

String urlDecode(const String &src);
String formField(const String &body, const String &key);
String htmlEscape(const String &s);

// Reads a form body off the request. Returns false if it is absent or larger
// than maxLen, which is the only sane response to an unbounded POST on a device
// with this much RAM.
bool readBody(httpd_req_t *req, String &out, size_t maxLen = 2048);
