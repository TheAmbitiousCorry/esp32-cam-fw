#include <MD5Builder.h>
#include "esp_camera.h"
#include <Preferences.h>
#include "mbedtls/pkcs5.h"

#include "config.h"

static constexpr char NVS_NAMESPACE[] = "camcfg";
static constexpr char NVS_BOOT_NAMESPACE[] = "camboot";
static constexpr char NVS_TRIAL_NAMESPACE[] = "camtrial";

// PBKDF2 rounds. Enough to make offline guessing expensive, few enough that a
// login stays under a second on a 240MHz core.
static constexpr int PBKDF2_ROUNDS = 20000;
static constexpr size_t HASH_BYTES = 32;
static constexpr size_t SALT_BYTES = 16;

static String toHex(const uint8_t *bytes, size_t len) {
  static const char digits[] = "0123456789abcdef";
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += digits[bytes[i] >> 4];
    out += digits[bytes[i] & 0x0F];
  }
  return out;
}

static size_t fromHex(const String &hex, uint8_t *out, size_t maxLen) {
  const size_t n = hex.length() / 2;
  if (n > maxLen) return 0;
  for (size_t i = 0; i < n; i++) {
    out[i] = (uint8_t)strtoul(hex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);
  }
  return n;
}

String makeSalt() {
  uint8_t salt[SALT_BYTES];
  // esp_random is backed by the hardware RNG once the radio is up, which it is
  // by the time anyone is setting a password.
  for (size_t i = 0; i < SALT_BYTES; i++) salt[i] = (uint8_t)(esp_random() & 0xFF);
  return toHex(salt, SALT_BYTES);
}

String derivePasswordHash(const String &saltHex, const String &password) {
  uint8_t salt[SALT_BYTES];
  const size_t saltLen = fromHex(saltHex, salt, sizeof(salt));
  if (saltLen == 0) return "";

  uint8_t out[HASH_BYTES];
  const int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
      MBEDTLS_MD_SHA256, (const unsigned char *)password.c_str(), password.length(),
      salt, saltLen, PBKDF2_ROUNDS, HASH_BYTES, out);
  if (rc != 0) return "";
  return toHex(out, HASH_BYTES);
}

bool passwordMatches(const Config &cfg, const String &password) {
  if (cfg.adminHash.isEmpty() || cfg.adminSalt.isEmpty()) return false;
  const String candidate = derivePasswordHash(cfg.adminSalt, password);
  if (candidate.length() != cfg.adminHash.length()) return false;

  // Constant time over the compared bytes, so a wrong password does not leak how
  // much of it was right through timing.
  uint8_t diff = 0;
  for (size_t i = 0; i < candidate.length(); i++) {
    diff |= (uint8_t)(candidate[i] ^ cfg.adminHash[i]);
  }
  return diff == 0;
}

static volatile uint32_t revision = 0;
uint32_t configRevision() { return revision; }

bool configLoad(Config &out) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return false;

  out.cameraName = prefs.getString("name", "");
  out.wifiSsid = prefs.getString("ssid", "");
  out.wifiPass = prefs.getString("pass", "");
  out.adminUser = prefs.getString("user", "");
  out.adminSalt = prefs.getString("salt", "");
  out.adminHash = prefs.getString("hash", "");
  out.otaPassword = prefs.getString("otapw", "");
  out.otaMd5 = prefs.getString("otamd5", "");
  out.timezone = prefs.getString("tz", "");
  out.motionEnabled = prefs.getBool("moten", false);
  out.motionSensitivity = prefs.getUChar("motsens", 20);
  out.recordSeconds = prefs.getUChar("recsec", 10);
  out.prerollSeconds = prefs.getUChar("presec", 5);
  out.quietSeconds = prefs.getUChar("quietsec", 5);
  out.frameSize = prefs.getUChar("fsize", (uint8_t)FRAMESIZE_SVGA);
  out.jpegQuality = prefs.getUChar("jq", 12);
  out.autoImage = prefs.getBool("auto", true);
  out.aeLevel = (int8_t)prefs.getChar("ael", 0);
  out.gainCeiling = prefs.getUChar("gc", 0);
  out.brightness = (int8_t)prefs.getChar("bri", 0);
  out.contrast = (int8_t)prefs.getChar("con", 0);
  out.saturation = (int8_t)prefs.getChar("sat", 0);
  out.wbMode = prefs.getUChar("wb", 0);
  out.grayscale = prefs.getBool("gray", false);
  out.hmirror = prefs.getBool("hmir", false);
  out.vflip = prefs.getBool("vflip", false);
  out.flashLevel = prefs.getUChar("flash", 60);
  out.flashOnMotion = prefs.getBool("flashmot", false);
  out.keepFreeMb = prefs.getUShort("keepfree", 512);
  out.scheduleEnabled = prefs.getBool("schen", false);
  out.scheduleFromHour = prefs.getUChar("schfrom", 22);
  out.scheduleToHour = prefs.getUChar("schto", 6);
  out.scheduleDays = prefs.getUChar("schdays", 0x7F);
  out.apWindow = prefs.getBool("apwin", true);
  out.configured = prefs.getBool("done", false);
  prefs.end();

  // A half-written config is worse than none: it would send the camera looking
  // for a network with no way to authenticate anyone who came to fix it.
  if (out.wifiSsid.isEmpty() || out.adminHash.isEmpty()) out.configured = false;
  if (out.cameraName.isEmpty()) out.cameraName = sanitizeHostname("");
  return out.configured;
}

