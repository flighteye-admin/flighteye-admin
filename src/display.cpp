#include "display.h"
#include "config.h"
#include "devlog.h"   // v3.37: nz() - null-safe String::c_str() guard, see devlog.h
#include <TFT_eSPI.h>
#include <qrcode.h>

extern TFT_eSPI tft;

// colour map (RGB565) — mirrors the mockups
#define COL_HEADER   TFT_NAVY
#define COL_CALLSIGN TFT_YELLOW
#define COL_ROUTE    TFT_CYAN
#define COL_VALUE    TFT_WHITE
#define COL_LABEL    0x7BEF
#define COL_AIRLINE  0xAD55
#define COL_CITY     0x8410
#define COL_LIVE     TFT_GREEN
#define COL_LOCK     TFT_ORANGE

static int W(){ return tft.width(); }
static int H(){ return tft.height(); }

void setBrightness(int pct){ pct=constrain(pct,5,100); ledcWrite(TFT_BL, map(pct,0,100,0,255)); }

void displayInit(){
  tft.init();
  tft.setRotation(cfg.rotation);
  ledcAttach(TFT_BL, 5000, 8);
  setBrightness(cfg.brightness);
  tft.fillScreen(TFT_BLACK);
}

// ---------- shared header ----------
static void header(){
  tft.fillRect(0,0,W(),34,COL_HEADER);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_VALUE,COL_HEADER);
  tft.setTextFont(4);
  tft.drawString("FLIGHT EYE",10,17,4);
}

// ---------- aircraft-type icons ----------
static void icon(int cx,int cy,int s,const char* cat){
  uint16_t c=COL_VALUE; float h=s/2.0;
  auto R=[&](float x,float y,float w,float hh){ tft.fillRect(cx+x,cy+y,w,hh,c); };
  if(!strcmp(cat,"heli")){
    tft.drawCircle(cx,cy-2,h,c); tft.drawCircle(cx,cy-2,h-1,c);
    tft.fillCircle(cx,cy-2,s*0.16,c); R(-1.5,0,3,h); R(-5,h-3,10,3); return;
  }
  if(!strcmp(cat,"military")){
    tft.fillTriangle(cx,cy-h, cx-h*0.9,cy+h*0.5, cx+h*0.9,cy+h*0.5, c);
    R(-1.5,-h,3,s); tft.fillTriangle(cx,cy+h*0.2, cx-h*0.35,cy+h, cx+h*0.35,cy+h, c); return;
  }
  R(-1.5,-h,3,s);
  if(!strcmp(cat,"turboprop")){
    R(-h,-2,s,4); tft.fillCircle(cx-h*0.75,cy-4,s*0.11,c); tft.fillCircle(cx+h*0.75,cy-4,s*0.11,c); R(-6,h-5,12,3);
  } else if(!strcmp(cat,"bizjet")){
    tft.fillTriangle(cx,cy, cx-h,cy+h*0.5, cx,cy+h*0.35,c);
    tft.fillTriangle(cx,cy, cx+h,cy+h*0.5, cx,cy+h*0.35,c);
    R(-4,h*0.55,2.6,5); R(1.4,h*0.55,2.6,5);
  } else if(!strcmp(cat,"light")){
    R(-h,-4,s,4); R(-6,-h,12,3); R(-5,h-5,10,3);
  } else if(!strcmp(cat,"cargo")){
    // v3.37: same big-jet silhouette as the "airliner" case below (a
    // freighter's airframe looks no different) with a boxy cargo pod slung
    // under the belly - a deliberate visual cue, driven by the callsign's
    // operator prefix (see isCargoPrefix() in flight.cpp), so a
    // FedEx/UPS/Cargolux/etc. flight doesn't look identical to a passenger
    // airliner on screen. The pod is drawn clearly wider than the fuselage
    // line and outlined in the slot's own background colour so it reads as
    // something bolted on, not just a thicker bit of fuselage.
    tft.fillTriangle(cx,cy-h*0.3, cx-h,cy+h*0.45, cx,cy+h*0.2,c);
    tft.fillTriangle(cx,cy-h*0.3, cx+h,cy+h*0.45, cx,cy+h*0.2,c);
    tft.fillTriangle(cx,cy+h*0.4, cx-h*0.45,cy+h, cx,cy+h*0.85,c);
    tft.fillTriangle(cx,cy+h*0.4, cx+h*0.45,cy+h, cx,cy+h*0.85,c);
    R(-6,-1,12,7);
    tft.drawRect(cx-6,cy-1,12,7,0x0841);
  } else {
    tft.fillTriangle(cx,cy-h*0.3, cx-h,cy+h*0.45, cx,cy+h*0.2,c);
    tft.fillTriangle(cx,cy-h*0.3, cx+h,cy+h*0.45, cx,cy+h*0.2,c);
    tft.fillTriangle(cx,cy+h*0.4, cx-h*0.45,cy+h, cx,cy+h*0.85,c);
    tft.fillTriangle(cx,cy+h*0.4, cx+h*0.45,cy+h, cx,cy+h*0.85,c);
  }
}
// tiny plane glyph (footer marker, points right at the text)
static void tinyPlane(int x,int y,uint16_t c){
  tft.fillTriangle(x, y-5, x, y+5, x+11, y, c);      // swept body pointing right
  tft.fillRect(x+2, y-1, 9, 3, c);
  tft.fillTriangle(x-1, y-6, x+4, y, x-1, y+6, TFT_BLACK);
  tft.fillTriangle(x+1, y-5, x+12, y, x+1, y+5, c);
  tft.fillRect(x+3, y-4, 2, 8, c);
}
// tiny tail-fin glyph (marks the registration / tail number)
static void tinyTail(int x,int y,uint16_t c){
  tft.fillTriangle(x+2, y+5, x+8, y-6, x+9, y+5, c); // swept fin
  tft.fillRect(x, y+5, 11, 2, c);                    // tailplane
}
// climb / descend / level marker
static void rateArrow(int x,int y,int vs,uint16_t c){
  if(vs>50)       tft.fillTriangle(x, y-5, x-5, y+4, x+5, y+4, c);   // up
  else if(vs<-50) tft.fillTriangle(x, y+5, x-5, y-4, x+5, y-4, c);   // down
  else            tft.fillRect(x-5, y-1, 10, 3, c);                  // level
}

