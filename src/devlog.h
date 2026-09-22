#pragma once
#include <Arduino.h>
#include <stdarg.h>

// Tiny ring buffer of log lines, mirrored to Serial and served to the admin page.
// v3.21: LOG_LEN bumped 96->160 so a logged HTTP error body (see fetchSource())
// isn't cut off before it says anything useful.
#define LOG_LINES 40
#define LOG_LEN   160

void logInit();
void logf(const char* fmt, ...);
String logAsJson();      // newest-last array of strings

// ---------------------------------------------------------------------------
// v3.33: heap-fragmentation guard. ESP32's WiFiClientSecure/mbedTLS needs one
// large *contiguous* allocation per TLS handshake (in practice ~40-45KB) -
// total free heap can look perfectly healthy while still being too
// fragmented to satisfy that single allocation, which is what actually
// crashes the device, not free heap reaching zero. largestFreeBlock() is the
// metric that actually predicts a handshake will succeed; heapOkForTls()
// wraps it with a threshold that has real headroom above that ~40-45KB need.
// Every function in this firmware that opens a WiFiClientSecure checks this
// first and skips the attempt (logging why) rather than risking a crash.
// ---------------------------------------------------------------------------
uint32_t largestFreeBlock();
bool     heapOkForTls();

// v3.35: the OTA download holds a TLS session open for the whole transfer
// (hundreds of KB, tens of seconds) at the same time as writing to flash -
// a lot more sustained memory pressure than the app's usual small, quick
// JSON requests. heapOkForOta() asks for real extra headroom on top of the
// ordinary heapOkForTls() floor before starting one.
bool     heapOkForOta();
