#pragma once
#include <Arduino.h>
#include "flight.h"

void displayInit();
void setBrightness(int pct);
void drawSplash();
void drawSetup(const String& ssid, const String& ip);
void drawConnecting(int step, const String& msg, int pct);
void drawConnected(const String& ip, const String& host);
void drawFlightCard(const Flight& f, bool live, int pos=0, int total=0);
void drawNoFlights(const char* source, const String& ip);
void drawError(const String& msg);
void drawLockWaiting(const String& target, uint32_t sinceSec, bool likelyLanded=false);
void drawCalibratePrompt(int corner);
void drawCalibrateDone(bool ok);
void drawDeviceInfo(const String& ip, const String& ssid, int rssi,
                    uint32_t uptimeSec, const String& ver, const String& net);

// v3.3 additions
void drawWifiHelp(const String& ssid, uint32_t secs);   // shown after ~60s of failure
void drawResetCountdown(int heldSec);                   // BOOT button held
void drawResetting(const String& what);

// v3.32: OTA update splash - shown full-screen while a new release downloads/flashes
void drawOtaSplash(const String& newVer, const String& curVer);
void drawOtaProgress(int pct);
void drawOtaDone(const String& newVer);   // v3.34: shown once flashed, waiting for a confirming tap

// v3.10: second page of the device-info screen (tap again to reach it)
void drawLedKey();

// v3.12: third page - a north-up radar view. rangeKm is the outer ring
// (cfg.radiusKm); each blip is labelled with its callsign and flight level.
// v3.16: view-only - no tap-to-lock (removed, too fiddly at this size) and
// no auto-dismiss (stays up until touched, unlike the other info pages).
void drawRadar(const std::vector<RadarBlip>& blips, float rangeKm, bool imperial);