static void iconSlot(const char* cat){
  int s=38, cx=W()-32, cy=64;   // sits below the lock button (which ends at y=30)
  tft.fillRoundRect(cx-25,cy-25,50,50,8,0x0841);
  tft.drawRoundRect(cx-25,cy-25,50,50,8,0x2104);
  icon(cx,cy,s,cat);
}

// v3.19: locking/unlocking a flight is admin-page only now - the touchscreen
// no longer has a lock button to tap, so this is a pure status indicator.
// It's only ever called when a lock is actually active, drawn right after
// the callsign/target text (wherever the caller places it).
static const int LOCK_W=28, LOCK_H=24;
static void lockIcon(int x, int y){
  tft.fillRoundRect(x,y,LOCK_W,LOCK_H,5, COL_LOCK);
  int bx=x+LOCK_W/2;
  tft.fillRect(bx-5,y+12,10,8,0x0000); tft.drawCircle(bx,y+12,4,0x0000);
}

// ---------- screens ----------
// big centred plane for the splash
static void splashPlane(int cx,int cy,float s,uint16_t c){
  tft.fillRect(cx-2,cy-s,4,s*2,c);                                  // fuselage
  tft.fillTriangle(cx,cy-s*0.35, cx-s*1.5,cy+s*0.45, cx,cy+s*0.15,c);
  tft.fillTriangle(cx,cy-s*0.35, cx+s*1.5,cy+s*0.45, cx,cy+s*0.15,c);
  tft.fillTriangle(cx,cy+s*0.45, cx-s*0.6,cy+s,      cx,cy+s*0.8,  c);
  tft.fillTriangle(cx,cy+s*0.45, cx+s*0.6,cy+s,      cx,cy+s*0.8,  c);
  tft.fillCircle(cx,cy-s*0.9,3,c);
}

void drawSplash(){
  tft.fillScreen(TFT_BLACK);
  int cx=W()/2;
  // soft horizon band behind the mark
  for(int i=0;i<26;i++) tft.drawFastHLine(0, 96+i, W(), tft.color565(0, 0, 34+i));
  splashPlane(cx, 92, 24, COL_CALLSIGN);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold18pt7b);
  tft.setTextColor(COL_VALUE,TFT_BLACK);
  tft.drawString("FLIGHT EYE", cx, 156);
  tft.setTextFont(2);
  tft.setTextColor(COL_ROUTE,TFT_BLACK);
  tft.drawString("live aircraft tracker", cx, 186, 2);
  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString("v3  -  starting up...", cx, 216, 2);
}

void drawSetup(const String& ssid, const String& ip){
  tft.fillScreen(TFT_BLACK); header();
  QRCode qr; uint8_t buf[qrcode_getBufferSize(3)];
  String payload="WIFI:S:"+ssid+";T:nopass;;";
  qrcode_initText(&qr,buf,3,ECC_MEDIUM,nz(payload));
  int scale=3, qs=qr.size*scale, ox=14, oy=52;
  tft.fillRect(ox-4,oy-4,qs+8,qs+8,TFT_WHITE);
  for(uint8_t y=0;y<qr.size;y++)for(uint8_t x=0;x<qr.size;x++)
    if(qrcode_getModule(&qr,x,y)) tft.fillRect(ox+x*scale,oy+y*scale,scale,scale,TFT_BLACK);
  int tx=ox+qs+18; tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_VALUE,TFT_BLACK); tft.drawString("Let's get set up",tx,48,4);
  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString("1  Scan to join Wi-Fi",tx,84,2);
  tft.drawString("2  Setup page opens",tx,104,2);
  tft.drawString("3  Pick your network",tx,124,2);
  tft.setTextColor(COL_CALLSIGN,TFT_BLACK); tft.drawString(ssid,tx,150,2);
  tft.setTextColor(COL_LABEL,TFT_BLACK);    tft.drawString("http://"+ip,tx,168,2);
}

