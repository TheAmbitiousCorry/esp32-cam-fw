#pragma once

#include <Arduino.h>

// The microSD slot, mounted over SDMMC in 1-bit mode.
//
// 1-bit rather than 4-bit is deliberate on this board. 4-bit additionally needs
// GPIO4, which drives the flash LED, and GPIO12, which is the strapping pin that
// selects flash voltage and will stop the board booting if it reads high at
// reset. 1-bit costs write speed and buys back both pins and a class of
// boot failure.
//
// GPIO2 is shared with the boot strapping pins, which is why a card in the slot
// blocks USB flashing entirely. That is a property of the board, not a fault.
bool sdInit();

bool sdMounted();
String sdCardType();
uint64_t sdTotalBytes();
uint64_t sdUsedBytes();

// Whether a file could actually be written, read back and removed at boot.
// Mounting proves the card answers; only a write proves it is usable.
bool sdWritable();

// A flat listing of the card's root. Enough to answer "what is already on this
// card", and the same call the recordings browser will need later.
struct SdEntry {
  String name;
  String path;  // full path, which is what delete needs
  uint64_t size;
  bool isDir;
};

// Fills up to `max` entries and returns how many were written. `totalFound` is
// set to the number actually present, which may exceed `max`.
int sdListRoot(SdEntry *out, int max, int *totalFound);

// A directory's entries in a form that can be held by the thousand. Names are
// fixed buffers rather than Strings, so a whole directory sits in PSRAM instead
// of competing for the internal heap everything else allocates from. Sorting a
// listing means holding all of it at once, which is the only reason this exists
// beside sdList.
struct SdName {
  char name[96];
  uint32_t size;
  bool isDir;
};

// Reads a whole directory at once. Returns how many entries were written.
// *totalFound is how many there were, and *skipped counts names too long to
// hold, which are reported rather than quietly dropped.
int sdScan(const String &path, SdName *out, int max, int *totalFound, int *skipped);

// Lists any directory, not just the root. Paths are rejected unless they start
// with a slash and contain no "..", so a crafted request cannot walk outside
// the card.
// startAt skips that many entries, so a directory larger than the page can be
// walked rather than truncated.
int sdList(const String &path, SdEntry *out, int max, int *totalFound, int startAt = 0);
bool sdMkdir(const String &path);

// Deletes oldest-first until the card has at least keepFreeMb free, and returns
// how many recordings were removed.
//
// Oldest is decided by name, which works because recordings are stored as
// /rec/YYYY-MM-DD/HHMMSS and those sort chronologically as text. Reading
// timestamps off the filesystem would be slower and no more correct.
int sdAgeOut(uint32_t keepFreeMb);
bool sdPathIsSafe(const String &path);
bool sdExists(const String &path);

// Playback needs raw reads at an offset. Kept here so web.cpp never touches
// SD_MMC directly.
bool sdOpenRead(const String &path, void **handle);
size_t sdReadAt(void *handle, uint32_t offset, uint8_t *out, size_t len);
void sdCloseRead(void *handle);
size_t sdFileSize(const String &path);

// Reads a small file whole. For the per-recording metadata only: anything that
// might be large belongs in sdOpenRead and chunks.
String sdReadSmall(const String &path, size_t maxLen = 256);
bool sdWriteSmall(const String &path, const String &contents);

// Writes a whole buffer to a file. For a single still image: anything that
// streams belongs in sdOpenRead and chunks.
bool sdWriteFile(const String &path, const uint8_t *data, size_t len);

// Adds a line to the end of a file, creating it if it is not there. The per-day
// summary is append-only on purpose: rewriting it after every recording would
// cost more than the reads it saves.
bool sdAppendSmall(const String &path, const String &contents);

// Reads the last bytes of a file. Recovers how long a recording ran from the end
// of its index without reading the whole thing.
String sdReadTail(const String &path, size_t maxLen);
size_t sdReadNext(void *handle, uint8_t *out, size_t len);

// Reads one line of an index file. Returns false at end of file.
bool sdIndexOpen(const String &path, void **handle);
bool sdIndexNext(void *handle, uint32_t *offset, uint32_t *length, uint32_t *atMs);
void sdIndexClose(void *handle);

// Removes a file, or a directory and everything under it. Recursion is depth
// limited: a corrupt filesystem should fail a delete, not exhaust the stack.
bool sdRemove(const String &path);

// Writes the same number of bytes two ways, many small files against one large
// one, and reports how long each took. Answers whether per-file overhead or raw
// throughput is what limits recording, rather than leaving it to inference.
struct SdBench {
  uint32_t manyFilesMs;
  uint32_t oneFileMs;
  uint32_t bytesEach;
  int fileCount;
  bool ok;
};

SdBench sdBenchmark();
