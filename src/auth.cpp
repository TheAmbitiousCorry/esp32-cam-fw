#include <Preferences.h>
#include <time.h>

#include "auth.h"

static constexpr int MAX_SESSIONS = 4;
static constexpr uint32_t SESSION_LIFETIME_S = 12UL * 60UL * 60UL;
static constexpr size_t TOKEN_HEX_LEN = 32;
static constexpr char NVS_NAMESPACE[] = "camauth";

// Expiry is wall-clock rather than millis(), because millis() restarts with the
// device and these now have to survive that.
struct Session {
  char token[TOKEN_HEX_LEN + 1];
  uint32_t expiresAt;  // unix seconds
};

static Session sessions[MAX_SESSIONS];
static bool loaded = false;

// Sessions used to live only in RAM, so every reboot signed everyone out. That
// looked like principle and was mostly friction: this camera reboots on every
// settings change and every firmware update, and the threat it defended against
// is someone with a stolen cookie on the same home network.
static void persist() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.putBytes("s", sessions, sizeof(sessions));
  prefs.end();
}

static void load() {
  if (loaded) return;
  loaded = true;
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return;
  if (prefs.getBytesLength("s") == sizeof(sessions)) {
    prefs.getBytes("s", sessions, sizeof(sessions));
  }
  prefs.end();
}

// Before the clock syncs, time() is near zero and every session would look
// expired. Treating an unsynced clock as "cannot judge" keeps a valid cookie
// working through the first seconds of a boot.
static bool clockUsable() { return time(nullptr) > 1704067200; }  // 2024-01-01

static bool expired(const Session &s) {
  return clockUsable() && s.expiresAt != 0 && (uint32_t)time(nullptr) > s.expiresAt;
}

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
  load();
  const String token = randomToken();

  // Reuse an empty or expired slot, otherwise evict whichever expires soonest.
  int slot = 0;
  uint32_t soonest = UINT32_MAX;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].token[0] == '\0' || expired(sessions[i])) {
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
  sessions[slot].expiresAt = clockUsable() ? (uint32_t)time(nullptr) + SESSION_LIFETIME_S : 0;
  persist();
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
  load();
  const String token = cookieToken(req);
  if (token.length() != TOKEN_HEX_LEN) return false;

  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].token[0] == '\0') continue;
    if (expired(sessions[i])) continue;

    uint8_t diff = 0;
    for (size_t c = 0; c < TOKEN_HEX_LEN; c++) diff |= (uint8_t)(sessions[i].token[c] ^ token[c]);
    if (diff == 0) return true;
  }
  return false;
}

void authEndSession(httpd_req_t *req) {
  load();
  const String token = cookieToken(req);
  bool changed = false;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (token.length() == TOKEN_HEX_LEN &&
        strncmp(sessions[i].token, token.c_str(), TOKEN_HEX_LEN) == 0) {
      sessions[i].token[0] = '\0';
      changed = true;
    }
  }
  if (changed) persist();
}

void authEndAllSessions() {
  memset(sessions, 0, sizeof(sessions));
  loaded = true;
  persist();
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