void drawConnecting(int step,const String& msg,int pct){
  static int last=-1;
  if(step!=last){ tft.fillScreen(TFT_BLACK); header();
    tft.setTextDatum(MC_DATUM); tft.setTextColor(COL_VALUE,TFT_BLACK);
    tft.drawString("Getting you airborne...",W()/2,60,4); last=step; }
  const char* steps[3]={"Saving settings","Joining your Wi-Fi","Fetching live flights"};
  tft.setTextDatum(TL_DATUM);
  for(int i=0;i<3;i++){
    uint16_t c = i<step?COL_LIVE : i==step?COL_VALUE : COL_LABEL;
    tft.setTextColor(c,TFT_BLACK);
    tft.drawString((i<step?"[x] ":i==step?"[>] ":"[ ] ")+String(steps[i]),24,100+i*24,2);
  }
  int bw=W()-48; tft.drawRect(24,180,bw,8,COL_LABEL);
  tft.fillRect(25,181,(bw-2)*constrain(pct,0,100)/100,6,COL_ROUTE);
  tft.fillRect(0,196,W(),16,TFT_BLACK);
  tft.setTextDatum(MC_DATUM); tft.setTextColor(COL_LABEL,TFT_BLACK); tft.drawString(msg,W()/2,204,2);
}

void drawConnected(const String& ip, const String& host){
  tft.fillScreen(TFT_BLACK); header();
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_LIVE,TFT_BLACK);  tft.drawString("Connected",W()/2,66,4);
  tft.setTextColor(COL_LABEL,TFT_BLACK); tft.drawString("Admin page at:",W()/2,104,2);
  tft.setTextColor(COL_VALUE,TFT_BLACK); tft.drawString("http://"+host,W()/2,130,4);
  tft.setTextColor(COL_CITY,TFT_BLACK);  tft.drawString("or  http://"+ip,W()/2,158,2);
  tft.setTextColor(COL_LABEL,TFT_BLACK); tft.drawString("tracking will start shortly",W()/2,196,2);
}

