#include <Arduino.h>

#include "camera.h"
#include "camera_pins.h"

// The OV2640 has no oscillator of its own. The ESP32 clocks it over XCLK using
// an LEDC PWM channel, which is why the config carries an LEDC timer and channel
// that have nothing to do with LEDs.
static constexpr int XCLK_HZ = 20000000;

// Exposure and gain converge over the first few frames. Frame zero is routinely
// black or blown out, which reads as a dead sensor if you trust it.
static constexpr int WARMUP_FRAMES = 4;

static bool cameraReady = false;
static int currentFrameSize = FRAMESIZE_SVGA;
static int currentQuality = 12;

// Power-down is asserted between attempts. A sensor that did not answer may be
// mid-way through its own power-up, and cycling it is more likely to help than
// asking the same question faster.
static bool cameraTry() {
  camera_config_t cfg = {};

  cfg.pin_pwdn     = PWDN_GPIO_NUM;
  cfg.pin_reset    = RESET_GPIO_NUM;
  cfg.pin_xclk     = XCLK_GPIO_NUM;
  cfg.pin_sccb_sda = SIOD_GPIO_NUM;
  cfg.pin_sccb_scl = SIOC_GPIO_NUM;

  cfg.pin_d7    = Y9_GPIO_NUM;
  cfg.pin_d6    = Y8_GPIO_NUM;
  cfg.pin_d5    = Y7_GPIO_NUM;
  cfg.pin_d4    = Y6_GPIO_NUM;
  cfg.pin_d3    = Y5_GPIO_NUM;
  cfg.pin_d2    = Y4_GPIO_NUM;
  cfg.pin_d1    = Y3_GPIO_NUM;
  cfg.pin_d0    = Y2_GPIO_NUM;
  cfg.pin_vsync = VSYNC_GPIO_NUM;
  cfg.pin_href  = HREF_GPIO_NUM;
  cfg.pin_pclk  = PCLK_GPIO_NUM;

  cfg.xclk_freq_hz = XCLK_HZ;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.ledc_channel = LEDC_CHANNEL_0;

  // The OV2640 encodes JPEG in hardware. A raw format moves that work onto the
  // CPU and costs more than every other feature combined.
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size   = (framesize_t)currentFrameSize;
  cfg.jpeg_quality = currentQuality;

  // Three, not two. With two, a stream holding one buffer while it sends over
  // Wi-Fi leaves a single buffer for everyone else, and a recorder competing for
  // it waits seconds rather than milliseconds. Each buffer costs about 90KB of
  // PSRAM, of which there is 4MB.
  cfg.fb_count    = 3;
  cfg.fb_location = CAMERA_FB_IN_PSRAM;

  // Drop stale frames rather than queueing them. For a live view, current beats
  // complete. Recording will want CAMERA_GRAB_WHEN_EMPTY instead.
  cfg.grab_mode = CAMERA_GRAB_LATEST;

  const esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("esp_camera_init failed: 0x%x (%s)\n", err, esp_err_to_name(err));
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  Serial.printf("sensor PID 0x%02x (0x26 is OV2640)\n", sensor->id.PID);

  for (int i = 0; i < WARMUP_FRAMES; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }
  return true;
}

static constexpr int INIT_ATTEMPTS = 3;

bool cameraInit() {
  for (int attempt = 1; attempt <= INIT_ATTEMPTS; attempt++) {
    if (cameraTry()) {
      if (attempt > 1) Serial.printf("camera came up on attempt %d\n", attempt);
      cameraReady = true;
      return true;
    }
    if (attempt < INIT_ATTEMPTS) {
      Serial.printf("camera attempt %d failed, power cycling the sensor\n", attempt);
      esp_camera_deinit();
      pinMode(PWDN_GPIO_NUM, OUTPUT);
      digitalWrite(PWDN_GPIO_NUM, HIGH);  // asserted: sensor powered down
      delay(150);
      digitalWrite(PWDN_GPIO_NUM, LOW);
      delay(150);
    }
  }
  cameraReady = false;
  return false;
}

bool cameraRetry() {
  esp_camera_deinit();
  cameraReady = false;
  return cameraInit();
}

bool cameraIsReady() { return cameraReady; }

bool isCompleteJpeg(const camera_fb_t *fb) {
  if (fb->len < 4) return false;
  const bool soi = fb->buf[0] == 0xFF && fb->buf[1] == 0xD8;
  const bool eoi = fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9;
  return soi && eoi;
}

// Timer 0 and channel 0 belong to the camera's XCLK, so the flash takes its own.
static constexpr int FLASH_LEDC_CHANNEL = 2;
static constexpr int FLASH_LEDC_FREQ = 5000;
static constexpr int FLASH_LEDC_BITS = 8;
static uint8_t flashDuty = 60;  // out of 255; full duty washes out anything close

static bool flashOn = false;

void flashInit() {
  ledcAttachChannel(LED_GPIO_NUM, FLASH_LEDC_FREQ, FLASH_LEDC_BITS, FLASH_LEDC_CHANNEL);
  ledcWrite(LED_GPIO_NUM, 0);
  flashOn = false;
}

