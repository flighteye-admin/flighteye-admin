#include "devlog.h"
#include <ArduinoJson.h>

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
