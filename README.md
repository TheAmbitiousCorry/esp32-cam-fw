<img src="docs/media/argus-eye.svg" alt="" width="52" align="right">

# Argus Cam

**Argus NVR compatible ESP32-CAM firmware.** Flash it once over USB, then set it
up from your phone like any other device. No serial console, no config file, no
credentials compiled in, and no cable ever again.

![Live view, the file browser, motion settings, and status](docs/media/argus-cam.gif)

## It sets itself up

A camera with no settings broadcasts its own open network, `ESP32CAM-Setup-XXXX`,
named from its MAC so two cameras in a box are never the same one. Join it from
a phone and the setup page opens by itself.

It has already scanned for networks, so yours is in a list rather than something
to type. It proposes a name for the camera, drawn from Greek myth and picked
from the board's own MAC, so the same camera always suggests the same name and
two of them never collide. Set a password, save, and the camera joins your
network and disappears from the air.

From then on it is `http://<name>.local`, and the only thing you ever needed a
cable for was the first flash.

**It stays reachable when the network does not.** The setup access point reopens
for fifteen minutes after every restart, so a camera whose Wi-Fi has changed
underneath it is still something you can walk up to, not something you take down
from the wall. It closes on its own once the camera has held a connection for a
minute, because both share one radio.

**And when all else fails, three presses.** Hold the reset button for a second,
three times, and it clears its settings and comes back as a new camera. No
serial console, no reflash, and nothing to remember but the button already on
the board.

## What it does

**Live view** at 24 frames a second, with digital zoom that follows where you
click and drag, and a still capture that can fire the flash for that one frame.

**Records to the SD card** on motion, keeping seconds of footage from *before*
the trigger so an event does not start with the subject already in shot. A
recording extends itself while movement continues and stops once the scene has
been still. The threshold is shown against the live reading, so it can be set
against what the camera is actually seeing, and a schedule decides when it arms.

**Plays back in the browser**, scrubbing through a recording at the speed it was
captured, with a date filter and sorting by name, size or length. Download one
and it opens in any player: the frames are wrapped in a container on the way
out, with nothing re-encoded and the real frame rate written into it.

**Adjusts its own exposure**, one ladder from bright to dark, moving only after
two readings agree so a passing shadow does not swing the picture. It reports
where it has settled, and what the sensor is actually doing rather than what was
last stored.

**Exposes every setting** through the web interface and as JSON: resolution,
quality, brightness, contrast, saturation, white balance, mirroring, flash
brightness, motion sensitivity, schedule, retention.

**Ages footage out** to keep the card from filling, oldest first.

**Announces itself** over mDNS as a camera rather than as another web server, so
an aggregator can tell it apart from the router.

## An update cannot brick it

New firmware goes over the air from the browser or from PlatformIO. An image is
on trial until it reaches the network with its web server answering; one that
restarts before confirming is discarded and the previous firmware runs again.
The status page names the version it reverted from, so a failed update is
something you are told about rather than something you discover.

## It tells you the truth about itself

Sensor state is asked of the driver rather than remembered from boot. The SD
card is re-checked rather than assumed, and a card swapped while it runs is
picked up rather than reported as still there. A control the sensor refuses is
greyed out with a note saying so, rather than pretending to work.

## First flash

Requires PlatformIO and a USB cable, once.

```bash
pio run -e esp32cam -t upload
```

Then join `ESP32CAM-Setup-XXXX` and follow the page. Note that an SD card in the
slot blocks USB flashing, because the card holds a boot strapping pin high;
take it out for the first flash, and after that updates go over Wi-Fi anyway.

## Honestly

Written from scratch on an AI-Thinker ESP32-CAM rather than forked: 6,900 lines,
70% of the flash, and every rule in `docs/principles.md` has the measurement
behind it. Worth reading before adding anything that touches the camera, the
card or the radio.

It signs you in over plain HTTP. The password is stored as a salted PBKDF2 hash
and compared in constant time, but there is no TLS, so this belongs on a network
you trust. Do not put it on the internet.

