#include <time.h>

#include "clock.h"

// pool.ntp.org resolves to a nearby server, and two fallbacks cover it being
// unreachable on a restricted network.
static constexpr char NTP1[] = "pool.ntp.org";
static constexpr char NTP2[] = "time.nist.gov";
static constexpr char NTP3[] = "time.google.com";

// Anything before 2024 means the clock has not been set: the ESP32 starts at
// the epoch, so a plausible year is the test for a real sync.
static constexpr time_t SANE_AFTER = 1704067200;  // 2024-01-01

void clockBegin(const String &posixTz) {
  configTzTime(posixTz.isEmpty() ? "UTC0" : posixTz.c_str(), NTP1, NTP2, NTP3);
  Serial.printf("clock: syncing, timezone %s\n",
                posixTz.isEmpty() ? "UTC0" : posixTz.c_str());
}

bool clockSynced() {
  time_t now = time(nullptr);
  return now > SANE_AFTER;
}

static String formatted(const char *fmt) {
  if (!clockSynced()) return "not synced";
  time_t now = time(nullptr);
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  char buf[32];
  strftime(buf, sizeof(buf), fmt, &tmNow);
  return buf;
}

String clockNow() { return formatted("%Y-%m-%d %H:%M:%S"); }
String clockStamp() { return formatted("%Y%m%d-%H%M%S"); }