void flashSet(bool on) {
  flashOn = on;
  ledcWrite(LED_GPIO_NUM, on ? flashDuty : 0);
}

bool flashIsOn() { return flashOn; }

void cameraApplySettings(int frameSize, int quality) {
  const bool sizeChanged = (frameSize != currentFrameSize);
  currentFrameSize = frameSize;
  currentQuality = quality;
  if (!cameraReady) return;

  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;

  // Quality is a sensor register and is safe to change while running.
  s->set_quality(s, quality);

  // Frame size is not. The driver's buffers were sized at init, and calling
  // set_framesize() on a running sensor leaves it in a state where a single
  // frame takes thirty seconds. Rebuilding the driver costs the live view about
  // a second and is the only correct way to change geometry.
  if (sizeChanged) {
    Serial.printf("frame size changed, reinitialising the camera\n");
    esp_camera_deinit();
    cameraReady = false;
    cameraReady = cameraInit();
  }
}

int cameraFrameSize() { return currentFrameSize; }
int cameraQuality() { return currentQuality; }

static String unsupported;
static ImageSettings applied;

void cameraApplyImage(const ImageSettings &s) {
  if (!cameraReady) return;
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) return;

  unsupported = "";
  applied = s;
  auto tried = [&](const char *name, int result) {
    // A negative return means this sensor does not implement the control. Saying
    // so beats a slider that moves and does nothing.
    if (result < 0) {
      if (!unsupported.isEmpty()) unsupported += ",";
      unsupported += name;
    }
  };

  tried("ael", sensor->set_ae_level(sensor, s.aeLevel));
  tried("gc", sensor->set_gainceiling(sensor, (gainceiling_t)s.gainCeiling));
  tried("bri", sensor->set_brightness(sensor, s.brightness));
  tried("con", sensor->set_contrast(sensor, s.contrast));
  tried("sat", sensor->set_saturation(sensor, s.saturation));

  // Fixed white balance only takes effect with the gain path on, so the two are
  // set together rather than leaving a mode that silently does nothing.
  sensor->set_awb_gain(sensor, 1);
  tried("wb", sensor->set_wb_mode(sensor, s.wbMode));

  // 2 is grayscale in the effects table. At night colour is mostly noise, and
  // mono compresses smaller as well as looking cleaner.
  tried("gray", sensor->set_special_effect(sensor, s.grayscale ? 2 : 0));
  tried("hmir", sensor->set_hmirror(sensor, s.hmirror ? 1 : 0));
  tried("vflip", sensor->set_vflip(sensor, s.vflip ? 1 : 0));

  if (!unsupported.isEmpty()) {
    Serial.printf("sensor refused: %s\n", unsupported.c_str());
  }
}

String cameraUnsupported() { return unsupported; }
const ImageSettings &cameraCurrentImage() { return applied; }

void flashSetLevel(uint8_t level) {
  flashDuty = level;
  if (flashOn) ledcWrite(LED_GPIO_NUM, flashDuty);
}

uint8_t flashLevel() { return flashDuty; }

// Exposure compensation and gain ceiling both trade darkness for noise, so auto
// treats them as one ladder rather than two independent loops that could fight.
// Rungs 0 to 4 spend exposure compensation, which is free; only above that does
// it start buying brightness with gain, and coming back down sheds the noisy gain
// first. One number, always monotonic in brightness.
static constexpr uint8_t AUTO_MAX_POS = 10;
static constexpr uint8_t AUTO_TARGET = 110;   // mid grey, of 255
static constexpr uint8_t AUTO_DEADBAND = 22;  // wide, so it settles instead of hunting

uint8_t cameraAutoPosition(const ImageSettings &s) {
  if (s.gainCeiling > 0) return (uint8_t)(4 + min<int>(s.gainCeiling, 6));
  return (uint8_t)constrain(s.aeLevel + 2, 0, 4);
}

static void autoApplyPosition(uint8_t pos, ImageSettings &s) {
  s.aeLevel = (int8_t)(-2 + min<int>(pos, 4));
  s.gainCeiling = (uint8_t)(pos > 4 ? pos - 4 : 0);
}

bool cameraAutoStep(uint8_t brightness, ImageSettings &s) {
  // Two readings in a row before moving. A single frame goes dark when someone
  // walks past the lens, and chasing that would swing the whole scene.
  static int8_t pending = 0;

  const int8_t want = brightness < AUTO_TARGET - AUTO_DEADBAND   ? 1
                      : brightness > AUTO_TARGET + AUTO_DEADBAND ? -1
                                                                 : 0;
  if (want == 0 || want != pending) {
    pending = want;
    return false;
  }
  pending = 0;

  const uint8_t pos = cameraAutoPosition(s);
  const int next = constrain(pos + want, 0, AUTO_MAX_POS);
  if (next == pos) return false;

  autoApplyPosition((uint8_t)next, s);
  cameraApplyImage(s);
  Serial.printf("auto exposure: brightness %u, rung %u -> %d\n", brightness, pos, next);
  return true;
}
