#include <Arduino.h>

#include "statusled.h"

static constexpr int LED_PIN = 33;  // active low
static Status current = Status::Booting;
static uint32_t phaseStartedAt = 0;
static int step = 0;

static void writeLed(bool on) { digitalWrite(LED_PIN, on ? LOW : HIGH); }

void statusLedInit() {
  pinMode(LED_PIN, OUTPUT);
  writeLed(true);  // solid from the first instruction, before anything can fail
  phaseStartedAt = millis();
}

void statusLedSet(Status state) {
  if (state == current) return;
  current = state;
  step = 0;
  phaseStartedAt = millis();
}

// Each pattern is a list of durations in milliseconds, alternating on and off,
// starting with on. Kept as data so a new state is a table entry rather than
// another branch of timing logic.
struct Pattern {
  const uint16_t *steps;
  uint8_t count;
};

static const uint16_t SEARCHING_MS[] = {150, 150};
static const uint16_t ONLINE_MS[] = {60, 2940};
static const uint16_t FAULT_MS[] = {120, 180, 120, 1580};

static Pattern patternFor(Status state) {
  switch (state) {
    case Status::Searching:   return {SEARCHING_MS, 2};
    case Status::Online:      return {ONLINE_MS, 2};
    case Status::CameraFault: return {FAULT_MS, 4};
    case Status::Booting:
    default:                  return {nullptr, 0};
  }
}

void statusLedTick() {
  const Pattern p = patternFor(current);
  if (p.count == 0) {
    writeLed(true);  // Booting is solid, so there is nothing to step through
    return;
  }

  if (millis() - phaseStartedAt < p.steps[step]) return;
  phaseStartedAt = millis();
  step = (step + 1) % p.count;
  writeLed(step % 2 == 0);  // even steps are on, odd are off
}
