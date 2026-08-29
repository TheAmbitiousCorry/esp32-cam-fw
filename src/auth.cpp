#include "auth.h"

static constexpr int MAX_SESSIONS = 4;
static constexpr uint32_t SESSION_LIFETIME_MS = 12UL * 60UL * 60UL * 1000UL;
static constexpr size_t TOKEN_HEX_LEN = 32;

struct Session {
  char token[TOKEN_HEX_LEN + 1];
  uint32_t expiresAt;
};

static Session sessions[MAX_SESSIONS];

static String randomToken() {
  static const char digits[] = "0123456789abcdef";
  String out;
  out.reserve(TOKEN_HEX_LEN);
  for (size_t i = 0; i < TOKEN_HEX_LEN / 8; i++) {
    const uint32_t word = esp_random();
    for (int shift = 28; shift >= 0; shift -= 4) out += digits[(word >> shift) & 0xF];
  }
  return out;
}

String authCreateSession() {
  const String token = randomToken();
  const uint32_t now = millis();

  // Reuse an empty or expired slot, otherwise evict whichever expires soonest.
  int slot = 0;
  uint32_t soonest = UINT32_MAX;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].token[0] == '\0' || (int32_t)(now - sessions[i].expiresAt) >= 0) {
      slot = i;
      break;
    }
    if (sessions[i].expiresAt < soonest) {
      soonest = sessions[i].expiresAt;
      slot = i;
    }
  }

  strncpy(sessions[slot].token, token.c_str(), TOKEN_HEX_LEN);
  sessions[slot].token[TOKEN_HEX_LEN] = '\0';
  sessions[slot].expiresAt = now + SESSION_LIFETIME_MS;
  return token;
}

// Reads the sid value out of the Cookie header. Cookies are scoped to a host and
// ignore the port, which is what lets the stream server on 81 accept a session
// issued by the page server on 80.
static String cookieToken(httpd_req_t *req) {
  const size_t len = httpd_req_get_hdr_value_len(req, "Cookie");
  if (len == 0 || len > 512) return "";

  String header;
  {
    char buf[513];
    if (httpd_req_get_hdr_value_str(req, "Cookie", buf, sizeof(buf)) != ESP_OK) return "";
    header = buf;
  }

  int at = header.indexOf("sid=");
  while (at >= 0) {
    // Only match at the start of a cookie, so "xsid=" cannot impersonate "sid=".
    if (at == 0 || header[at - 1] == ' ' || header[at - 1] == ';') {
      int end = header.indexOf(';', at);
      if (end < 0) end = header.length();
      String value = header.substring(at + 4, end);
      value.trim();
      return value;
    }
    at = header.indexOf("sid=", at + 4);
  }
  return "";
}

bool authIsSignedIn(httpd_req_t *req) {
  const String token = cookieToken(req);
  if (token.length() != TOKEN_HEX_LEN) return false;

  const uint32_t now = millis();
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].token[0] == '\0') continue;
    if ((int32_t)(now - sessions[i].expiresAt) >= 0) continue;

    uint8_t diff = 0;
    for (size_t c = 0; c < TOKEN_HEX_LEN; c++) diff |= (uint8_t)(sessions[i].token[c] ^ token[c]);
    if (diff == 0) return true;
  }
  return false;
}

void authEndSession(httpd_req_t *req) {
  const String token = cookieToken(req);
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (token.length() == TOKEN_HEX_LEN && strncmp(sessions[i].token, token.c_str(), TOKEN_HEX_LEN) == 0) {
      sessions[i].token[0] = '\0';
    }
  }
}

String authSessionCookie(const String &token) {
  // HttpOnly keeps the token away from page scripts. Secure is deliberately
  // absent: this is plain HTTP on a LAN, and setting it would stop the cookie
  // being sent at all.
  return "sid=" + token + "; Path=/; Max-Age=43200; HttpOnly; SameSite=Lax";
}

String authClearCookie() { return "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax"; }

bool authGuardPage(httpd_req_t *req) {
  if (authIsSignedIn(req)) return true;
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/login");
  httpd_resp_send(req, "", 0);
  return false;
}

bool authGuardResource(httpd_req_t *req) {
  if (authIsSignedIn(req)) return true;
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, "sign in first", HTTPD_RESP_USE_STRLEN);
  return false;
}
