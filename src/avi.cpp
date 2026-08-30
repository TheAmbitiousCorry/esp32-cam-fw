#include "avi.h"

// Everything in an AVI is little endian, and almost every field is a four byte
// integer or a four character tag, so the whole format is these two writers.
static uint8_t *put32(uint8_t *p, uint32_t v) {
  *p++ = (uint8_t)(v);
  *p++ = (uint8_t)(v >> 8);
  *p++ = (uint8_t)(v >> 16);
  *p++ = (uint8_t)(v >> 24);
  return p;
}

static uint8_t *put16(uint8_t *p, uint16_t v) {
  *p++ = (uint8_t)(v);
  *p++ = (uint8_t)(v >> 8);
  return p;
}

static uint8_t *putTag(uint8_t *p, const char *tag) {
  for (int i = 0; i < 4; i++) *p++ = (uint8_t)tag[i];
  return p;
}

// A frame is padded to an even length. The pad byte is not counted in the
// chunk's own size, but everything after it is offset by it, which is the one
// place this format punishes a careless writer.
static uint32_t chunkBytes(uint32_t frameLen) { return 8 + frameLen + (frameLen & 1); }

uint32_t aviTotalBytes(const AviInfo &info) {
  return AVI_HEADER_BYTES + info.movieBytes + AVI_INDEX_HEADER_BYTES +
         AVI_INDEX_ENTRY_BYTES * info.frames;
}

void aviWriteHeader(uint8_t *out, const AviInfo &info) {
  const uint32_t movieList = 4 + info.movieBytes;
  const uint32_t indexBytes = AVI_INDEX_HEADER_BYTES + AVI_INDEX_ENTRY_BYTES * info.frames;
  // 'AVI ', the header list, the frame list and the index.
  const uint32_t riff = 4 + 200 + 8 + movieList + indexBytes;

  // The header describes one rate for the whole recording, but frames are only
  // written when the camera produces one, so the true rate varies. Taking it
  // from the frame count over the measured length puts the end of playback where
  // the recording actually ended, which is what someone checking a timestamp
  // against a clock will notice.
  const uint32_t durMs = info.durMs > 0 ? info.durMs : 1;
  const uint32_t usPerFrame = info.frames > 0 ? (uint32_t)((uint64_t)durMs * 1000 / info.frames) : 40000;

  uint8_t *p = out;
  p = putTag(p, "RIFF");
  p = put32(p, riff);
  p = putTag(p, "AVI ");

  p = putTag(p, "LIST");
  p = put32(p, 192);
  p = putTag(p, "hdrl");

  p = putTag(p, "avih");
  p = put32(p, 56);
  p = put32(p, usPerFrame);
  p = put32(p, info.maxFrameLen * (1000000 / (usPerFrame ? usPerFrame : 1)));  // bytes a second
  p = put32(p, 0);                    // padding granularity
  p = put32(p, 0x10);                 // has an index
  p = put32(p, info.frames);
  p = put32(p, 0);                    // initial frames
  p = put32(p, 1);                    // one stream
  p = put32(p, info.maxFrameLen);
  p = put32(p, info.width);
  p = put32(p, info.height);
  for (int i = 0; i < 4; i++) p = put32(p, 0);  // reserved

  p = putTag(p, "LIST");
  p = put32(p, 116);
  p = putTag(p, "strl");

  p = putTag(p, "strh");
  p = put32(p, 56);
  p = putTag(p, "vids");
  p = putTag(p, "MJPG");
  p = put32(p, 0);            // flags
  p = put16(p, 0);            // priority
  p = put16(p, 0);            // language
  p = put32(p, 0);            // initial frames
  // Scale over rate is a fraction, so the odd frame rates a camera that drops
  // frames actually produces are exact rather than rounded.
  p = put32(p, durMs);
  p = put32(p, info.frames * 1000);
  p = put32(p, 0);            // start
  p = put32(p, info.frames);  // length, in frames
  p = put32(p, info.maxFrameLen);
  p = put32(p, 0xFFFFFFFF);   // quality: use the codec's own
  p = put32(p, 0);            // sample size: frames vary
  p = put16(p, 0);
  p = put16(p, 0);
  p = put16(p, info.width);
  p = put16(p, info.height);

  p = putTag(p, "strf");
  p = put32(p, 40);
  p = put32(p, 40);  // size of this structure
  p = put32(p, info.width);
  p = put32(p, info.height);
  p = put16(p, 1);   // planes
  p = put16(p, 24);  // bits per pixel
  p = putTag(p, "MJPG");
  p = put32(p, (uint32_t)info.width * info.height * 3);
  p = put32(p, 0);  // pixels per metre, horizontal
  p = put32(p, 0);  // pixels per metre, vertical
  p = put32(p, 0);  // colours used
  p = put32(p, 0);  // colours that matter

  p = putTag(p, "LIST");
  p = put32(p, movieList);
  p = putTag(p, "movi");
}

void aviWriteChunkHeader(uint8_t *out, uint32_t frameLen) {
  uint8_t *p = putTag(out, "00dc");
  put32(p, frameLen);
}

void aviWriteIndexHeader(uint8_t *out, uint32_t frames) {
  uint8_t *p = putTag(out, "idx1");
  put32(p, AVI_INDEX_ENTRY_BYTES * frames);
}

void aviWriteIndexEntry(uint8_t *out, uint32_t frameLen, uint32_t *offset) {
  uint8_t *p = putTag(out, "00dc");
  p = put32(p, 0x10);  // every frame is a key frame; JPEG has no other kind
  p = put32(p, *offset);
  p = put32(p, frameLen);
  *offset += chunkBytes(frameLen);
}

bool aviJpegSize(const uint8_t *data, size_t len, uint16_t *width, uint16_t *height) {
  // Walk the markers rather than searching for a byte pair: 0xFFC0 appears
  // inside compressed data often enough that searching finds the wrong one.
  size_t i = 2;  // past the start-of-image marker
  while (i + 9 < len) {
    if (data[i] != 0xFF) return false;
    const uint8_t marker = data[i + 1];
    const uint16_t size = (uint16_t)((data[i + 2] << 8) | data[i + 3]);
    // Every start-of-frame marker but the four that mean something else carries
    // the dimensions in the same place.
    const bool isSOF = (marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 &&
                       marker != 0xC8 && marker != 0xCC;
    if (isSOF) {
      *height = (uint16_t)((data[i + 5] << 8) | data[i + 6]);
      *width = (uint16_t)((data[i + 7] << 8) | data[i + 8]);
      return *width > 0 && *height > 0;
    }
    if (marker == 0xDA || marker == 0xD9) return false;  // into the scan, too late
    if (size < 2) return false;
    i += 2 + size;
  }
  return false;
}
