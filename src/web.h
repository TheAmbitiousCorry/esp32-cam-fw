#pragma once

// Starts two servers: the page and stills on port 80, the stream on 81.
bool startWebServers(bool cameraOk);

// Reported by the page so a sensor fault is visible in the interface rather than
// showing up as a silently broken image.
bool webCameraReady();

// The reconnect tally lives in the network loop; the status page reports it.
void webSetReconnects(uint32_t n);

// The reset-press tally recorded at boot, so the status page can confirm a press
// actually registered rather than leaving it to be guessed at.
void webSetBootPress(int presses, int needed);

// Signals any running stream to release the camera before firmware is written.
// Does not stop the servers: a handler that loops until its client disconnects
// cannot be stopped from outside, and httpd_stop() waits for it forever.
void webBeginUpdate();
