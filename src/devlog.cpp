#include "devlog.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// Real-world mbedTLS handshake allocations on this core run up to ~40-45KB
// in one block; 60KB gives that real margin rather than a number picked to
// just barely clear it.
static const uint32_t kMinFreeBlockForTls = 60000;

uint32_t largestFreeBlock(){
  return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}
bool heapOkForTls(){
  return largestFreeBlock() >= kMinFreeBlockForTls;
}

static const uint32_t kMinFreeBlockForOta = 100000;
bool heapOkForOta(){
  return largestFreeBlock() >= kMinFreeBlockForOta;
}

static char  s_buf[LOG_LINES][LOG_LEN];
static int   s_head=0, s_count=0;

void logInit(){ s_head=0; s_count=0; }

void logf(const char* fmt, ...){
  char body[LOG_LEN-14];
  va_list ap; va_start(ap,fmt);
  vsnprintf(body,sizeof(body),fmt,ap);
  va_end(ap);

  uint32_t s=millis()/1000;
  snprintf(s_buf[s_head],LOG_LEN,"[%02u:%02u:%02u] %s",
           (unsigned)(s/3600),(unsigned)((s/60)%60),(unsigned)(s%60), body);
  Serial.println(s_buf[s_head]);
  s_head=(s_head+1)%LOG_LINES;
  if(s_count<LOG_LINES) s_count++;
}

String logAsJson(){
  JsonDocument d; JsonArray a=d.to<JsonArray>();
  int start=(s_head-s_count+LOG_LINES)%LOG_LINES;
  for(int i=0;i<s_count;i++) a.add(s_buf[(start+i)%LOG_LINES]);
  String out; serializeJson(d,out); return out;
}
