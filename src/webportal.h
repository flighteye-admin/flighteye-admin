#pragma once
#include <Arduino.h>
#include "flight.h"

void portalBeginAP();                 // captive setup portal (SoftAP)
void portalBeginSTA();                // admin portal on the home network
void portalLoop();                    // call from loop() (handles captive DNS)
void portalSetCurrent(const Flight& f, bool haveFlight);   // feed status endpoint
extern volatile bool g_wifiSubmitted; // set when user saves Wi-Fi in setup
