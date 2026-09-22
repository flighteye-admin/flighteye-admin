#include "ota.h"
#include "config.h"
#include "devlog.h"
#include "display.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>

static OtaState  s_state = OTA_IDLE;
static uint32_t  s_lastCheckMs = 0;
static bool      s_everChecked = false;

// GitHub's API and release-asset hosts both sit behind public, regularly
// rotated CAs. setInsecure() skips certificate validation rather than
// pinning one specific root - reasonable for firmware pulled from a repo
// you control yourself. Swap in client.setCACert(...) with a pinned root
// if that tradeoff ever matters more than the upkeep of a pinned cert.
static void prepClient(WiFiClientSecure& c) {
  c.setInsecure();
  c.setTimeout(15000);
}

// Compares "3.19" vs "3.20" (leading "v" tolerated) as major.minor.patch -
// a plain string compare would rank "3.9" above "3.19".
static bool isNewer(const String& tagIn, const String& current) {
  String tag = tagIn;
  if (tag.length() && (tag[0] == 'v' || tag[0] == 'V')) tag.remove(0, 1);
  int ta = 0, tb = 0, tc = 0, ca = 0, cb = 0, cc = 0;
  sscanf(tag.c_str(),     "%d.%d.%d", &ta, &tb, &tc);
  sscanf(current.c_str(), "%d.%d.%d", &ca, &cb, &cc);
  if (ta != ca) return ta > ca;
  if (tb != cb) return tb > cb;
  return tc > cc;
}

static bool downloadAndFlash(const String& url, size_t expectedLen) {
  WiFiClientSecure client;
  prepClient(client);
  HTTPClient https;
  https.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); // asset URLs 302 to objects.githubusercontent.com
  https.setUserAgent("FlightEye-ESP32");

  if (!https.begin(client, url)) {
    logf("ota: begin() failed for asset download");
    return false;
  }
  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    logf("ota: asset GET -> HTTP %d", code);
    https.end();
    return false;
  }

  int len = https.getSize();
  if (len <= 0) len = (int)expectedLen;
  if (!Update.begin(len > 0 ? (size_t)len : UPDATE_SIZE_UNKNOWN)) {
    logf("ota: Update.begin failed (%s)", Update.errorString());
    https.end();
    return false;
  }

  // Drives drawOtaProgress() from actual bytes flashed, not bytes downloaded -
  // Update.writeStream() reads and writes in lockstep so the two track closely.
  Update.onProgress([](size_t written, size_t total){
    if (total > 0) drawOtaProgress((int)((written * 100UL) / total));
  });

  WiFiClient* stream = https.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  https.end();

  if (len > 0 && written != (size_t)len) {
    logf("ota: wrote %u of %d bytes - aborting", (unsigned)written, len);
    Update.abort();
    return false;
  }
  if (!Update.end(true) || !Update.isFinished()) {
    logf("ota: Update.end failed (%s)", Update.errorString());
    return false;
  }
  logf("ota: %u bytes flashed OK, rebooting", (unsigned)written);
  return true;
}

static void checkNow() {
  s_state = OTA_CHECKING;
  s_lastCheckMs = millis();
  s_everChecked = true;

  WiFiClientSecure client;
  prepClient(client);
  HTTPClient https;
  https.setUserAgent("FlightEye-ESP32"); // GitHub's API rejects requests with no User-Agent

  String api = String("https://api.github.com/repos/") + OTA_GH_OWNER + "/" + OTA_GH_REPO + "/releases/latest";
  if (!https.begin(client, api)) {
    logf("ota: check begin() failed");
    s_state = OTA_FAILED;
    return;
  }
  int code = https.GET();
  if (code != HTTP_CODE_OK) {
    // 404 just means the repo has no releases yet - not an error worth flagging.
    if (code == 404) { logf("ota: no releases published yet"); s_state = OTA_UP_TO_DATE; }
    else              { logf("ota: release check -> HTTP %d", code); s_state = OTA_FAILED; }
    https.end();
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, https.getStream());
  https.end();
  if (err) {
    logf("ota: release JSON parse failed (%s)", err.c_str());
    s_state = OTA_FAILED;
    return;
  }

  String tag = doc["tag_name"] | "";
  if (!tag.length()) {
    logf("ota: latest release has no tag_name");
    s_state = OTA_FAILED;
    return;
  }

  if (!isNewer(tag, FW_VERSION)) {
    logf("ota: up to date (running %s, latest %s)", FW_VERSION, tag.c_str());
    s_state = OTA_UP_TO_DATE;
    return;
  }

  String assetUrl;
  size_t assetLen = 0;
  for (JsonObject a : doc["assets"].as<JsonArray>()) {
    if (String((const char*)(a["name"] | "")) == OTA_ASSET_NAME) {
      assetUrl = a["browser_download_url"].as<String>();
      assetLen = (size_t)(a["size"] | 0);
      break;
    }
  }
  if (!assetUrl.length()) {
    logf("ota: release %s has no '%s' asset", tag.c_str(), OTA_ASSET_NAME);
    s_state = OTA_FAILED;
    return;
  }

  logf("ota: %s available (running %s) - downloading", tag.c_str(), FW_VERSION);
  s_state = OTA_DOWNLOADING;
  String tagClean = tag;
  if (tagClean.length() && (tagClean[0] == 'v' || tagClean[0] == 'V')) tagClean.remove(0, 1);
  drawOtaSplash(tagClean, FW_VERSION);
  if (downloadAndFlash(assetUrl, assetLen)) {
    delay(300);
    ESP.restart();
  } else {
    s_state = OTA_FAILED;
  }
}

void otaInit() {
  s_state = OTA_IDLE;
  s_lastCheckMs = 0;
  s_everChecked = false;
}

void otaLoop() {
  if (WiFi.status() != WL_CONNECTED) return;
  // First check ~20s after boot (let Wi-Fi/NTP settle), then every
  // OTA_CHECK_INTERVAL_MS after that.
  if (!s_everChecked) {
    if (millis() > 20000) checkNow();
    return;
  }
  if (millis() - s_lastCheckMs > OTA_CHECK_INTERVAL_MS) checkNow();
}

String otaStateString() {
  switch (s_state) {
    case OTA_CHECKING:    return "checking";
    case OTA_DOWNLOADING: return "downloading";
    case OTA_UP_TO_DATE:  return "up-to-date";
    case OTA_FAILED:      return "failed";
    default:               return "idle";
  }
}

String otaLastCheckedAgo() {
  if (!s_everChecked) return "never";
  uint32_t s = (millis() - s_lastCheckMs) / 1000;
  if (s < 60)   return String(s) + "s ago";
  if (s < 3600) return String(s / 60) + "m ago";
  return String(s / 3600) + "h ago";
}
