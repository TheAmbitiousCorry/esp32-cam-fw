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

// Removes a file, or a directory and everything under it. Recursion is depth
// limited: a corrupt filesystem should fail a delete, not exhaust the stack.
bool sdRemove(const String &path);
