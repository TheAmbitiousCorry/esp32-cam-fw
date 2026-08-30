#pragma once

#include "esp_camera.h"

// Retries on failure. The sensor probe over SCCB fails intermittently on this
// board, on identical firmware, roughly half of boots. A retry costs a few
// hundred milliseconds; a failed boot costs the camera until someone notices.
bool cameraInit();

// Tears the driver down and brings it back. Lets a fault be cleared from the
// web interface without a reboot, which matters when the camera is mounted.
bool cameraRetry();

bool cameraIsReady();

// Applied to the running sensor rather than by reinitialising it. Frame size and
// quality are sensor registers, so changing them costs a few milliseconds and no
// dropped connection, which is what makes tuning them interactive.
//
// frameSize is a framesize_t; quality is 10 (best) to 63 (worst).
void cameraApplySettings(int frameSize, int quality);
int cameraFrameSize();
int cameraQuality();

// A DMA transfer cut short still returns a buffer with a plausible length. The
// start and end of image markers distinguish a whole frame from a partial one.
bool isCompleteJpeg(const camera_fb_t *fb);

// The white LED on GPIO4. Driven through LEDC rather than a plain digital write
// so brightness is available later; at full duty it is blinding at close range.
// Note GPIO4 doubles as SD data line 1 in 4-bit mode, which is one reason to
// keep the card in 1-bit mode.
void flashInit();
void flashSet(bool on);
bool flashIsOn();