// v3.36: repaints just the bottom line of the Connected screen above, so the
// one guaranteed version check on a fresh boot (see main.cpp's CONNECTING
// case) is visible instead of silently happening off-screen for ~20s while
// the device already looks idle. Call drawConnected() again afterwards to
// put the normal "tracking will start shortly" line back.
void drawCheckingUpdate(){
  tft.fillRect(0,186,W(),20,TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString("checking for updates...",W()/2,196,2);
}

static void fmtAlt(const Flight& f,char* b,size_t n){
  if(f.onGround){ snprintf(b,n,"LANDED"); return; }   // ADS-B reports alt_baro "ground"
  if(f.altFt<=0){ snprintf(b,n,"GND"); return; }
  if(f.altFt>=18000) snprintf(b,n,"FL%03d",f.altFt/100);
  else snprintf(b,n,cfg.imperial?"%dft":"%dm",cfg.imperial?f.altFt:(int)(f.altFt*0.3048));
}

// Insert the hyphen into a bare tail number. Prefix-aware: US marks (N...)
// are correctly written without one, most others take a hyphen after the
// national prefix.
static String hyphenateReg(const String& raw){
  String r = raw; r.toUpperCase();
  if(r.indexOf('-')>=0) return r;                 // already formatted
  if(r.length()<4) return r;
  if(r[0]=='N') return r;                         // United States: no hyphen

  // two-character national prefixes
  static const char* P2[] = {"EC","EI","OO","OE","OK","OM","OY","LN","LX","LY","LZ",
                             "PH","SE","SP","TC","UR","YL","YR","ES","HA","HB","9H",
                             "9A","5B","4X","VH","ZK","ZS","CS","CN","TF","OH"};
  String two = r.substring(0,2);
  for(auto p:P2) if(two==p) return two + "-" + r.substring(2);

  // single-character national prefixes (G, D, F, I, C, ...)
  return r.substring(0,1) + "-" + r.substring(1);
}

// choose the callsign string per config (ICAO default, IATA optional), with fallbacks
static String callsignText(const Flight& f){
  if(cfg.callsignIata && f.csIata.length()) return f.csIata;

  if(f.callsign.length()){
    // If the "callsign" is really the tail number, show it properly punctuated.
    if(f.reg.length()){
      String a=f.callsign; a.toUpperCase();
      String b=f.reg;      b.toUpperCase(); b.replace("-","");
      if(a==b) return f.reg;                      // the reg field already has the hyphen
    }
    // No reg field to borrow from: infer from the shape of the callsign.
    String c=f.callsign; c.toUpperCase();
    bool hasDigit=false, allAlnum=true;
    for(size_t i=0;i<c.length();i++){
      if(isdigit(c[i])) hasDigit=true;
      if(!isalnum(c[i])) allAlnum=false;
    }
    bool airlinePrefix = c.length()>=3 && isalpha(c[0]) && isalpha(c[1]) && isalpha(c[2])
                         && c.length()>3 && isdigit(c[3]);
    if(allAlnum && hasDigit==false && c.length()>=5 && !airlinePrefix)
      return hyphenateReg(c);                     // e.g. GCCZV -> G-CCZV
    if(allAlnum && !airlinePrefix && c.length()>=5 && c[0]!='N' && !isdigit(c[1]))
      return hyphenateReg(c);
    return f.callsign;
  }

  if(f.reg.length()) return f.reg;
  return "HEX "+f.hex;
}

void drawFlightCard(const Flight& f, bool live, int pos, int total){
  tft.fillScreen(TFT_BLACK);

  // --- callsign, with the lock button immediately following it ---
  tft.setTextDatum(TL_DATUM);
  tft.setFreeFont(&FreeSansBold24pt7b);
  String cs = callsignText(f);
  int csW = tft.textWidth(cs);
  tft.setTextColor(COL_CALLSIGN,TFT_BLACK);
  tft.drawString(cs,12,8);
  tft.setTextFont(2);
  // v3.19: the lock icon is a pure status indicator now (no touch target),
  // so it only appears at all when actually locked - nothing to show
  // otherwise.
  if(cfg.lockOn) lockIcon(12+csW+14, 10);

  // live dot - purely informational, not a touch target, so it can stay put
  tft.fillCircle(W()-14,17,4, live?COL_LIVE:0x0320);

  // --- operator, or the full aircraft name for GA ---
  tft.setTextColor(COL_AIRLINE,TFT_BLACK);
  {
    // Operator only. The aircraft name lives in the footer, so don't repeat it here.
    String line = f.airline;
    if(!line.length()){
      bool ga = strcmp(f.icon,"light")==0 || strcmp(f.icon,"bizjet")==0;
      bool hl = strcmp(f.icon,"heli")==0;
      bool ml = strcmp(f.icon,"military")==0;
      line = ga? "Private / GA" : hl? "Rotary" : ml? "Military" : "";
    }
    if(line.length()) tft.drawString(line,14,54,2);
  }

  iconSlot(f.icon);

  // --- route ---
  bool haveRoute = f.originIata.length()||f.destIata.length();
  tft.setTextColor(COL_ROUTE,TFT_BLACK);
  if(haveRoute)
    tft.drawString((f.originIata.length()?f.originIata:"?")+" -> "+(f.destIata.length()?f.destIata:"?"),14,74,4);
  else {
    bool ga = strcmp(f.icon,"light")==0 || strcmp(f.icon,"bizjet")==0 || strcmp(f.icon,"heli")==0;
    tft.drawString(ga? "no route filed" : "- en route -",14,74,4);
  }
  if(f.originCity.length()||f.destCity.length()){
    tft.setTextColor(COL_CITY,TFT_BLACK);
    tft.drawString((f.originCity.length()?f.originCity:"?")+" -> "+(f.destCity.length()?f.destCity:"?"),14,102,2);
  }

  // --- stat columns ---
  char a[12]; fmtAlt(f,a,sizeof(a));
  char sp[12]; snprintf(sp,sizeof(sp),cfg.imperial?"%dkt":"%dkmh",cfg.imperial?f.gs:(int)lround(f.gs*1.852));
  char hd[8];  snprintf(hd,sizeof(hd),"%03d",f.track);
  struct{int x;const char*l;const char*v;} col[3]={{62,"ALT",a},{164,"SPD",sp},{266,"HDG",hd}};
  tft.setTextDatum(MC_DATUM);
  for(auto&cc:col){
    tft.setTextColor(COL_LABEL,TFT_BLACK); tft.drawString(cc.l,cc.x,134,2);
    tft.setTextColor(COL_VALUE,TFT_BLACK); tft.drawString(cc.v,cc.x,158,4);
  }

  // --- vertical rate (left) and distance from home (right) ---
  {
    uint16_t vc = f.vsFpm>50?COL_LIVE : f.vsFpm<-50?COL_LOCK : COL_LABEL;
    char v[16];
    if(abs(f.vsFpm)<50) snprintf(v,sizeof(v),"level");
    else                snprintf(v,sizeof(v),"%d fpm", abs(f.vsFpm));
    rateArrow(26,186,f.vsFpm,vc);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(vc,TFT_BLACK); tft.drawString(v,38,186,2);

    char dk[24];
    double d = cfg.imperial ? f.distKm*0.621371 : f.distKm;
    const char* u = cfg.imperial ? "mi" : "km";
    if(d < 100)       snprintf(dk,sizeof(dk),"%.1f %s away", d, u);
    else              snprintf(dk,sizeof(dk),"%d %s away", (int)lround(d), u);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(COL_ROUTE,TFT_BLACK);
    tft.drawString(dk,W()-14,186,2);

    // rotation position sits here, not in the footer, so the footer can use
    // its full width for the aircraft name and tail number
    if(total>1 && !cfg.lockOn){
      char q[12]; snprintf(q,sizeof(q),"%d/%d",pos,total);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(0x528A,TFT_BLACK);
      tft.drawString(q,W()/2,186,2);
    }
  }

  // --- footer: full aircraft name, then the tail block flowed in after it ---
  tft.fillRect(0,212,W(),28,0x0841);
  tft.setTextDatum(ML_DATUM);
  tinyPlane(12,226,COL_ROUTE);
  tft.setTextColor(COL_VALUE,0x0841);
  {
    const int NAME_X   = 30;
    const int GAP      = 12;      // space between the name and the tail glyph
    const int TAIL_W   = 18;      // glyph plus its padding
    // only "LOCKED" needs room on the right now
    const int RIGHT_RESERVE = cfg.lockOn ? 54 : 10;

    tft.setTextFont(2);
    String name = f.typeName.length()? f.typeName
                : (f.type.length()? f.type : "----");
    String reg  = f.reg;

    int nameW = tft.textWidth(name);
    int regW  = reg.length()? tft.textWidth(reg) : 0;
    int need  = NAME_X + nameW + (reg.length()? GAP + TAIL_W + regW : 0);

    // If the full name pushes the registration off the edge, fall back to the
    // short ICAO code so both still fit on the one line.
    if(reg.length() && need > W() - RIGHT_RESERVE && f.type.length()){
      name  = f.type;
      nameW = tft.textWidth(name);
      need  = NAME_X + nameW + GAP + TAIL_W + regW;
    }

    tft.drawString(name, NAME_X, 226, 2);

    if(reg.length() && need <= W() - RIGHT_RESERVE){
      int tailX = NAME_X + nameW + GAP;
      tinyTail(tailX, 220, COL_ROUTE);
      tft.drawString(reg, tailX + TAIL_W, 226, 2);
    }
  }

  tft.setTextDatum(MR_DATUM);
  if(cfg.lockOn){
    tft.setTextColor(COL_LOCK,0x0841); tft.drawString("LOCKED",W()-12,226,2);
    tft.drawRect(0,0,W(),H(),COL_LOCK); tft.drawRect(1,1,W()-2,H()-2,COL_LOCK);
  }
}

void drawNoFlights(const char* source, const String& ip){
  tft.fillScreen(TFT_BLACK); header();
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString("No aircraft in range",W()/2,H()/2-16,4);
  tft.drawString(String("via ")+source,W()/2,H()/2+10,2);
  tft.setTextColor(COL_CITY,TFT_BLACK);
  tft.drawString("flighteye.local  /  "+ip,W()/2,H()-22,2);
}

void drawError(const String& msg){
  tft.fillScreen(TFT_BLACK); header();
  tft.setTextDatum(MC_DATUM); tft.setTextColor(COL_LOCK,TFT_BLACK);
  tft.drawString(msg,W()/2,H()/2,2);
}

void drawCalibratePrompt(int corner){
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_VALUE,TFT_BLACK);
  tft.drawString("Touch calibration",W()/2,H()/2-34,4);
  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString(corner==0 ? "tap the marker, top-left"
                           : "now tap the marker, bottom-right",W()/2,H()/2-4,2);
  tft.setTextColor(0x528A,TFT_BLACK);
  tft.drawString(corner==0 ? "step 1 of 2" : "step 2 of 2",W()/2,H()/2+18,2);

  int x = corner==0 ? 10 : W()-10;
  int y = corner==0 ? 10 : H()-10;
  tft.drawFastHLine(x-9,y,19,COL_ROUTE);
  tft.drawFastVLine(x,y-9,19,COL_ROUTE);
  tft.fillCircle(x,y,4,COL_LOCK);
  tft.drawCircle(x,y,9,COL_ROUTE);
}

