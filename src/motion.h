#pragma once

#include <Arduino.h>
#include "esp_camera.h"

// Frame-difference motion detection.
//
// Frames are decoded at one eighth scale and reduced to a grid of block
// averages. Comparing block averages rather than pixels is what makes this
// affordable: an 800x600 frame becomes 80 numbers, and sensor noise averages out
// instead of registering as movement.
//
// This is the trigger that needs no extra hardware. A PIR sensor is better where
// one is fitted: it sees heat rather than pixels, works in darkness, and can
// wake the board from sleep, which frame differencing cannot.
void motionInit();

// Feeds one frame. Returns true when enough of the scene changed. Frames may be
// fed at any rate; the detector compares each against the previous one it saw.
bool motionCheck(camera_fb_t *fb);

// 1 is the most sensitive, 100 the least. Stored so it can be tuned without a
// rebuild, because the right value depends entirely on the scene.
void motionSetSensitivity(uint8_t percent);
uint8_t motionSensitivity();

// Mean brightness of the last frame checked, 0 to 255. The block averages are
// computed for detection anyway, so their mean costs one more division and gives
// auto exposure a measurement of the real scene rather than the sensor's guess.
uint8_t motionBrightness();

// How much of the scene changed on the last frame checked, as a percentage.
// Reported so sensitivity can be set from what the camera actually sees.
uint8_t motionLastChange();

// Whether motion recording is enabled and inside its schedule right now.
bool motionArmed();

// Offered by whoever currently holds the camera. History and detection then ride
// on frames someone else already grabbed, instead of grabbing their own and
// competing: two consumers of this camera do not share fairly, and adding a
// second grabber at 5 fps took a live stream from 17 fps to 0.5.
void motionObserve(camera_fb_t *fb);
