# Roadmap

Custom ESP32-CAM security camera firmware, written from scratch on an AI-Thinker
ESP32-CAM (ESP32-D0WD, 4MB flash, 4MB PSRAM, OV2640). Built this way to learn the
stack, with s60sc/ESP32-CAM_MJPEG2SD as a reference implementation to read rather
than to fork.

Domain tags are provisional: no spec directory exists yet.

## Now

- **Footage survives on the SD card and can be played back**: stated goal, and the
  half of "security camera" that outlives a power cut. Why now: motion detection is
  worth nothing until there is somewhere for a trigger to write to, so storage
  precedes it in dependency order. Domains: storage, camera. Appetite: large.

## Next

- **Someone finds out an event happened without opening the web page**: an
  unwatched camera that only stores footage is a recorder, not an alarm. Delivery
  route is deliberately unspecified. Why now: it shapes what metadata the recording
  path has to expose. Domains: network, storage. Appetite: small.

## Later

- **Detection distinguishes a person from a moving branch**: naive motion
  detection produces false positives that train you to ignore it. What would
  promote it: an always-on machine on the network to run real detection against a
  stream, or enough false positives to make the current trigger useless.
  Domains: motion, network. Appetite: large.

- **Classification happens on the device itself**: no network dependency, no frames
  leaving the house. What would promote it: an ESP32-S3 board. Blocked on hardware,
  not on effort. The AI-Thinker lacks the memory and the vector instructions.
  Domains: camera, motion. Appetite: large.

- **The camera runs where there is no mains socket**: mounting position is
  currently constrained by cable reach. What would promote it: a chosen location
  that has no power, and an acceptance that continuous streaming and battery
  operation are mutually exclusive. Domains: power, camera. Appetite: large.

- **Recordings capture sound as well as picture**: What would promote it: a stated
  need for it. This board is GPIO-constrained and the SD card already competes for
  pins, so it is expensive here specifically. Domains: peripherals, storage.
  Appetite: large.

- **Recordings are usable in high contrast**: both test frames so far were badly
  exposed, one blown out by a window and one crushed to near-black by a ceiling
  light. A doorway camera faces exactly that scene every day. What would promote
  it: footage from the real mounting position turning out unusable at dawn or
  dusk. Domains: camera. Appetite: small.
