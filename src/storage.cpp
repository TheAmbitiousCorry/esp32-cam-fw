#include <SD_MMC.h>

#include "storage.h"

static bool mounted = false;
static bool writable = false;

// f_getfree walks the whole allocation table, which on a 32GB card takes long
// enough to make the status page feel broken. The figure only moves when
// something is written, so a cached value refreshed on a timer is honest enough
// for a status display.
static constexpr uint32_t SPACE_CACHE_MS = 30000;
static uint64_t cachedTotal = 0;
static uint64_t cachedUsed = 0;
static uint32_t cachedAt = 0;

static void refreshSpace(bool force) {
  if (!mounted) return;
  if (!force && cachedAt != 0 && millis() - cachedAt < SPACE_CACHE_MS) return;
  cachedTotal = SD_MMC.totalBytes();
  cachedUsed = SD_MMC.usedBytes();
  cachedAt = millis();
}

static bool runWriteTest() {
  const char *path = "/.write-test";

  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) return false;
  const size_t written = f.print("ok");
  f.close();
  if (written != 2) return false;

  f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;
  const String back = f.readString();
  f.close();
  SD_MMC.remove(path);

  return back == "ok";
}

bool sdInit() {
  // The second argument is 1-bit mode. Format-if-empty is left off: silently
  // formatting a card someone just plugged in is not a decision firmware should
  // make on its own.
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("SD: no card, or it could not be mounted");
    mounted = false;
    return false;
  }

  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("SD: slot mounted but no card present");
    SD_MMC.end();
    mounted = false;
    return false;
  }

  mounted = true;
  writable = runWriteTest();
  refreshSpace(true);
  Serial.printf("SD: %s, %llu MB total, %llu MB used, %s\n", sdCardType().c_str(),
                sdTotalBytes() / (1024ULL * 1024ULL), sdUsedBytes() / (1024ULL * 1024ULL),
                writable ? "writable" : "NOT WRITABLE");
  return true;
}

bool sdMounted() { return mounted; }
bool sdWritable() { return writable; }

String sdCardType() {
  switch (SD_MMC.cardType()) {
    case CARD_MMC:  return "MMC";
    case CARD_SD:   return "SDSC";
    case CARD_SDHC: return "SDHC";
    case CARD_NONE: return "none";
    default:        return "unknown";
  }
}

uint64_t sdTotalBytes() {
  refreshSpace(false);
  return mounted ? cachedTotal : 0;
}

uint64_t sdUsedBytes() {
  refreshSpace(false);
  return mounted ? cachedUsed : 0;
}

bool sdMkdir(const String &path) {
  if (!mounted || !sdPathIsSafe(path)) return false;
  return SD_MMC.exists(path) || SD_MMC.mkdir(path);
}

bool sdExists(const String &path) {
  return mounted && sdPathIsSafe(path) && SD_MMC.exists(path);
}

bool sdPathIsSafe(const String &path) {
  if (path.isEmpty() || path[0] != '/') return false;
  if (path.indexOf("..") >= 0) return false;
  return true;
}

int sdList(const String &path, SdEntry *out, int max, int *totalFound, int startAt) {
  if (totalFound) *totalFound = 0;
  if (!mounted || !sdPathIsSafe(path)) return 0;

  File dir = SD_MMC.open(path);
  if (!dir || !dir.isDirectory()) return 0;

  int written = 0, seen = 0;
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    seen++;
    if (seen > startAt && written < max) {
      out[written].name = entry.name();
      out[written].path = entry.path();
      out[written].size = entry.size();
      out[written].isDir = entry.isDirectory();
      written++;
    }
    entry.close();
  }
  dir.close();
  if (totalFound) *totalFound = seen;
  return written;
}

int sdListRoot(SdEntry *out, int max, int *totalFound) {
  return sdList("/", out, max, totalFound, 0);
}

static constexpr int MAX_DELETE_DEPTH = 8;

static bool removeAt(const String &path, int depth) {
  if (depth > MAX_DELETE_DEPTH) return false;

  File f = SD_MMC.open(path);
  if (!f) return false;

  if (!f.isDirectory()) {
    f.close();
    return SD_MMC.remove(path);
  }

  // Collect children before deleting any of them: removing entries while
  // iterating the same directory handle is undefined on FATFS.
  static constexpr int MAX_CHILDREN = 64;
  String children[MAX_CHILDREN];
  int count = 0;
  for (File child = f.openNextFile(); child && count < MAX_CHILDREN;
       child = f.openNextFile()) {
    children[count++] = child.path();
    child.close();
  }
  f.close();

  bool ok = true;
  for (int i = 0; i < count; i++) {
    if (!removeAt(children[i], depth + 1)) ok = false;
  }
  return SD_MMC.rmdir(path) && ok;
}

bool sdRemove(const String &path) {
  if (!mounted || path.isEmpty() || path == "/") return false;
  const bool ok = removeAt(path, 0);
  refreshSpace(true);
  return ok;
}

SdBench sdBenchmark() {
  SdBench r = {};
  r.fileCount = 50;
  r.bytesEach = 15000;  // about the size of an SVGA frame in this scene
  r.ok = false;
  if (!mounted || !writable) return r;

  // One buffer, reused. Allocating per write would measure the allocator rather
  // than the card.
  uint8_t *buf = (uint8_t *)malloc(r.bytesEach);
  if (!buf) return r;
  memset(buf, 0xA5, r.bytesEach);

  SD_MMC.mkdir("/bench");

  const uint32_t t0 = millis();
  for (int i = 0; i < r.fileCount; i++) {
    char path[32];
    snprintf(path, sizeof(path), "/bench/%03d.bin", i);
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) { free(buf); return r; }
    f.write(buf, r.bytesEach);
    f.close();
  }
  r.manyFilesMs = millis() - t0;

  const uint32_t t1 = millis();
  File one = SD_MMC.open("/bench/all.bin", FILE_WRITE);
  if (!one) { free(buf); return r; }
  for (int i = 0; i < r.fileCount; i++) {
    one.write(buf, r.bytesEach);
  }
  one.close();
  r.oneFileMs = millis() - t1;

  free(buf);
  sdRemove("/bench");
  r.ok = true;
  return r;
}

// File handles are passed back as opaque pointers so the web layer can stream a
// recording without including SD_MMC or knowing what a File is.
bool sdOpenRead(const String &path, void **handle) {
  if (!mounted || !sdPathIsSafe(path)) return false;
  File *f = new File(SD_MMC.open(path, FILE_READ));
  if (!*f) {
    delete f;
    return false;
  }
  *handle = f;
  return true;
}

size_t sdReadAt(void *handle, uint32_t offset, uint8_t *out, size_t len) {
  File *f = (File *)handle;
  if (!f || !*f) return 0;
  if (!f->seek(offset)) return 0;
  return f->read(out, len);
}

void sdCloseRead(void *handle) {
  File *f = (File *)handle;
  if (!f) return;
  f->close();
  delete f;
}

bool sdIndexOpen(const String &path, void **handle) { return sdOpenRead(path, handle); }

bool sdIndexNext(void *handle, uint32_t *offset, uint32_t *length, uint32_t *atMs) {
  File *f = (File *)handle;
  if (!f || !*f || !f->available()) return false;
  const String line = f->readStringUntil('\n');
  if (line.length() < 3) return false;
  return sscanf(line.c_str(), "%lu %lu %lu", (unsigned long *)offset,
                (unsigned long *)length, (unsigned long *)atMs) == 3;
}

void sdIndexClose(void *handle) { sdCloseRead(handle); }