void drawCalibrateDone(bool ok){
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(ok?COL_LIVE:COL_LOCK,TFT_BLACK);
  tft.drawString(ok? "Calibration saved" : "Calibration cancelled",W()/2,H()/2-10,4);
  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString(ok? "the padlock should respond now"
                   : "keeping the previous settings",W()/2,H()/2+20,2);
}

// Shown when the display is locked to an aircraft that is not currently in range.
void drawLockWaiting(const String& target, uint32_t sinceSec, bool likelyLanded){
  tft.fillScreen(TFT_BLACK);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_LOCK,TFT_BLACK);
  tft.drawString("LOCKED",12,10,4);

  tft.setFreeFont(&FreeSansBold24pt7b);
  int tgtW = tft.textWidth(target);
  tft.setTextColor(COL_CALLSIGN,TFT_BLACK);
  tft.drawString(target,12,44);
  tft.setTextFont(2);
  lockIcon(12+tgtW+14, 48);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString(likelyLanded? "likely landed" : "no signal right now",W()/2,116,4);

  char t[64];
  if(sinceSec<60)      snprintf(t,sizeof(t),"searching worldwide  -  %us",(unsigned)sinceSec);
  else                 snprintf(t,sizeof(t),"searching worldwide  -  %um %us",
                                (unsigned)(sinceSec/60),(unsigned)(sinceSec%60));
  tft.setTextColor(COL_CITY,TFT_BLACK);
  tft.drawString(t,W()/2,146,2);

  tft.setTextColor(0x528A,TFT_BLACK);
  if(likelyLanded){
    tft.drawString("last seen low and descending -",W()/2,178,2);
    tft.drawString("transponder likely switched off",W()/2,194,2);
  } else {
    tft.drawString("aircraft may be on the ground or over an",W()/2,178,2);
    tft.drawString("ocean with no receiver coverage",W()/2,194,2);
  }
  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString("unlock from the admin page",W()/2,216,2);

  tft.drawRect(0,0,W(),H(),COL_LOCK);
  tft.drawRect(1,1,W()-2,H()-2,COL_LOCK);
}

// ---------- recovery / reset screens ----------
void drawWifiHelp(const String& ssid, uint32_t secs){
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0,0,W(),30,0x6000);                       // amber-ish warning bar
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_LOCK,0x6000);
  tft.drawString("Can't join Wi-Fi",10,15,4);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString("Network:",14,44,2);
  tft.setTextColor(COL_VALUE,TFT_BLACK);
  tft.drawString(ssid.length()?ssid:"(none saved)",80,44,2);

  tft.setTextColor(COL_LABEL,TFT_BLACK);
  char t[48]; snprintf(t,sizeof(t),"Still retrying... %us elapsed",(unsigned)secs);
  tft.drawString(t,14,64,2);

  tft.drawFastHLine(14,86,W()-28,0x2965);

  tft.setTextColor(COL_VALUE,TFT_BLACK);
  tft.drawString("Use the BOOT button on the board:",14,98,2);
  tft.setTextColor(COL_ROUTE,TFT_BLACK);
  tft.drawString("Hold 5s",14,124,4);
  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString("reset Wi-Fi, restart setup",96,130,2);
  tft.setTextColor(COL_LOCK,TFT_BLACK);
  tft.drawString("Hold 10s",14,156,4);
  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString("full factory reset",108,162,2);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(0x528A,TFT_BLACK);
  tft.drawString("release early to cancel",W()/2,200,2);
}

