#include <SD_MMC.h>

#include "camera.h"
#include "clock.h"
#include "recording.h"
#include "storage.h"

static constexpr char REC_ROOT[] = "/rec";

// The camera tops out near 24 fps, and the card sustains far more than that once
// frames go into one file. This ceiling exists to bound storage rather than to
// match the hardware: at 15KB a frame, 25 fps fills 30GB in about 22 hours.
static constexpr uint32_t MIN_FRAME_INTERVAL_MS = 40;

static bool active = false;
static String dir;
static uint32_t startedAt = 0;
static uint32_t stopAt = 0;
static uint32_t lastFrameAt = 0;
static uint32_t frames = 0;
static uint32_t bytes = 0;

// Both stay open for the life of the recording. Reopening per frame is exactly
// the cost this format exists to avoid.
static File videoFile;
static File indexFile;

// Published for the stream to read while recording owns the camera.
static uint8_t *pubBuf = nullptr;
static size_t pubLen = 0;
static uint32_t pubSeq = 0;
static portMUX_TYPE pubLock = portMUX_INITIALIZER_UNLOCKED;

// A circular byte arena with a ring of descriptors into it. One allocation
// rather than a malloc per frame: frames vary in size, and repeatedly freeing
// and allocating varying blocks fragments PSRAM until a large one cannot be had.
static constexpr int PREROLL_MAX_FRAMES = 200;

struct PreFrame {
  uint32_t off;
  uint32_t len;
  uint32_t at;
};

static uint8_t *preArena = nullptr;
static size_t preArenaSize = 0;
static size_t preWritePos = 0;
static PreFrame preRing[PREROLL_MAX_FRAMES];
static uint32_t preWindowMs = 5000;
static int preHead = 0;   // next slot to write
static int preCount = 0;

static bool (*activityCheck)(camera_fb_t *) = nullptr;
static uint32_t activityQuietMs = 0;
static uint32_t lastActivityCheck = 0;

// While recording, this only decides when to stop. With several seconds of
// required stillness, checking once a second moves the end by under a second and
// costs a fifth of the decodes. Recording is the most loaded moment the firmware
// has, so it is the right place to be frugal.
static constexpr uint32_t ACTIVITY_INTERVAL_MS = 1000;

// A recording that extends while anything moves has no natural end. Traffic past
// a window, or a sensitivity set too low, would otherwise record until the card
// filled.
static constexpr uint32_t MAX_RECORDING_MS = 5UL * 60UL * 1000UL;
static bool triggered = false;

static uint32_t grabTotalMs = 0;
static uint32_t writeTotalMs = 0;
static uint32_t indexTotalMs = 0;

// Directory names come from scanning rather than a stored counter: a counter in
// NVS and the card's contents can disagree after a card swap, and the card is
// the thing that actually holds the recordings.
static uint32_t nextRecordingNumber() {
  File root = SD_MMC.open(REC_ROOT);
  if (!root || !root.isDirectory()) return 1;

  uint32_t highest = 0;
  for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    const uint32_t n = strtoul(entry.name(), nullptr, 10);
    if (n > highest) highest = n;
    entry.close();
  }
  root.close();
  return highest + 1;
}

void prerollInit(size_t bytes) {
  if (preArena) free(preArena);
  preArena = (uint8_t *)ps_malloc(bytes);
  preArenaSize = preArena ? bytes : 0;
  preWritePos = 0;
  preHead = 0;
  preCount = 0;
  Serial.printf("preroll: %u KB buffer %s\n", (unsigned)(bytes / 1024),
                preArena ? "ready" : "FAILED to allocate");
}

// Drops the oldest descriptors whose bytes are about to be overwritten. Without
// this a wrapped write would silently corrupt the frames it landed on.
static void preDropOverlapping(size_t from, size_t len) {
  const size_t to = from + len;
  while (preCount > 0) {
    const int tail = (preHead - preCount + PREROLL_MAX_FRAMES) % PREROLL_MAX_FRAMES;
    const size_t tFrom = preRing[tail].off;
    const size_t tTo = tFrom + preRing[tail].len;
    if (tTo <= from || tFrom >= to) break;  // no overlap
    preCount--;
  }
}

void prerollPush(const camera_fb_t *fb) {
  if (!preArena || !fb || fb->len == 0 || fb->len > preArenaSize) return;

  if (preWritePos + fb->len > preArenaSize) preWritePos = 0;
  preDropOverlapping(preWritePos, fb->len);

  memcpy(preArena + preWritePos, fb->buf, fb->len);
  const uint32_t now = millis();
  preRing[preHead] = {(uint32_t)preWritePos, (uint32_t)fb->len, now};
  preHead = (preHead + 1) % PREROLL_MAX_FRAMES;
  if (preCount < PREROLL_MAX_FRAMES) preCount++;
  preWritePos += fb->len;

  // Age out anything beyond the window. Without this the buffer holds whatever
  // fits in the arena, which is a different number of seconds for every frame
  // size and makes the setting meaningless.
  while (preCount > 1) {
    const int tail = (preHead - preCount + PREROLL_MAX_FRAMES) % PREROLL_MAX_FRAMES;
    if (now - preRing[tail].at <= preWindowMs) break;
    preCount--;
  }
}

