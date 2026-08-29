# Roadmap

Custom ESP32-CAM security camera firmware, written from scratch on an AI-Thinker
ESP32-CAM (ESP32-D0WD, 4MB flash, 4MB PSRAM, OV2640). Built this way to learn the
stack, with s60sc/ESP32-CAM_MJPEG2SD as a reference implementation to read rather
than to fork.

Domain tags are provisional: no spec directory exists yet.

## Now

- **Firmware updates without physical access**: an inserted SD card holds GPIO2
  high and blocks USB flashing entirely, so every iteration on the recording code
  costs a card removal and reinsertion. Why now: the storage work below is many
  cycles of exactly that, and OTA needs a dual-slot partition table, which is free
  to change now and requires erasing the card's companion device later.
  Domains: network. Appetite: small.

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

- **A full SD card does not stop the camera recording**: the failure is silent and
  arrives exactly when the camera is least supervised. Why now: it is cheap to
  decide the retention rule early and awkward to bolt on later. Domains: storage.
  Appetite: small.

- **Tuning happens without a reflash**: changing motion sensitivity or frame size
  by editing a header and rebuilding makes each experiment a minutes-long round
  trip, which is how tuning gets abandoned half done. Why now: it pays back across
  the motion work above. Domains: config, web. Appetite: small.

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

- **The web interface is not open to anyone on the network**: What would promote
  it: exposing the camera beyond a trusted LAN, or putting it on a network with
  guests. Domains: web, network. Appetite: small.

- **Recordings are usable in high contrast**: both test frames so far were badly
  exposed, one blown out by a window and one crushed to near-black by a ceiling
  light. A doorway camera faces exactly that scene every day. What would promote
  it: footage from the real mounting position turning out unusable at dawn or
  dusk. Domains: camera. Appetite: small.
