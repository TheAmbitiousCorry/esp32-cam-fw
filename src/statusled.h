#pragma once

// The small red LED on the back of the module, GPIO33, active low. It is not
// broken out to the header and nothing else uses it, so it costs no pins.
//
// This exists because a camera powered from a power-only cable has no serial
// console: until it reaches the network it cannot tell you anything at all. The
// LED covers exactly that window.
enum class Status {
  Booting,       // solid: powered, firmware running, nothing else known yet
  Searching,     // fast blink: looking for or joining a network
  Online,        // heartbeat: on the network, servers answering
  CameraFault,   // double blink: reachable, but the sensor did not respond
};

void statusLedInit();
void statusLedSet(Status state);

// Non-blocking; call every loop pass.
void statusLedTick();
