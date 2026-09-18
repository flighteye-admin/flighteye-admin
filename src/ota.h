#pragma once
#include <Arduino.h>

// GitHub repo that publishes Flight Eye firmware releases.
// *** Set OTA_GH_OWNER to your GitHub username once the repo exists. ***
#define OTA_GH_OWNER   "flighteye-admin"
#define OTA_GH_REPO    "flighteye-admin"

// Must exactly match the asset name the release workflow attaches
// (see .github/workflows/release.yml).
#define OTA_ASSET_NAME "flighteye-firmware.bin"

// How often the device checks GitHub for a newer release, once Wi-Fi is up.
#define OTA_CHECK_INTERVAL_MS (6UL*60UL*60UL*1000UL)   // 6 hours

enum OtaState { OTA_IDLE, OTA_CHECKING, OTA_DOWNLOADING, OTA_UP_TO_DATE, OTA_FAILED };

void   otaInit();               // call once from setup()
void   otaLoop();                // call every loop(); no-ops unless Wi-Fi is connected
String otaStateString();        // "idle" | "checking" | "downloading" | "up-to-date" | "failed"
String otaLastCheckedAgo();     // "3m ago" / "never", for the admin status JSON