void drawResetCountdown(int heldSec){
  bool factory = heldSec>=5;                    // 5..9 -> wifi, 10+ -> factory
  uint16_t col = factory? COL_LOCK : COL_ROUTE;
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  tft.setTextColor(COL_VALUE,TFT_BLACK);
  tft.drawString(factory? "Factory reset in" : "Wi-Fi reset in",W()/2,54,4);

  int remain = factory? (10-heldSec) : (5-heldSec);
  if(remain<0) remain=0;
  char n[6]; snprintf(n,sizeof(n),"%d",remain);
  tft.setFreeFont(&FreeSansBold24pt7b);
  tft.setTextColor(col,TFT_BLACK);
  tft.drawString(n,W()/2,110);
  tft.setTextFont(2);

  int bw=W()-60;
  tft.drawRect(30,158,bw,10,0x2965);
  int pct = constrain(heldSec*10,0,100);
  tft.fillRect(31,159,(bw-2)*pct/100,8,col);

  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString(factory? "keep holding to wipe everything"
                        : "keep holding for factory reset",W()/2,186,2);
  tft.setTextColor(0x528A,TFT_BLACK);
  tft.drawString("release to cancel",W()/2,208,2);
}

void drawResetting(const String& what){
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_LOCK,TFT_BLACK);
  tft.drawString(what,W()/2,H()/2-14,4);
  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString("restarting...",W()/2,H()/2+16,2);
}

// ---------------------------------------------------------------------------
// v3.32: OTA update splash. Shown once when a newer release is found and the
// device starts downloading + flashing it, so the "why did the screen change"
// moment reads as reassuring progress rather than a hang. drawOtaSplash()
// paints the static frame once; drawOtaProgress() repaints just the bar fill
// and percentage as Update.onProgress() reports bytes written, same pattern
// as drawConnecting()'s progress bar.
// ---------------------------------------------------------------------------
static int s_otaLastPct = -1;

void drawOtaSplash(const String& newVer, const String& curVer){
  s_otaLastPct = -1;
  tft.fillScreen(TFT_BLACK);
  int cx = W()/2;
  splashPlane(cx, 34, 16, COL_CALLSIGN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_VALUE, TFT_BLACK);
  tft.drawString("New version found!", cx, 68, 4);
  tft.setTextColor(COL_ROUTE, TFT_BLACK);
  tft.drawString("v" + curVer + "  -->  v" + newVer, cx, 96, 4);
  tft.setTextColor(COL_LABEL, TFT_BLACK);
  tft.drawString("Installing new version...", cx, 128, 2);
  tft.drawString("Please wait, don't unplug me", cx, 150, 2);
  int bw = W()-48;
  tft.drawRect(24, 176, bw, 12, COL_LABEL);
  tft.fillRect(0, 198, W(), 20, TFT_BLACK);
  tft.setTextColor(COL_LIVE, TFT_BLACK);
  tft.drawString("0%", cx, 206, 2);
}

void drawOtaProgress(int pct){
  pct = constrain(pct, 0, 100);
  if(pct == s_otaLastPct) return;
  s_otaLastPct = pct;
  int cx = W()/2;
  int bw = W()-48;
  tft.fillRect(25, 177, bw-2, 10, TFT_BLACK);
  tft.fillRect(25, 177, (bw-2)*pct/100, 10, COL_ROUTE);
  tft.fillRect(0, 198, W(), 20, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_LIVE, TFT_BLACK);
  tft.drawString(String(pct) + "%", cx, 206, 2);
}

// v3.34: shown once the download finishes flashing, instead of restarting
// straight away - stays up until the screen is tapped (with a timed fallback
// restart, so a board nobody's watching doesn't just sit here forever). Most
// updates land in the background with nobody looking at the device, so this
// also doubles as visible confirmation that an update actually happened.
void drawOtaDone(const String& newVer){
  tft.fillScreen(TFT_BLACK);
  int cx = W()/2;
  splashPlane(cx, 34, 16, COL_LIVE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_LIVE, TFT_BLACK);
  tft.drawString("Update installed!", cx, 68, 4);
  tft.setTextColor(COL_ROUTE, TFT_BLACK);
  tft.drawString("Now running v" + newVer, cx, 96, 4);
  tft.setTextColor(COL_LABEL, TFT_BLACK);
  tft.drawString("Tap the screen to restart", cx, 140, 2);
  tft.drawString("(or it restarts itself shortly)", cx, 162, 2);
}

