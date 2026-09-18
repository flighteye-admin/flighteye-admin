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
