#include <SD_MMC.h>

#include "storage.h"

static bool mounted = false;
static bool writable = false;

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

uint64_t sdTotalBytes() { return mounted ? SD_MMC.totalBytes() : 0; }
uint64_t sdUsedBytes() { return mounted ? SD_MMC.usedBytes() : 0; }

int sdListRoot(SdEntry *out, int max, int *totalFound) {
  if (totalFound) *totalFound = 0;
  if (!mounted) return 0;

  File root = SD_MMC.open("/");
  if (!root || !root.isDirectory()) return 0;

  int written = 0;
  int seen = 0;
  for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    seen++;
    if (written < max) {
      out[written].name = entry.name();
      out[written].path = entry.path();
      out[written].size = entry.size();
      out[written].isDir = entry.isDirectory();
      written++;
    }
    entry.close();
  }
  root.close();

  if (totalFound) *totalFound = seen;
  return written;
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
  return removeAt(path, 0);
}
