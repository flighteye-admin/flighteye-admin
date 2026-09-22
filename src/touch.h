#pragma once

// Minimal "is the panel currently pressed" check - just the XPT2046's IRQ
// line going low on contact, no coordinate mapping or calibration involved.
// Implemented in main.cpp, which owns the touch SPI setup and pins; exposed
// here so other modules don't need their own copy of that wiring. Added in
// v3.34 for ota.cpp's "tap the screen to restart" prompt after a firmware
// update finishes flashing.
bool touchDown();
