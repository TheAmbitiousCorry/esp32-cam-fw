#include "img_converters.h"

#include "motion.h"

// The decoded frame is 1/8 scale: 800x600 becomes 100x75. Reducing that to a
// 10x8 grid gives 80 blocks, each averaging about 90 pixels, which is enough to
// swamp sensor noise without losing a person-sized object.
static constexpr int GRID_W = 10;
static constexpr int GRID_H = 8;
static constexpr int BLOCKS = GRID_W * GRID_H;

// A block counts as changed when its average brightness moves by more than this,
// out of 255. Below about 12 the detector fires on the automatic gain control
// adjusting itself.
static constexpr int BLOCK_DELTA = 14;

static uint8_t previous[BLOCKS];
static bool havePrevious = false;
static uint8_t sensitivity = 20;  // percent of blocks that must change
static uint8_t lastChange = 0;

void motionInit() {
  havePrevious = false;
  lastChange = 0;
}

void motionSetSensitivity(uint8_t percent) {
  sensitivity = percent == 0 ? 1 : (percent > 100 ? 100 : percent);
}

uint8_t motionSensitivity() { return sensitivity; }
uint8_t motionLastChange() { return lastChange; }

bool motionCheck(camera_fb_t *fb) {
  if (!fb || fb->format != PIXFORMAT_JPEG) return false;

  // One eighth of SVGA. Allocated per call from PSRAM and freed: motion runs a
  // couple of times a second, not per frame, and holding 15KB permanently for
  // that is a poor trade against fragmenting the heap.
  const int w = fb->width / 8;
  const int h = fb->height / 8;
  if (w < GRID_W || h < GRID_H) return false;

  uint8_t *rgb = (uint8_t *)ps_malloc(w * h * 2);
  if (!rgb) return false;
  if (!jpg2rgb565(fb->buf, fb->len, rgb, JPG_SCALE_8X)) {
    free(rgb);
    return false;
  }

  uint32_t sums[BLOCKS] = {0};
  uint32_t counts[BLOCKS] = {0};
  for (int y = 0; y < h; y++) {
    const int by = (y * GRID_H) / h;
    for (int x = 0; x < w; x++) {
      const uint16_t px = ((uint16_t *)rgb)[y * w + x];
      // RGB565 to brightness, weighted roughly as the eye sees it.
      const uint8_t r = (px >> 11) & 0x1F;
      const uint8_t g = (px >> 5) & 0x3F;
      const uint8_t b = px & 0x1F;
      const uint8_t luma = (uint8_t)((r * 8 * 77 + g * 4 * 150 + b * 8 * 29) >> 8);

      const int bx = (x * GRID_W) / w;
      const int idx = by * GRID_W + bx;
      sums[idx] += luma;
      counts[idx]++;
    }
  }
  free(rgb);

  uint8_t current[BLOCKS];
  for (int i = 0; i < BLOCKS; i++) {
    current[i] = counts[i] ? (uint8_t)(sums[i] / counts[i]) : 0;
  }

  if (!havePrevious) {
    memcpy(previous, current, BLOCKS);
    havePrevious = true;
    return false;
  }

  int changed = 0;
  for (int i = 0; i < BLOCKS; i++) {
    if (abs((int)current[i] - (int)previous[i]) > BLOCK_DELTA) changed++;
  }
  memcpy(previous, current, BLOCKS);

  lastChange = (uint8_t)((changed * 100) / BLOCKS);
  return lastChange >= sensitivity;
}
