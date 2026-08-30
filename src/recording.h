#pragma once

#include <Arduino.h>

// A recording is one file of concatenated JPEGs plus a text index of offsets.
//
// It began as one file per frame, which is more robust still, until measurement
// showed FAT charges 142ms to create a file against 13ms to write 15KB into an
// open one. That capped recording at 4.9 fps while the card can sustain 74.
//
// Robustness survives the change. An interrupted recording loses its tail rather
// than the whole file, every frame before that point is intact, and the index is
// rebuildable by scanning for JPEG start and end markers. An AVI would offer
// none of that: its index is written at close, so an interrupted one is
// unplayable.
//
// Capture runs from the main loop rather than a task. Writing to SD and reading
// from the camera both want the same PSRAM bandwidth, and serialising them keeps
// the failure modes obvious while the throughput is still unknown.
bool recordingStart(uint32_t seconds);
void recordingStop();
void recordingTick();

bool recordingActive();
String recordingDir();
uint32_t recordingFrames();
uint32_t recordingBytes();
float recordingFps();

// Average milliseconds per frame spent waiting for the camera, writing the
// frame, and writing its index entry. Reported so the limit can be found by
// measurement rather than argued about.
void recordingTiming(uint32_t *grabMs, uint32_t *writeMs, uint32_t *indexMs);

// While recording, the recorder is the only thing that reads the camera and it
// publishes each frame here. Two consumers calling esp_camera_fb_get() compete,
// and a tight-looping stream starves a recorder that asks once per loop: 0.5 fps
// against 23. Copying a frame costs about a millisecond; losing it costs the
// recording.
//
// Returns false when nothing newer than `seq` has been published. On success
// `seq` is updated to the frame that was copied.
bool recordingCopyLatest(uint8_t *dst, size_t dstLen, size_t *outLen, uint32_t *seq);
