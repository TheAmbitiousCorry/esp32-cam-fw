# Roadmap

Custom ESP32-CAM security camera firmware, written from scratch on an AI-Thinker
ESP32-CAM (ESP32-D0WD, 4MB flash, 4MB PSRAM, OV2640). Built this way to learn the
stack, with s60sc/ESP32-CAM_MJPEG2SD as a reference implementation to read rather
than to fork.

Domain tags are provisional: no spec directory exists yet.

## Now

- **The card can be cleared without taking it out**: browse what is on the card
  and delete it from the web interface. Why now: the card arrived full of
  H2testw artifacts, there is no card reader to hand, and an inserted card blocks
  USB flashing, so firmware is the only way to free the space that everything
  below depends on. The same page becomes the recordings browser.
  Domains: storage, web. Appetite: small.

- **Footage survives on the SD card and can be played back**: stated goal, and the
  half of "security camera" that outlives a power cut. Why now: motion detection is
  worth nothing until there is somewhere for a trigger to write to, so storage
  precedes it in dependency order. Domains: storage, camera. Appetite: large.

## Next

- **Recording starts without a human asking**: a camera that records on command is
  not a security camera. Covers the trigger source as an open question: frame
  differencing, a PIR sensor, or both. Why now: it is the point of the project, and
  everything after it is refinement. Domains: motion, camera, storage. Appetite: large.

- **Recordings carry a real timestamp**: files named by an uptime counter cannot be
  reviewed after an event. Why now: it changes the file naming and directory
  layout, which is expensive to migrate once there is footage on the card.
  Domains: storage, network. Appetite: small.

- **A full card ages out old footage on its own**: manual deletion covers the
  card being full today; it does not cover the camera filling it again unattended
  at 3am. Why now: the retention rule is cheap to decide while the file layout is
  still being designed and awkward to bolt on afterwards. Domains: storage.
  Appetite: small.

- **Camera settings are tunable from the settings page**: motion sensitivity,
  frame size and exposure are the values worth changing often, and each one
  currently costs a rebuild. Why now: the setup page in Now establishes where
  stored settings live, so this becomes additive rather than structural.
  Domains: config, web. Appetite: small.

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