To watch several of these on one screen, with one login, [Argus
NVR](https://github.com/TheAmbitiousCorry/argus-nvr) is the sibling repository.

## Status LED

The small red LED on the back of the module reports what the camera is doing.
It is the only feedback available before the camera reaches the network.

| Pattern | Meaning |
|---|---|
| Solid | Powered and running, not yet on the network |
| Fast blink, about 3 per second | Joining a Wi-Fi network |
| Brief flash every 3 seconds | On the network, serving pages |
| Two quick blinks every 2 seconds | On the network, camera sensor not detected |

Solid that never changes means the firmware started but never reached the
network.

## Factory reset

Resetting erases the Wi-Fi network, the sign-in password, and the update
password. It does not erase the SD card.

1. Press and hold the reset button for about 1 second, then release.
2. Wait about 1 second.
3. Repeat until you have pressed three times.

The camera erases its settings and returns to the setup network.

A brief tap does not reset the chip, so the press does not count. Hold each press
for about a second. Leave less than 5 seconds between presses, or the count
returns to zero.

The reset button on the ESP32-CAM-MB programmer board and the button on the
underside of the module both work.

## Updating firmware

Two paths exist. Both require signing in.

### From a browser

1. Open **Firmware** in the sidebar.
2. Choose a `firmware.bin`.
3. Select **Upload**.

Progress appears in MB. The camera restarts into the new firmware and signs you
out.

### From the command line

Set the address and update password in `platformio.local.ini`, which git ignores:

```ini
[env:esp32cam_ota]
upload_port = 192.168.10.208
upload_flags =
  --host_port=45678
  --auth=<update password from the status page>
```

Then run:

```bash
pio run -e esp32cam_ota -t upload
```

The device opens a connection back to your machine on port 45678. Allow that
port through your firewall from the camera's address.

### If an update fails

A new firmware image is on trial until it reaches the network with its web
server answering. An image that restarts before confirming is discarded, and the
previous firmware runs again. The status page reports `on trial` or `confirmed`,
and names the version of any image that was reverted.

## Recovery access point

The camera opens its setup network for 15 minutes after each restart, so a
camera whose stored Wi-Fi stops working stays reachable. Signing in still
requires the password.

The access point closes early once the camera holds a Wi-Fi connection for 60
seconds, because both share one radio.

To turn it off, clear the checkbox on the **Settings** page.

## Building

Requires PlatformIO. The platform is pinned to a pioarduino release, which
supplies Arduino core 3.x.

```bash
pio run -e esp32cam                 # build
pio run -e esp32cam -t upload       # flash over USB
pio run -e esp32cam_ota -t upload   # flash over Wi-Fi
```

The version string comes from `git describe` at build time and appears on the
status page.

## Design principles

`docs/principles.md` records what this board taught us, with the measurement
behind each rule. Read it before adding anything that touches the camera, the
card, or the radio.

## Hardware notes

### An inserted SD card blocks USB flashing

GPIO2 is both an SD data line and a boot strapping pin. A card in the slot holds
it high, which prevents the download mode that USB flashing needs. Remove the
card to flash over USB, or update over Wi-Fi instead.

### Pin usage

| Pins | Used by |
|---|---|
| 0, 5, 18, 19, 21, 22, 23, 25, 26, 27, 32, 34, 35, 36, 39 | Camera |
| 2, 14, 15 | SD card, 1-bit SDMMC |
| 16 | PSRAM chip select |
| 1, 3 | Serial console |
| 4 | White flash LED |
| 33 | Status LED |
| 12, 13 | Free |

The SD card runs in 1-bit mode. 4-bit mode additionally needs GPIO4 and GPIO12,
and GPIO12 stops the board booting if it reads high at reset.

### Power

The camera draws current spikes when the sensor initialises and when the radio
transmits. Use a 5V supply rated at 1A or more and a short cable. An inadequate
supply produces symptoms that look like unrelated faults: an undetected camera
sensor, weak Wi-Fi signal, and repeated restarts.

## Licence

MIT. See `LICENSE`.
