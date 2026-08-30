#pragma once

#include <Arduino.h>

// A recording is a directory of numbered JPEGs rather than a video container.
//
// An AVI would play in any media player, but its index is written when the file
// closes, so an interrupted recording leaves a file nothing can open. A
// directory of frames degrades instead: lose power midway and you lose the frame
// being written, not the recording.
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