// ---------------------------------------------------------------------------
// Device info screen. The QR encodes the admin URL by IP address, so a phone
// camera opens the page directly - no mDNS, no typing. This is the reliable
// route on Android, where browsers cannot resolve .local names at all.
// ---------------------------------------------------------------------------
void drawDeviceInfo(const String& ip, const String& ssid, int rssi,
                    uint32_t uptimeSec, const String& ver, const String& net){
  tft.fillScreen(TFT_BLACK);

  String url = "http://" + ip;
  QRCode qr; uint8_t buf[qrcode_getBufferSize(3)];
  qrcode_initText(&qr, buf, 3, ECC_MEDIUM, nz(url));
  int scale = 3, qs = qr.size*scale, ox = 12, oy = 46;
  tft.fillRect(ox-5, oy-5, qs+10, qs+10, TFT_WHITE);
  for(uint8_t y=0;y<qr.size;y++)
    for(uint8_t x=0;x<qr.size;x++)
      if(qrcode_getModule(&qr,x,y))
        tft.fillRect(ox+x*scale, oy+y*scale, scale, scale, TFT_BLACK);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_CALLSIGN,TFT_BLACK);
  tft.drawString("Flight Eye",12,10,4);
  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString(ver,150,18,2);

  int tx = ox + qs + 16;
  tft.setTextColor(COL_VALUE,TFT_BLACK);
  tft.drawString("Scan to open",tx,50,4);
  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString("the admin page",tx,76,2);

  tft.setTextColor(COL_ROUTE,TFT_BLACK);
  tft.drawString(url,tx,104,2);

  tft.setTextColor(COL_LABEL,TFT_BLACK);
  char l[48];
  snprintf(l,sizeof(l),"%s  %ddBm",nz(ssid),rssi);
  tft.drawString(l,tx,128,2);
  snprintf(l,sizeof(l),"up %uh %um",(unsigned)(uptimeSec/3600),(unsigned)((uptimeSec/60)%60));
  tft.drawString(l,tx,146,2);

  // v3.11: gateway/subnet, so a mismatched phone (different network/VLAN,
  // or a stale fixed-IP setup) can be spotted just by comparing this against
  // the phone's own Wi-Fi details screen.
  tft.setTextColor(COL_CITY,TFT_BLACK);
  tft.drawString(net,tx,166,2);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(0x528A,TFT_BLACK);
  tft.drawString("tap for the LED key",W()/2,H()-14,2);
}

// ---------------------------------------------------------------------------
// LED colour key - second page of the info screen (tap the device-info
// screen to reach this; tap again to return to the flight card). Mirrors the
// actual behaviour coded in ledUpdate() in main.cpp, so keep the two in sync
// if the LED logic ever changes.
// ---------------------------------------------------------------------------
void drawLedKey(){
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_CALLSIGN,TFT_BLACK);
  tft.setTextFont(4);
  tft.drawString("LED key",12,6);
  tft.setTextFont(2);

  // Solid dot = steady colour. Ring = pulses/blinks at that colour.
  struct Row{ uint16_t col; const char* label; bool pulse; };
  static const Row rows[] = {
    {TFT_WHITE,   "Overhead (<2km)",              false},
    {TFT_RED,     "Military",                     false},
    {TFT_MAGENTA, "Helicopter",                   false},
    {TFT_GREEN,   "Private / GA / bizjet",        false},
    {TFT_CYAN,    "Turboprop",                    false},
    {TFT_BLUE,    "Airliner",                     false},
    {TFT_YELLOW,  "Cargo",                        false},
    {TFT_BLUE,    "No traffic in range",          true},
    {TFT_GREEN,   "Status-only mode",             false},
    {TFT_WHITE,   "Proximity - faster=closer",    true},
    {TFT_CYAN,    "Density - faster=busier",      true},
    {TFT_RED,     "Emergency squawk (fast)",      true},
  };
  const int n = sizeof(rows)/sizeof(rows[0]);
  int y = 34, dy = 14, cx = 20;
  for(int i=0;i<n;i++,y+=dy){
    if(rows[i].pulse){
      tft.drawCircle(cx,y+6,5,rows[i].col);
      tft.drawCircle(cx,y+6,3,rows[i].col);
    } else {
      tft.fillCircle(cx,y+6,5,rows[i].col);
    }
    tft.setTextColor(COL_VALUE,TFT_BLACK);
    tft.drawString(rows[i].label,cx+14,y,2);
  }

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(0x528A,TFT_BLACK);
  tft.drawString("solid = steady   ring = pulses",W()/2,H()-30,1);
  tft.drawString("tap for the radar",W()/2,H()-12,2);
}

// ---------------------------------------------------------------------------
// Radar screen - fourth page of the info cycle. No offscreen buffer (a full
// 320x240 sprite is too much RAM on a board with no PSRAM configured), so
// this redraws the whole scene straight to the panel each tick, same
// "fillScreen then redraw" style as every other screen. There's no sweep
// animation (it flickered and didn't add anything real aircraft don't
// already provide), so the tick only needs to run slowly enough to reflect
// dead reckoning - see the ~1s interval in main.cpp.
// ---------------------------------------------------------------------------
static void radarGeom(int& cx,int& cy,int& rPix){
  cx = W()/2; cy = H()/2 + 8;
  rPix = (W()<H()? W():H())/2 - 26;
}
// polar (distance from home, compass bearing) -> screen xy
static void radarXY(double distKm,double bearingDeg,int cx,int cy,int rPix,double rangeKm,int& sx,int& sy){
  double r = rangeKm>0 ? (distKm/rangeKm)*rPix : 0;
  if(r>rPix) r=rPix;
  if(r<0) r=0;
  double rad = radians(bearingDeg);
  sx = cx + (int)lround(r*sin(rad));
  sy = cy - (int)lround(r*cos(rad));
}
static uint16_t radarBlipColour(const RadarBlip& b){
  if(b.emergency)               return TFT_RED;
  if(!strcmp(b.icon,"military"))  return TFT_RED;
  if(!strcmp(b.icon,"heli"))      return TFT_MAGENTA;
  if(!strcmp(b.icon,"light")||!strcmp(b.icon,"bizjet")) return TFT_GREEN;
  if(!strcmp(b.icon,"turboprop")) return TFT_CYAN;
  if(!strcmp(b.icon,"cargo"))     return TFT_ORANGE;     // v3.37: was lumped in with airliner (blue)
  return TFT_BLUE;                                       // airliner
}

