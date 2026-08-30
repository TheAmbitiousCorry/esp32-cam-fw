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
// Keeps the most recent frames in PSRAM so a recording can begin before its
// trigger. A camera that starts when it sees motion has already missed the
// approach, which is usually the part worth having.
//
// Sized in bytes rather than frames, because a frame is 17KB at 400x296 and
// 54KB at 800x600, and a fixed frame count would mean a wildly different amount
// of history depending on an unrelated setting.
void prerollInit(size_t bytes);

// Frames older than this are dropped, so the setting means what it says. The
// buffer is bounded by both: whichever runs out first, seconds or bytes.
void prerollSetWindow(uint32_t seconds);
void prerollPush(const camera_fb_t *fb);
uint32_t prerollFrames();
uint32_t prerollSeconds();

bool recordingStart(uint32_t seconds);

// Pushes the stop time out to `quietSeconds` from now. Called while something is
// still happening, so a recording lasts as long as the event rather than a fixed
// span decided in advance.
void recordingExtend(uint32_t quietSeconds);

// Called for each captured frame while recording. Returning true extends the
// recording. Set to nullptr for a fixed length.
void recordingSetActivityCheck(bool (*fn)(camera_fb_t *fb), uint32_t quietSeconds);

// Whether the current recording was started by motion rather than by hand.
bool recordingWasTriggered();
void recordingMarkTriggered();
void recordingStop();
void recordingTick();

bool recordingActive();

// True while a recording is running that this camera cannot write, because
// there is no usable card. The event is reported so whoever holds this camera's
// stream can keep it; nothing is stored here.
bool recordingCardless();

// True while the recorder is taking frames of its own. Anything that reads the
// recorder's published frame must ask this rather than recordingActive(), since
// a cardless recording publishes nothing.
bool recordingOwnsCamera();
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
