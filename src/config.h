#pragma once

#include <Arduino.h>

// Everything the camera needs to come up on its own, held in NVS rather than
// compiled in, so the device can be moved or handed on without a toolchain.
struct Config {
  String cameraName;  // also the mDNS name, so it must be DNS-safe
  String wifiSsid;
  String wifiPass;
  String adminUser;
  String adminSalt;  // hex
  String adminHash;  // hex, PBKDF2-HMAC-SHA256 over salt + password
  // ArduinoOTA authenticates with MD5 over the wire, so it cannot use the
  // PBKDF2 verifier. This is MD5 of the admin password, captured at setup while
  // the plaintext is briefly in hand. Weaker than adminHash by design of the OTA
  // protocol, which is why the browser updater is the preferred path.
  String otaMd5;
  String otaPassword;  // legacy generated password, used when otaMd5 is unset
  // A maintenance window after every restart, the way a router or a tower does
  // it: the access point is up for a fixed time and then closes on its own.
  // Covers a Wi-Fi password that changed, without leaving an AP broadcasting
  // permanently.
  // POSIX TZ string, for example "SAST-2" or "GMT0BST,M3.5.0/1,M10.5.0". Empty
  // means UTC. Stored rather than an offset so daylight saving is handled by
  // the C library instead of by us.
  String timezone;

  // Motion triggering. Sensitivity is the percentage of the scene that must
  // change; the right value depends entirely on what the camera is pointed at,
  // which is why it is stored rather than compiled in.
  bool motionEnabled = false;
  uint8_t motionSensitivity = 20;
  uint8_t recordSeconds = 10;

  // Free space to protect, in MB. When a recording would take the card below
  // this, the oldest recordings are removed first. Zero disables it.
  // Frame size is a framesize_t; quality runs 10 (best) to 63 (worst). Together
  // they decide how much card an hour costs and whether a face is recognisable.
  uint8_t frameSize = 8;  // FRAMESIZE_SVGA
  uint8_t jpegQuality = 12;

  uint16_t keepFreeMb = 512;

  // Motion schedule. Hours are local, and a start later than the end means the
  // window crosses midnight, which is the case people actually want. Days is a
  // bitmask with Sunday as bit 0; 0x7F is every day.
  bool scheduleEnabled = false;
  uint8_t scheduleFromHour = 22;
  uint8_t scheduleToHour = 6;
  uint8_t scheduleDays = 0x7F;

  bool apWindow = true;
  bool configured = false;
};

bool configLoad(Config &out);
bool configSave(const Config &cfg);
void configClear();

// Derives the stored verifier. Deliberately slow: a stored hash is worth
// nothing if a weak password falls to a dictionary in seconds.
String derivePasswordHash(const String &saltHex, const String &password);
String makeSalt();
String md5Hex(const String &input);
bool passwordMatches(const Config &cfg, const String &password);

// Counts resets that happen close together, so repeated taps on the reset button
// mean "let me back in". Held in NVS rather than RTC memory: the reset button
// pulls the chip enable line low, which powers down the RTC domain and takes
// anything stored there with it.
// Reduces a human-typed name to something mDNS will accept: lowercase, letters,
// digits and single hyphens. Returns a MAC-derived fallback if nothing survives.
String sanitizeHostname(const String &raw);

// Rollback bookkeeping. A freshly uploaded image is on trial until it proves it
// can reach the network; only an unconfirmed image is ever a rollback candidate,
// so a brownout loop on already-proven firmware cannot revert it.
struct TrialState {
  String pendingPartition;  // slot on trial, empty when nothing is
  String pendingVersion;
  int boots = 0;
  String rolledBackFrom;  // version that failed, kept for the status page
};

bool trialLoad(TrialState &out);
void trialSave(const TrialState &t);
void trialClear();

int bumpBootCounter();
void clearBootCounter();
