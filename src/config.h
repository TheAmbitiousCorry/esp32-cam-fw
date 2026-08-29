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
  String otaPassword;  // generated, not chosen: espota needs the plaintext
  bool configured = false;
};

bool configLoad(Config &out);
bool configSave(const Config &cfg);
void configClear();

// Derives the stored verifier. Deliberately slow: a stored hash is worth
// nothing if a weak password falls to a dictionary in seconds.
String derivePasswordHash(const String &saltHex, const String &password);
String makeSalt();
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
