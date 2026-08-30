#pragma once

#include <Arduino.h>

// Wraps recorded JPEG frames in an AVI container.
//
// A recording is stored as the frames themselves, one after another, with the
// timing kept beside them in an index. That is the right shape for a camera:
// nothing has to be rewritten when a recording ends, so a power cut costs the
// last frame rather than the whole file. It is the wrong shape for a player,
// which finds no timing in the file and guesses: mpv shows it as a folder of
// photos at one frame a second, ffmpeg assumes twenty five.
//
// AVI is the cheapest container that fixes this. It stores JPEG frames exactly
// as they already are, so nothing is decoded or re-encoded, and it costs a fixed
// header plus sixteen bytes an entry, well under one percent of a recording.

// Everything the header needs that cannot be known from a single frame.
struct AviInfo {
  uint32_t frames;
  uint32_t durMs;
  uint16_t width;
  uint16_t height;
  uint32_t maxFrameLen;
  uint32_t movieBytes;  // every frame's chunk header, data and padding
};

// The leading headers are a fixed size, so the buffer for them can be too.
static constexpr size_t AVI_HEADER_BYTES = 224;

// Writes RIFF, the header list and the opening of the frame list. Always
// AVI_HEADER_BYTES long.
void aviWriteHeader(uint8_t *out, const AviInfo &info);

// One frame's chunk header, the eight bytes that precede its JPEG data.
void aviWriteChunkHeader(uint8_t *out, uint32_t frameLen);

// The index that follows the last frame: its own header, then one entry per
// frame. aviWriteIndexEntry advances *offset to where the next frame starts.
void aviWriteIndexHeader(uint8_t *out, uint32_t frames);
void aviWriteIndexEntry(uint8_t *out, uint32_t frameLen, uint32_t *offset);
static constexpr size_t AVI_INDEX_ENTRY_BYTES = 16;
static constexpr size_t AVI_INDEX_HEADER_BYTES = 8;

// Where the first frame's chunk header sits, measured the way the index wants
// it: from the 'movi' tag rather than from the start of the file.
static constexpr uint32_t AVI_FIRST_FRAME_OFFSET = 4;

// How large the wrapped recording will be.
uint32_t aviTotalBytes(const AviInfo &info);

// Reads the frame size out of a JPEG's start-of-frame marker. Recordings made
// at an earlier resolution are still on the card, so this asks the footage
// rather than assuming whatever the camera is set to now.
bool aviJpegSize(const uint8_t *data, size_t len, uint16_t *width, uint16_t *height);
