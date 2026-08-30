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

bool cameraInit() {
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
  cfg.frame_size   = FRAMESIZE_SVGA;
  cfg.jpeg_quality = 12;

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
static constexpr int FLASH_DUTY_ON = 60;  // out of 255; full duty washes out anything close

static bool flashOn = false;

void flashInit() {
  ledcAttachChannel(LED_GPIO_NUM, FLASH_LEDC_FREQ, FLASH_LEDC_BITS, FLASH_LEDC_CHANNEL);
  ledcWrite(LED_GPIO_NUM, 0);
  flashOn = false;
}

void flashSet(bool on) {
  flashOn = on;
  ledcWrite(LED_GPIO_NUM, on ? FLASH_DUTY_ON : 0);
}

bool flashIsOn() { return flashOn; }
