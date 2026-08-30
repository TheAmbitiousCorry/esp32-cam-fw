#pragma once

#include <Arduino.h>

// Wall-clock time from NTP. Without it recordings can only be named by a
// counter, which says nothing about when anything happened.
//
// The timezone is a POSIX TZ string rather than an offset, so the C library
// handles daylight saving rather than us.
void clockBegin(const String &posixTz);

// False until the first successful sync. Nothing should write a timestamped
// name before this returns true.
bool clockSynced();

// "2026-08-30 04:15:32", or "not synced".
String clockNow();

// "20260830-041532", safe for a filename and sorting chronologically as text.
String clockStamp();

// "2026-08-30" and "041532". Recordings live in a directory per day, so a day
// with hundreds of them does not make a directory nobody can list.
String clockDate();
String clockTime();

// Local hour 0-23 and weekday with Sunday as 0. Both return -1 until the clock
// has synced, so a schedule cannot silently run against the epoch.
int clockHour();
int clockWeekday();
