# Design principles for this board

Rules extracted from building this firmware, each one paid for by a specific
mistake. Every claim below has a measurement or a log line behind it. Where a
number appears, the command that produced it is named so it can be re-checked.

The board is an AI-Thinker ESP32-CAM: one 240MHz dual-core chip, 4MB PSRAM, one
radio, one camera, a shared SD bus, and two spare GPIO pins.

## Measure before designing, and again before believing

Inference produced the right answer about FAT overhead and the wrong answer four
other times. The difference was cheap: `/sdbench` writes the same bytes as many
files and as one file and reports both.

```
50 files of 15000 bytes
  separate files : 7743 ms  (95 KB/s, 154.9 ms per file)
  one file       : 655 ms  (1118 KB/s)
```

Wrong inferences, all stated confidently first: the camera fault was the ribbon
cable (it was not, twice), stream lag was the access point (an idle one costs
0.9ms), camera contention was buffer starvation (raising `fb_count` from 2 to 3
changed nothing).

**Practice:** put the measurement in the device, behind an endpoint. A benchmark
that ships is a benchmark you can run when the symptom appears.

## Cost is per operation, not per byte

Creating a file on FAT costs 142ms. Writing 15KB into one already open costs
13ms. The card was never slow; it was being asked to do the expensive thing 50
times a second.

The same shape recurs. `f_getfree` walks the whole allocation table, so reporting
free space on a 32GB card is expensive regardless of how few bytes the answer is.
Serving one frame by walking an index costs 1384ms at frame 122 and 127ms when
the offset is supplied.

**Practice:** find the fixed charge per operation, then batch against it. Ask
what the operation costs when the payload is zero.

## One owner per scarce resource

Two consumers calling `esp_camera_fb_get()` do not share fairly. A tight-looping
stream handler starved a recorder asking once per loop:

| | Frames in 10s | Camera wait |
|---|---|---|
| Recorder alone | 230 | 18 ms |
| Recorder while streaming | 4 | 2608 ms |

More buffers did not help, because it was never about buffers. The fix was to
make the recorder the only reader and have it publish a copy for the stream.
Copying 15KB costs about a millisecond.

**Practice:** name one owner for the camera, the radio, and PSRAM bandwidth.
Everything else gets a copy or a turn, never direct access.

## The loop period is a ceiling on everything in it

`loop()` ended with `delay(50)`, and `recordingTick()` ran once per iteration.
That capped recording at 20 fps no matter what the card could do. Removing it
took recording from 16.2 fps to 24.7.

**Practice:** anything driven from `loop()` inherits its period as a hard
maximum. Make the delay conditional on what is actually running.

## A handler that runs until its client leaves cannot be stopped from outside

`httpd_stop()` waits for its handlers. The MJPEG stream handler exits only when a
send fails, which needs the client to disconnect. Calling `httpd_stop()` while
someone watched the live view deadlocked the update path until a power cycle.

This was fixed for the browser updater and then reintroduced in the
over-the-air path, which called the same function.

**Practice:** signal long-running handlers with a flag they poll. Delete the
function that stops them, so it cannot be called by mistake again.

## Assume the API keeps your pointer

`httpd_resp_set_hdr()` stores the pointer, it does not copy. Passing
`makeCookie().c_str()` left a dangling pointer, the `Set-Cookie` header went out
as garbage, and login silently failed with no error anywhere.

**Practice:** in embedded C APIs, assume no copy. Bind to a named local that
outlives the send.

## Silent failures are the expensive ones

`esp_http_server` allows eight URI handlers by default. Registering ten returned
`ESP_ERR_HTTPD_HANDLERS_FULL` for the last two, the return value was never
checked, and two pages simply did not exist. It looked exactly like a firmware
upload that had not worked, and cost two flashes to disbelieve.

**Practice:** check return values on registration and setup calls, and log the
failure. A missing feature should say why.

## Two ways in, always

The SD card blocks USB flashing, because GPIO2 is both an SD data line and a boot
strapping pin. Over-the-air updates then became the only route, until a deadlock
made those fail, at which point the browser uploader was the only route.

