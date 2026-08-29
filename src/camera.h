#pragma once

#include "esp_camera.h"

bool cameraInit();

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