void prerollSetWindow(uint32_t seconds) { preWindowMs = seconds * 1000; }

uint32_t prerollFrames() { return preCount; }

uint32_t prerollSeconds() {
  if (preCount < 2) return 0;
  const int tail = (preHead - preCount + PREROLL_MAX_FRAMES) % PREROLL_MAX_FRAMES;
  const int newest = (preHead - 1 + PREROLL_MAX_FRAMES) % PREROLL_MAX_FRAMES;
  return (preRing[newest].at - preRing[tail].at) / 1000;
}

void recordingExtend(uint32_t quietSeconds) {
  if (!active) return;
  if (millis() - startedAt > MAX_RECORDING_MS) return;  // long enough
  const uint32_t target = millis() + quietSeconds * 1000;
  if ((int32_t)(target - stopAt) > 0) stopAt = target;
}

void recordingSetActivityCheck(bool (*fn)(camera_fb_t *), uint32_t quietSeconds) {
  activityCheck = fn;
  activityQuietMs = quietSeconds * 1000;
}

bool recordingWasTriggered() { return triggered; }
void recordingMarkTriggered() { triggered = true; }

// Writes the buffered history at the head of a new recording, oldest first, so
// the file opens on the approach rather than on the arrival.
static void writePreroll() {
  if (!preArena || preCount == 0) return;

  const int tail = (preHead - preCount + PREROLL_MAX_FRAMES) % PREROLL_MAX_FRAMES;
  const uint32_t base = preRing[tail].at;

  for (int i = 0; i < preCount; i++) {
    const PreFrame &f = preRing[(tail + i) % PREROLL_MAX_FRAMES];
    const uint32_t offset = bytes;
    if (videoFile.write(preArena + f.off, f.len) != f.len) return;
    // Timestamps are shifted so the recording starts at zero and the trigger
    // lands wherever it actually happened within it.
    indexFile.printf("%lu %lu %lu\n", (unsigned long)offset, (unsigned long)f.len,
                     (unsigned long)(f.at - base));
    frames++;
    bytes += f.len;
  }
  Serial.printf("preroll: wrote %d frames covering %lus\n", preCount,
                (unsigned long)((preRing[(preHead - 1 + PREROLL_MAX_FRAMES) %
                                          PREROLL_MAX_FRAMES].at - base) / 1000));
}

bool recordingStart(uint32_t seconds) {
  if (active || !sdMounted() || !sdWritable()) return false;

  if (!SD_MMC.exists(REC_ROOT) && !SD_MMC.mkdir(REC_ROOT)) {
    Serial.println("recording: could not create /rec");
    return false;
  }

  // A timestamp names the recording after when it happened and sorts
  // chronologically as text. A counter says nothing about either, so it is only
  // the fallback for a camera that has not reached an NTP server yet.
  // A directory per day. A flat directory of every recording ever made becomes
  // unlistable within a week of motion triggering, and grouping by day is what
  // the date filter already wants.
  char name[64];
  if (clockSynced()) {
    const String dayDir = String(REC_ROOT) + "/" + clockDate();
    if (!sdMkdir(dayDir)) {
      Serial.printf("recording: could not create %s\n", dayDir.c_str());
      return false;
    }
    snprintf(name, sizeof(name), "%s/%s", dayDir.c_str(), clockTime().c_str());
  } else {
    snprintf(name, sizeof(name), "%s/%lu", REC_ROOT, (unsigned long)nextRecordingNumber());
  }
  if (!SD_MMC.mkdir(name)) {
    Serial.printf("recording: could not create %s\n", name);
    return false;
  }

  videoFile = SD_MMC.open(String(name) + "/video.mjpeg", FILE_WRITE);
  indexFile = SD_MMC.open(String(name) + "/index.txt", FILE_WRITE);
  if (!videoFile || !indexFile) {
    Serial.printf("recording: could not open files in %s\n", name);
    if (videoFile) videoFile.close();
    if (indexFile) indexFile.close();
    return false;
  }

  dir = name;
  startedAt = millis();
  stopAt = startedAt + seconds * 1000;
  lastFrameAt = 0;
  lastActivityCheck = 0;
  frames = 0;
  bytes = 0;
  grabTotalMs = 0;
  writeTotalMs = 0;
  indexTotalMs = 0;
  active = true;
  triggered = false;

  // Written before the clock starts so the recorded length covers the history
  // plus the event, not just the event.
  writePreroll();
  if (frames) startedAt = millis() - (preRing[(preHead - 1 + PREROLL_MAX_FRAMES) %
                                              PREROLL_MAX_FRAMES].at -
                                      preRing[(preHead - preCount + PREROLL_MAX_FRAMES) %
                                              PREROLL_MAX_FRAMES].at);

  Serial.printf("recording to %s for %lus\n", dir.c_str(), (unsigned long)seconds);
  return true;
}

