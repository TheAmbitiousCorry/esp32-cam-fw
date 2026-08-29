#include "httputil.h"

String urlDecode(const String &src) {
  String out;
  out.reserve(src.length());
  for (size_t i = 0; i < src.length(); i++) {
    const char c = src[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < src.length()) {
      out += (char)strtoul(src.substring(i + 1, i + 3).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

String formField(const String &body, const String &key) {
  int pos = 0;
  while (pos < (int)body.length()) {
    int amp = body.indexOf('&', pos);
    if (amp < 0) amp = body.length();
    const String pair = body.substring(pos, amp);
    const int eq = pair.indexOf('=');
    if (eq > 0 && urlDecode(pair.substring(0, eq)) == key) {
      return urlDecode(pair.substring(eq + 1));
    }
    pos = amp + 1;
  }
  return "";
}

String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '&') out += "&amp;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

bool readBody(httpd_req_t *req, String &out, size_t maxLen) {
  if (req->content_len == 0 || req->content_len > maxLen) return false;

  out = "";
  out.reserve(req->content_len + 1);
  char chunk[257];
  size_t remaining = req->content_len;
  while (remaining > 0) {
    const size_t want = remaining < sizeof(chunk) - 1 ? remaining : sizeof(chunk) - 1;
    const int got = httpd_req_recv(req, chunk, want);
    if (got <= 0) return false;
    chunk[got] = '\0';
    out += chunk;
    remaining -= got;
  }
  return true;
}
