#include <SD_MMC.h>

#include "camera.h"
#include "recording.h"
#include "storage.h"

static constexpr char REC_ROOT[] = "/rec";

// Frames are written as fast as the card accepts them, up to this. Without a
// ceiling a fast card fills 30GB with near-identical frames surprisingly quickly.
static constexpr uint32_t MIN_FRAME_INTERVAL_MS = 100;

static bool active = false;
static String dir;
static uint32_t startedAt = 0;
static uint32_t stopAt = 0;
static uint32_t lastFrameAt = 0;
static uint32_t frames = 0;
static uint32_t bytes = 0;

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

  dir = name;
  startedAt = millis();
  stopAt = startedAt + seconds * 1000;
  lastFrameAt = 0;
  frames = 0;
  bytes = 0;
  active = true;
  Serial.printf("recording to %s for %lus\n", dir.c_str(), (unsigned long)seconds);
  return true;
}

void recordingStop() {
  if (!active) return;
  active = false;
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

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;

  // A frame that failed its integrity check is worse than a missing one: it
  // looks like footage until someone tries to view it.
  if (!isCompleteJpeg(fb)) {
    esp_camera_fb_return(fb);
    return;
  }

  char path[48];
  snprintf(path, sizeof(path), "%s/%05lu.jpg", dir.c_str(), (unsigned long)frames);
  File f = SD_MMC.open(path, FILE_WRITE);
  if (f) {
    const size_t written = f.write(fb->buf, fb->len);
    f.close();
    if (written == fb->len) {
      frames++;
      bytes += written;
    } else {
      Serial.printf("recording: short write, %u of %u bytes. Stopping.\n", written, fb->len);
      esp_camera_fb_return(fb);
      recordingStop();
      return;
    }
  } else {
    Serial.println("recording: could not open frame file. Stopping.");
    esp_camera_fb_return(fb);
    recordingStop();
    return;
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