void recordingStop() {
  if (!active) return;
  active = false;
  if (videoFile) videoFile.close();
  if (indexFile) indexFile.close();
  const float seconds = (millis() - startedAt) / 1000.0f;
  Serial.printf("recording done: %lu frames, %lu KB, %.1f fps over %.1fs\n",
                (unsigned long)frames, (unsigned long)(bytes / 1024), recordingFps(), seconds);
}

void recordingTick() {
  if (!active) return;

  if ((int32_t)(millis() - stopAt) >= 0) {
    recordingStop();
    return;
  }
  if (millis() - lastFrameAt < MIN_FRAME_INTERVAL_MS) return;
  lastFrameAt = millis();

  const uint32_t tGrab = millis();
  camera_fb_t *fb = esp_camera_fb_get();
  grabTotalMs += millis() - tGrab;
  if (!fb) return;

  // A frame that failed its integrity check is worse than a missing one: it
  // looks like footage until someone tries to view it.
  if (!isCompleteJpeg(fb)) {
    esp_camera_fb_return(fb);
    return;
  }

  const uint32_t offset = bytes;
  const uint32_t tWrite = millis();
  const size_t written = videoFile.write(fb->buf, fb->len);
  writeTotalMs += millis() - tWrite;
  if (written != fb->len) {
    Serial.printf("recording: short write, %u of %u bytes. Stopping.\n", written, fb->len);
    esp_camera_fb_return(fb);
    recordingStop();
    return;
  }

  // Written per frame rather than at the end, so an interrupted recording still
  // has an index for everything up to the interruption. The timestamp is what
  // lets playback run at the speed it was recorded rather than a guessed rate.
  const uint32_t tIndex = millis();
  indexFile.printf("%lu %lu %lu\n", (unsigned long)offset, (unsigned long)fb->len,
                   (unsigned long)(millis() - startedAt));
  indexTotalMs += millis() - tIndex;

  frames++;
  bytes += written;

  // Asked on the frame already in hand, so extending costs no extra capture, and
  // asked at the rate movement happens rather than the rate frames arrive.
  if (activityCheck && millis() - lastActivityCheck >= ACTIVITY_INTERVAL_MS) {
    lastActivityCheck = millis();
    if (activityCheck(fb)) recordingExtend(activityQuietMs / 1000);
  }

  // Publish a copy so a viewer sees what is being recorded without competing for
  // the camera. Allocated once, on the first frame, sized for this recording.
  if (!pubBuf) pubBuf = (uint8_t *)ps_malloc(200 * 1024);
  if (pubBuf && fb->len <= 200 * 1024) {
    memcpy(pubBuf, fb->buf, fb->len);
    portENTER_CRITICAL(&pubLock);
    pubLen = fb->len;
    pubSeq++;
    portEXIT_CRITICAL(&pubLock);
  }
  esp_camera_fb_return(fb);
}

bool recordingActive() { return active; }
String recordingDir() { return dir; }
uint32_t recordingFrames() { return frames; }
uint32_t recordingBytes() { return bytes; }

float recordingFps() {
  const uint32_t elapsed = (active ? millis() : lastFrameAt) - startedAt;
  if (elapsed == 0 || frames == 0) return 0.0f;
  return (frames * 1000.0f) / elapsed;
}

void recordingTiming(uint32_t *grabMs, uint32_t *writeMs, uint32_t *indexMs) {
  const uint32_t n = frames ? frames : 1;
  if (grabMs) *grabMs = grabTotalMs / n;
  if (writeMs) *writeMs = writeTotalMs / n;
  if (indexMs) *indexMs = indexTotalMs / n;
}

bool recordingCopyLatest(uint8_t *dst, size_t dstLen, size_t *outLen, uint32_t *seq) {
  if (!pubBuf || !dst) return false;

  portENTER_CRITICAL(&pubLock);
  const uint32_t currentSeq = pubSeq;
  const size_t currentLen = pubLen;
  portEXIT_CRITICAL(&pubLock);

  if (currentSeq == *seq || currentLen == 0 || currentLen > dstLen) return false;

  memcpy(dst, pubBuf, currentLen);
  *outLen = currentLen;
  *seq = currentSeq;
  return true;
}
