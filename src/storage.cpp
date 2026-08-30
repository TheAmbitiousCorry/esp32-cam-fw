#include <SD_MMC.h>
#include <dirent.h>
#include <sys/stat.h>

#include "storage.h"

// Where the card is mounted. Needed as a string as well as a handle, because
// listing goes through the POSIX layer rather than the Arduino one.
static constexpr char SD_MOUNT[] = "/sdcard";

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

  // A card pulled while the camera is running leaves a mount that answers
  // nothing. Nothing told us, because mounting happened once at boot and was
  // never questioned again, so the status page went on reporting a card and its
  // free space while every recording failed with "no writable card". A total of
  // zero is what a dead mount reports, and it is not a size a real card has.
  if (cachedTotal == 0) {
    Serial.println("SD: the card stopped answering, unmounting");
    SD_MMC.end();
    mounted = false;
    writable = false;
  }
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
  if (!SD_MMC.begin(SD_MOUNT, true)) {
    // Two different problems wear the same failure here, and the second one is
    // the common one: cards larger than 32GB ship formatted exFAT, which this
    // driver cannot read at all. Naming it beats leaving someone to conclude
    // their card or their slot is broken.
    Serial.println("SD: no card, or a filesystem this cannot read. Cards over "
                   "32GB ship as exFAT; reformat as FAT32.");
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

bool sdMounted() {
  refreshSpace(false);  // so asking whether there is a card is asking, not remembering
  return mounted;
}

bool sdRemount() {
  if (mounted) {
    SD_MMC.end();
    mounted = false;
  }
  cachedAt = 0;
  return sdInit();
}
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

  // readdir, rather than the Arduino directory API, which opens every child just
  // to learn its name. An open costs about 150ms on this card, so a day holding
  // a hundred recordings took eight seconds to list. The directory entry already
  // carries the name and whether it is a directory; only a file's size needs
  // asking for, and sizes are not shown for the directories that dominate.
  const String base = String(SD_MOUNT) + (path == "/" ? "" : path);
  DIR *dir = opendir(base.c_str());
  if (!dir) return 0;

  const String prefix = (path == "/" ? String("/") : path + "/");
  int written = 0, seen = 0;
  for (struct dirent *e = readdir(dir); e; e = readdir(dir)) {
    // Hidden names are the firmware's own bookkeeping, plus whatever a desktop
    // left behind the last time the card was in one. Neither is footage, and
    // counting them here would put the paging numbers out by however many there
    // happened to be.
    if (e->d_name[0] == '.') continue;
    seen++;
    if (seen > startAt && written < max) {
      out[written].name = e->d_name;
      out[written].path = prefix + e->d_name;
      out[written].isDir = (e->d_type == DT_DIR);
      out[written].size = 0;
      if (!out[written].isDir) {
        struct stat st;
        if (stat((base + "/" + e->d_name).c_str(), &st) == 0) {
          out[written].size = st.st_size;
        }
      }
      written++;
    }
  }
  closedir(dir);
  if (totalFound) *totalFound = seen;
  return written;
}

int sdScan(const String &path, SdName *out, int max, int *totalFound, int *skipped) {
  if (totalFound) *totalFound = 0;
  if (skipped) *skipped = 0;
  if (!mounted || !sdPathIsSafe(path)) return 0;

  const String base = String(SD_MOUNT) + (path == "/" ? "" : path);
  DIR *dir = opendir(base.c_str());
  if (!dir) return 0;

  int written = 0, seen = 0;
  for (struct dirent *e = readdir(dir); e; e = readdir(dir)) {
    if (e->d_name[0] == '.') continue;
    // A name this long did not come from this firmware, which writes names of
    // eleven characters at most. Counting it separately is what lets the page
    // say so rather than showing a short list and calling it the whole story.
    if (strlen(e->d_name) >= sizeof(out[0].name)) {
      if (skipped) (*skipped)++;
      continue;
    }
    seen++;
    if (written >= max) continue;
    strcpy(out[written].name, e->d_name);
    out[written].isDir = (e->d_type == DT_DIR);
    out[written].size = 0;
    if (!out[written].isDir) {
      struct stat st;
      if (stat((base + "/" + e->d_name).c_str(), &st) == 0) {
        out[written].size = (uint32_t)st.st_size;
      }
    }
    written++;
  }
  closedir(dir);
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

String sdReadSmall(const String &path, size_t maxLen) {
  if (!mounted || !sdPathIsSafe(path)) return "";
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return "";
  String out;
  while (f.available() && out.length() < maxLen) out += (char)f.read();
  f.close();
  return out;
}

bool sdWriteSmall(const String &path, const String &contents) {
  if (!mounted || !sdPathIsSafe(path)) return false;
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) return false;
  const size_t n = f.print(contents);
  f.close();
  return n == contents.length();
}

