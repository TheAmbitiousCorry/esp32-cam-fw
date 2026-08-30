# Roadmap

Custom ESP32-CAM security camera firmware, written from scratch on an AI-Thinker
ESP32-CAM (ESP32-D0WD, 4MB flash, 4MB PSRAM, OV2640). Built this way to learn the
stack, with s60sc/ESP32-CAM_MJPEG2SD as a reference implementation to read rather
than to fork.

Domain tags are provisional: no spec directory exists yet.

## Now

_Empty. The next item comes from Next._

## Next

- **Someone finds out an event happened without opening the web page**: an
  unwatched camera that only stores footage is a recorder, not an alarm. Delivery
  route is deliberately unspecified. Why now: it shapes what metadata the recording
  path has to expose. Domains: network, storage. Appetite: small.

## Later

- **Footage leaves the camera on its own**: a recording that exists only on a
  card in a camera is lost with the camera. What would promote it: the camera
  being mounted somewhere a thief could reach it, or wanting footage without
  fetching it. FTP or a plain HTTP POST to a server on the network; the card
  stops being the only copy. Domains: network, storage. Appetite: small.

- **Several cameras are watched from one screen**: one camera means opening one
  page; four means remembering four addresses and four passwords. What would
  promote it: owning a second camera. Likely shape is a machine on the network
  that discovers cameras by mDNS, holds their credentials, and shows their
  streams together, rather than anything running on the cameras themselves.
  Domains: network. Appetite: large.

- **Zoom shows more detail rather than bigger pixels**: the browser zoom crops
  an image the device already downscaled, so it magnifies without revealing
  anything. Cropping at the sensor before it downscales would give real detail
  at the cost of field of view. What would promote it: wanting to read a face or
  a plate from footage that currently cannot. Held here because it means
  changing sensor windowing at runtime, which is the same class of change as the
  frame size handling that wedged the camera for thirty seconds a frame.
  Domains: camera. Appetite: small.

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