// v3.30: a small heading-aligned aircraft glyph (fuselage + wings +
// tailplane) instead of a dot with a short tick - still just a handful of
// drawLine() calls (no bitmap/sprite), so it's cheap enough to redraw every
// tick for several blips at once. trackDeg 0 = up (matches radarXY's
// bearing convention), increasing clockwise.
static void drawPlaneGlyph(int sx,int sy,int trackDeg,uint16_t c){
  double rad = radians((double)trackDeg);
  double s = sin(rad), co = cos(rad);
  auto rot=[&](double along,double across,int& x,int& y){
    x = sx + (int)lround(along*s + across*co);
    y = sy - (int)lround(along*co - across*s);
  };
  int noseX,noseY, tailX,tailY, wingLX,wingLY, wingRX,wingRY,
      tailLX,tailLY, tailRX,tailRY;
  rot( 9, 0, noseX,noseY);
  rot(-6, 0, tailX,tailY);
  rot( 0,-6, wingLX,wingLY);
  rot( 0, 6, wingRX,wingRY);
  rot(-5,-3, tailLX,tailLY);
  rot(-5, 3, tailRX,tailRY);

  tft.drawLine(tailX,tailY,noseX,noseY,c);       // fuselage
  tft.drawLine(wingLX,wingLY,wingRX,wingRY,c);   // wings
  tft.drawLine(tailLX,tailLY,tailRX,tailRY,c);   // tailplane
  tft.fillCircle(noseX,noseY,1,c);               // nose, a touch bolder
}

void drawRadar(const std::vector<RadarBlip>& blips, float rangeKm, bool imperial){
  tft.fillScreen(TFT_BLACK);
  int cx,cy,rPix; radarGeom(cx,cy,rPix);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_CALLSIGN,TFT_BLACK);
  tft.setTextFont(4);
  tft.drawString("Radar",12,6);
  char cnt[8]; snprintf(cnt,sizeof(cnt),"%d",(int)blips.size());
  tft.setTextColor(COL_LABEL,TFT_BLACK);
  tft.drawString(cnt,W()-30,14,2);

  // range rings + outer boundary
  for(int i=1;i<=3;i++) tft.drawCircle(cx,cy,rPix*i/3,0x2965);

  // cardinal marks
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(0x528A,TFT_BLACK);
  tft.drawString("N",cx,cy-rPix-9,1);
  tft.drawString("S",cx,cy+rPix+9,1);
  tft.drawString("E",cx+rPix+10,cy,1);
  tft.drawString("W",cx-rPix-10,cy,1);

  // outer-ring range label
  char rl[16];
  double disp = imperial? rangeKm*0.621371 : rangeKm;
  snprintf(rl,sizeof(rl), imperial? "%.0fmi" : "%.0fkm", disp);
  tft.setTextColor(0x3A4552,TFT_BLACK);
  tft.drawString(rl,cx,cy-rPix+9,1);

  // home position
  tft.fillCircle(cx,cy,3,COL_VALUE);

  // aircraft: a heading-aligned plane glyph, plus a callsign/altitude label
  // in the next size up from the N/S/E/W and range chrome above, so the
  // per-aircraft data is actually easy to read at a glance. Still small
  // enough that two or three nearby blips don't run their labels together.
  for(const auto& b : blips){
    int sx,sy; radarXY(b.distKm,b.bearingDeg,cx,cy,rPix,rangeKm,sx,sy);
    uint16_t c = radarBlipColour(b);
    drawPlaneGlyph(sx,sy,b.track,c);

    if(b.callsign.length()){
      // callsign, then flight level just below it - blips are never
      // on-ground here (the filters upstream already exclude those), so
      // there's no "GND"/"LANDED" case to handle like the flight card does.
      char alt[12];
      if(b.altFt>=18000) snprintf(alt,sizeof(alt),"FL%03d",b.altFt/100);
      else snprintf(alt,sizeof(alt), imperial?"%dft":"%dm",
                    imperial? b.altFt : (int)(b.altFt*0.3048));

      bool nearRight = sx > W()-58;
      tft.setTextDatum(nearRight? MR_DATUM : ML_DATUM);
      int tx = nearRight? sx-9 : sx+9;
      tft.setTextColor(COL_VALUE,TFT_BLACK);
      tft.drawString(b.callsign,tx,sy-9,2);
      tft.setTextColor(COL_CITY,TFT_BLACK);
      tft.drawString(alt,tx,sy+9,2);
    }
  }

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(0x528A,TFT_BLACK);
  tft.drawString("tap anywhere for the flight card",W()/2,H()-8,1);
}
