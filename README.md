<img src="docs/media/argus-eye.svg" alt="" width="52" align="right">

# Argus Cam

**Argus NVR compatible ESP32-CAM firmware.** Flash it once over USB and never
open a serial console again: everything after that happens in a browser, or over
the air.

![Live view, the file browser, motion settings, and status](docs/media/argus-cam.gif)

## What you get

- **24 frames a second at 800x600**, straight from the sensor to your browser,
  and to the SD card at the same time.
- **Motion recording** with a threshold you can see against the live reading, a
  schedule, and seconds of footage kept from *before* the trigger.
- **Recordings that play.** Download one and it opens in any player: the camera
  wraps its frames in a container on the way out, without re-encoding anything.
- **Exposure that follows the light.** One ladder from bright to dark, moving
  only when the scene has genuinely changed, so a passing shadow does not swing
  the picture.
- **Updates that cannot brick it.** An image that fails to reach the network is
  discarded and the previous firmware runs again. The status page names the
  version it reverted from.
- **Recovery you can reach.** Three deliberate presses of the reset button
  restore the setup network, whatever state the settings are in.

Written from scratch, not forked: 6,900 lines, 70% of the flash, no serial
console needed after the first flash. `docs/principles.md` records what this
board taught us with the measurement behind each rule, which is worth reading
before adding anything that touches the camera, the card, or the radio.

## Honestly

It signs you in over plain HTTP. The password is stored as a salted PBKDF2 hash
and compared in constant time, but there is no TLS, so this belongs on a network
you trust. Do not put it on the internet.

To watch several of these on one screen, with one login and one connection per
camera, [Argus NVR](https://github.com/TheAmbitiousCorry/argus-nvr) is the
sibling repository.

## First run

A camera with no stored settings broadcasts its own network.

1. Join the Wi-Fi network named `ESP32CAM-Setup-XXXX`, where `XXXX` comes from the
   camera's MAC address. It has no password.
2. Open `http://192.168.0.1` if the setup page does not appear on its own.
3. Enter a camera name, choose your Wi-Fi network, and set a sign-in password of
   at least 8 characters.
4. Select **Save and restart**.

The camera joins your network and its setup network disappears. Reach it at
`http://<camera-name>.local`, or at the address your router assigns.

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
