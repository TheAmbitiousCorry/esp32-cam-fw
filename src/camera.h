#pragma once

#include "esp_camera.h"

// Retries on failure. The sensor probe over SCCB fails intermittently on this
// board, on identical firmware, roughly half of boots. A retry costs a few
// hundred milliseconds; a failed boot costs the camera until someone notices.
bool cameraInit();

// Tears the driver down and brings it back. Lets a fault be cleared from the
// web interface without a reboot, which matters when the camera is mounted.
bool cameraRetry();

bool cameraIsReady();

// Applied to the running sensor rather than by reinitialising it. Frame size and
// quality are sensor registers, so changing them costs a few milliseconds and no
// dropped connection, which is what makes tuning them interactive.
//
// frameSize is a framesize_t; quality is 10 (best) to 63 (worst).
void cameraApplySettings(int frameSize, int quality);

// Everything here is a sensor register write, so it takes effect on the next
// frame with no reinitialisation. That is what makes these worth tuning by eye
// while watching the picture, and why they are not grouped with frame size.
struct ImageSettings {
  int8_t aeLevel;      // -2..2, exposure compensation
  uint8_t gainCeiling; // 0..6, GAINCEILING_2X..128X
  int8_t brightness;   // -2..2
  int8_t contrast;     // -2..2
  int8_t saturation;   // -2..2
  uint8_t wbMode;      // 0..4 auto, sunny, cloudy, office, home
  bool grayscale;
  bool hmirror;
  bool vflip;
};

void cameraApplyImage(const ImageSettings &s);

// Which controls this sensor refused, as a comma-separated list, or empty.
//
// The generic sensor interface offers thirty-odd setters and the OV2640
// implements a subset; the rest return an error and change nothing. Rather than
// hardcode a list from memory, which is how the frame size labels ended up wrong,
// apply them and report what came back.
String cameraUnsupported();

// What the sensor is actually set to right now. Auto changes exposure and gain
// without writing them to flash, so the stored config is not a truthful answer to
// "what is the camera doing"; this is.
const ImageSettings &cameraCurrentImage();

// One step of auto exposure, given the scene brightness motion detection just
// measured. Returns true when it moved something.
//
// The sensor runs its own auto exposure and gain already, and this does not
// replace them. It corrects the cases their metering gets wrong: a bright window
// behind a dark hallway, or a room lit only by a streetlight, where the sensor
// settles on an average that leaves the part you care about unreadable.
bool cameraAutoStep(uint8_t brightness, ImageSettings &s);

// Where the ladder currently sits, 0 (darkest) to 10 (brightest), for display.
uint8_t cameraAutoPosition(const ImageSettings &s);

// 0 to 255. Full duty blows out anything within a metre and reflects off glass,
// which is why this is a setting rather than on or off.
void flashSetLevel(uint8_t level);
uint8_t flashLevel();
int cameraFrameSize();
int cameraQuality();

// A DMA transfer cut short still returns a buffer with a plausible length. The
// start and end of image markers distinguish a whole frame from a partial one.
bool isCompleteJpeg(const camera_fb_t *fb);

// The same check on a raw buffer, for frames that arrive as bytes and a length
// rather than in a driver frame buffer.
bool isCompleteJpegBuf(const uint8_t *buf, size_t len);

// The white LED on GPIO4. Driven through LEDC rather than a plain digital write
// so brightness is available later; at full duty it is blinding at close range.
// Note GPIO4 doubles as SD data line 1 in 4-bit mode, which is one reason to
// keep the card in 1-bit mode.
void flashInit();
void flashSet(bool on);
bool flashIsOn();