Each path rescued the other more than once in a single night.

**Practice:** never let the recovery path share a failure mode with the primary
one. Two independent ways to get firmware onto the device, and a physical reset
gesture that needs neither.

## Bound the blast radius of an update

A new image is on trial until it reaches the network with its servers answering.
If it restarts before confirming, the bootloader restores the previous slot. Only
an image that has never confirmed is a rollback candidate, so a brownout cannot
revert firmware that has already proven itself.

**Practice:** define "healthy" as the property that determines whether you need a
ladder, not as "it booted".

## Prefer degradation to loss

A recording is concatenated JPEGs plus an index written per frame, not an AVI.
An AVI writes its index at close, so an interrupted recording is unplayable in
its entirety. Here an interruption costs the last frame; every frame before it is
intact and the index is rebuildable by scanning for JPEG markers.

The same rule elsewhere: a camera fault no longer takes the network down with it,
because a camera you can reach and diagnose beats one that went silent.

**Practice:** ask what a power cut at the worst moment destroys. If the answer is
"everything since the last close", change the format.

## Power is a feature, and its symptoms lie

One inadequate supply produced four separate faults that each looked like a
different bug: an undetected camera sensor, Wi-Fi associating at -91 dBm having
scanned at -64, a spontaneous reset mid-sentence, and a boot loop 76 resets deep
that never reached application code.

Hours went into firmware explanations for all four.

**Practice:** when symptoms are diverse, intermittent, and resist explanation,
suspect the rail before the code. `rst:0x1 (POWERON_RESET)` appearing during
normal operation is a power event, not a crash.

## Pins are the budget, and shared pins are the trap

Of 16 header pins, two are genuinely free. The scarcity is not the problem;
the sharing is.

| Pin | Shared between | Consequence |
|---|---|---|
| GPIO2 | SD data 0, boot strapping | A card in the slot blocks USB flashing |
| GPIO4 | Flash LED, SD data 1 in 4-bit mode | Use 1-bit mode and keep the LED |
| GPIO12 | SD data 2, flash voltage select | Reads high at boot and the board will not start |
| GPIO0 | Camera clock, bootloader select | Cannot be a button while the camera runs |

**Practice:** map every pin's second function before designing around its first.

## Do not make one side rediscover what the other already knows

The browser held the frame index and asked the device for "frame 122", so the
device walked 122 lines of index to find an offset the browser could have sent.
Supplying the offset took a seek from 1384ms to 152ms.

**Practice:** when both ends have the same data, pass the derived value rather
than recomputing it on the constrained side.

## Throttle anything a human can trigger continuously

A slider fired one request per tick, each queueing behind the last, so the image
was permanently chasing the handle and looked frozen. One request in flight with
the newest target winning fixed it without changing the server.

**Practice:** for anything driven by dragging, typing or scrolling, keep one
request in flight and let the latest intent win.

## Diagnose in layers, cheapest first

The camera being unreachable was diagnosed by working up the stack: ARP said the
device was on the network, a TCP connect said the port was listening, a raw HTTP
request said the server answered in 23ms, which left the client. Each step cost
seconds and eliminated a stratum.

Boot codes did the same job in hardware: `boot:0x13` and `boot:0x1b` differ by
one bit, and that bit is GPIO2, which is how the SD card was identified as the
thing blocking download mode.

**Practice:** learn the cheap signals. `esp_reset_reason()`, the ROM boot code,
ARP, a bare TCP connect. Each answers one question definitively.

## Instrument the device, because the harness lies

Opening the serial port asserts DTR and RTS, which resets the board. Three
separate "the camera is unreachable" conclusions were caused by the act of
measuring. The status page, which reports uptime, signal, heap, sensor state and
recording rate, produced better diagnosis than the cable did, and it works when
the cable is not attached.

**Practice:** build the diagnostics into the product. A status endpoint you can
reach from a phone beats a console you have to be standing next to.
