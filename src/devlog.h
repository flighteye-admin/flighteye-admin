#pragma once
#include <Arduino.h>
#include <stdarg.h>

// Tiny ring buffer of log lines, mirrored to Serial and served to the admin page.
#define LOG_LINES 40
#define LOG_LEN   96

void logInit();
void logf(const char* fmt, ...);
String logAsJson();      // newest-last array of strings