bool configSave(const Config &cfg) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;

  prefs.putString("name", cfg.cameraName);
  prefs.putString("ssid", cfg.wifiSsid);
  prefs.putString("pass", cfg.wifiPass);
  prefs.putString("user", cfg.adminUser);
  prefs.putString("salt", cfg.adminSalt);
  prefs.putString("hash", cfg.adminHash);
  prefs.putString("otapw", cfg.otaPassword);
  prefs.putString("otamd5", cfg.otaMd5);
  prefs.putString("tz", cfg.timezone);
  prefs.putBool("moten", cfg.motionEnabled);
  prefs.putUChar("motsens", cfg.motionSensitivity);
  prefs.putUChar("recsec", cfg.recordSeconds);
  prefs.putUChar("presec", cfg.prerollSeconds);
  prefs.putUChar("quietsec", cfg.quietSeconds);
  prefs.putUChar("fsize", cfg.frameSize);
  prefs.putUChar("jq", cfg.jpegQuality);
  prefs.putBool("auto", cfg.autoImage);
  prefs.putChar("ael", cfg.aeLevel);
  prefs.putUChar("gc", cfg.gainCeiling);
  prefs.putChar("bri", cfg.brightness);
  prefs.putChar("con", cfg.contrast);
  prefs.putChar("sat", cfg.saturation);
  prefs.putUChar("wb", cfg.wbMode);
  prefs.putBool("gray", cfg.grayscale);
  prefs.putBool("hmir", cfg.hmirror);
  prefs.putBool("vflip", cfg.vflip);
  prefs.putUChar("flash", cfg.flashLevel);
  prefs.putBool("flashmot", cfg.flashOnMotion);
  prefs.putUShort("keepfree", cfg.keepFreeMb);
  prefs.putBool("schen", cfg.scheduleEnabled);
  prefs.putUChar("schfrom", cfg.scheduleFromHour);
  prefs.putUChar("schto", cfg.scheduleToHour);
  prefs.putUChar("schdays", cfg.scheduleDays);
  prefs.putBool("apwin", cfg.apWindow);
  prefs.putBool("done", true);
  prefs.end();
  revision++;
  return true;
}

void configClear() {
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, false)) {
    prefs.clear();
    prefs.end();
  }

  // Sessions outlive a reboot now, so wiping the credentials has to wipe them
  // too. Otherwise a factory reset would leave a cookie that still signs in.
  if (prefs.begin("camauth", false)) {
    prefs.clear();
    prefs.end();
  }
}

int bumpBootCounter() {
  Preferences prefs;
  if (!prefs.begin(NVS_BOOT_NAMESPACE, false)) return 0;
  const int next = prefs.getInt("n", 0) + 1;
  prefs.putInt("n", next);
  prefs.end();
  return next;
}

void clearBootCounter() {
  Preferences prefs;
  if (prefs.begin(NVS_BOOT_NAMESPACE, false)) {
    prefs.putInt("n", 0);
    prefs.end();
  }
}

String sanitizeHostname(const String &raw) {
  String out;
  out.reserve(raw.length());
  for (size_t i = 0; i < raw.length() && out.length() < 32; i++) {
    char ch = raw[i];
    if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    if (ok) {
      out += ch;
    } else if (!out.isEmpty() && out[out.length() - 1] != '-') {
      // Spaces, underscores and punctuation all collapse to a single hyphen.
      out += '-';
    }
  }
  while (!out.isEmpty() && out[out.length() - 1] == '-') out.remove(out.length() - 1);

  if (out.isEmpty()) {
    const uint64_t chipId = ESP.getEfuseMac();
    char fallback[20];
    snprintf(fallback, sizeof(fallback), "esp32cam-%02x%02x",
             (uint8_t)((chipId >> 32) & 0xFF), (uint8_t)((chipId >> 40) & 0xFF));
    out = fallback;
  }
  return out;
}

bool trialLoad(TrialState &out) {
  Preferences prefs;
  if (!prefs.begin(NVS_TRIAL_NAMESPACE, true)) return false;
  out.pendingPartition = prefs.getString("part", "");
  out.pendingVersion = prefs.getString("ver", "");
  out.boots = prefs.getInt("boots", 0);
  out.rolledBackFrom = prefs.getString("failed", "");
  prefs.end();
  return !out.pendingPartition.isEmpty();
}

void trialSave(const TrialState &t) {
  Preferences prefs;
  if (!prefs.begin(NVS_TRIAL_NAMESPACE, false)) return;
  prefs.putString("part", t.pendingPartition);
  prefs.putString("ver", t.pendingVersion);
  prefs.putInt("boots", t.boots);
  prefs.putString("failed", t.rolledBackFrom);
  prefs.end();
}

void trialClear() {
  // Deliberately keeps "failed": the record of a rollback is the whole point of
  // reporting one, and it must outlive the trial that produced it.
  Preferences prefs;
  if (prefs.begin(NVS_TRIAL_NAMESPACE, false)) {
    prefs.remove("part");
    prefs.remove("ver");
    prefs.remove("boots");
    prefs.end();
  }
}

String md5Hex(const String &input) {
  MD5Builder md5;
  md5.begin();
  md5.add(input);
  md5.calculate();
  return md5.toString();
}
