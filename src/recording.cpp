#include <SD_MMC.h>

#include "camera.h"
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

bool recordingStart(uint32_t seconds) {
  if (active || !sdMounted() || !sdWritable()) return false;

  if (!SD_MMC.exists(REC_ROOT) && !SD_MMC.mkdir(REC_ROOT)) {
    Serial.println("recording: could not create /rec");
    return false;
  }

  char name[32];
  snprintf(name, sizeof(name), "%s/%lu", REC_ROOT, (unsigned long)nextRecordingNumber());
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
  frames = 0;
  bytes = 0;
  grabTotalMs = 0;
  writeTotalMs = 0;
  indexTotalMs = 0;
  active = true;
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
