#pragma once

#include "esp_camera.h"

bool cameraInit();

// A DMA transfer cut short still returns a buffer with a plausible length. The
// start and end of image markers distinguish a whole frame from a partial one.
bool isCompleteJpeg(const camera_fb_t *fb);