bool sdAppendSmall(const String &path, const String &contents) {
  if (!mounted || !sdPathIsSafe(path)) return false;
  File f = SD_MMC.open(path, FILE_APPEND);
  if (!f) return false;
  const size_t n = f.print(contents);
  f.close();
  return n == contents.length();
}

String sdReadTail(const String &path, size_t maxLen) {
  if (!mounted || !sdPathIsSafe(path)) return "";
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return "";
  const size_t size = f.size();
  if (size > maxLen) f.seek(size - maxLen);
  String out;
  while (f.available()) out += (char)f.read();
  f.close();
  return out;
}

size_t sdFileSize(const String &path) {
  if (!mounted || !sdPathIsSafe(path)) return 0;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return 0;
  const size_t n = f.size();
  f.close();
  return n;
}

// Sequential read, so a download does not seek back to the start of the file for
// every chunk it sends.
size_t sdReadNext(void *handle, uint8_t *out, size_t len) {
  File *f = (File *)handle;
  if (!f || !*f) return 0;
  return f->read(out, len);
}

void sdCloseRead(void *handle) {
  File *f = (File *)handle;
  if (!f) return;
  f->close();
  delete f;
}

bool sdIndexOpen(const String &path, void **handle) { return sdOpenRead(path, handle); }

bool sdIndexNext(void *handle, uint32_t *offset, uint32_t *length, uint32_t *atMs,
                 uint32_t *fourth) {
  File *f = (File *)handle;
  if (!f || !*f) return false;
  // A line that does not parse is skipped rather than taken for the end of the
  // file. An append that lost power part way through one line would otherwise
  // hide every line written after it.
  while (f->available()) {
    const String line = f->readStringUntil('\n');
    unsigned long extra = 0;
    const int got = sscanf(line.c_str(), "%lu %lu %lu %lu", (unsigned long *)offset,
                           (unsigned long *)length, (unsigned long *)atMs, &extra);
    if (got >= 3) {
      if (fourth) *fourth = got >= 4 ? (uint32_t)extra : 0;
      return true;
    }
  }
  return false;
}

void sdIndexClose(void *handle) { sdCloseRead(handle); }

// Finds the alphabetically first entry, which for date-named directories is the
// oldest. Scanning twice, once for days and once within a day, avoids holding a
// whole card's worth of names in memory.
static bool firstEntry(const String &path, String *outPath, String *outName) {
  SdEntry entries[32];
  int total = 0;
  String best, bestName;
  for (int start = 0;; start += 32) {
    const int n = sdList(path, entries, 32, &total, start);
    if (n == 0) break;
    for (int i = 0; i < n; i++) {
      if (bestName.isEmpty() || entries[i].name < bestName) {
        bestName = entries[i].name;
        best = entries[i].path;
      }
    }
    if (start + n >= total) break;
  }
  if (best.isEmpty()) return false;
  *outPath = best;
  *outName = bestName;
  return true;
}

int sdAgeOut(uint32_t keepFreeMb) {
  if (!mounted || keepFreeMb == 0) return 0;

  int removed = 0;
  // Bounded so a wrong threshold cannot empty the card in one pass. Ageing runs
  // before every recording, so a genuine backlog clears over several of them.
  for (int guard = 0; guard < 20; guard++) {
    refreshSpace(true);
    const uint64_t freeMb = (cachedTotal - cachedUsed) / (1024ULL * 1024ULL);
    if (freeMb >= keepFreeMb) break;

    String dayPath, dayName;
    if (!firstEntry("/rec", &dayPath, &dayName)) break;

    String recPath, recName;
    if (!firstEntry(dayPath, &recPath, &recName)) {
      // A day whose recordings have all been aged out. It is not empty: its
      // summary file is still in there, and that file is hidden, so the listing
      // which just reported the day empty cannot see it and rmdir refuses to
      // remove a directory that is not. removeAt walks the filesystem directly
      // rather than through the listing, so it takes the summary with it.
      //
      // Without this the loop retried the same day on every one of its twenty
      // attempts, freed nothing, and did the same on every recording after.
      // Ageing stopped working permanently, which on a camera means the card
      // fills and recording stops. The return value is checked for the same
      // reason: a delete that cannot succeed must end the pass, not spin.
      if (!removeAt(dayPath, 0)) break;
      continue;
    }

    Serial.printf("ageing out %s, %llu MB free of %lu wanted\n", recPath.c_str(),
                  freeMb, (unsigned long)keepFreeMb);
    if (!removeAt(recPath, 0)) break;
    removed++;
  }

  if (removed) refreshSpace(true);
  return removed;
}
