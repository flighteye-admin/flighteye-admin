#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <time.h>
#include "config.h"
#include "display.h"
#include "flight.h"
#include "webportal.h"
#include "devlog.h"
#include "ota.h"
#include "touch.h"

Config    cfg;
TFT_eSPI  tft;

// ---- touch (XPT2046) read directly on the CYD's dedicated pins ----
#define T_CLK 25
#define T_CS  33
#define T_DIN 32
#define T_DO  39
#define T_IRQ 36
static SPIClass tsSPI(VSPI);

// ---- BOOT button + onboard RGB LED (active LOW) ----
#define BOOT_BTN 0
#define LED_R    4
#define LED_G    16
#define LED_B    17

static const char* HOSTNAME = "flighteye";

// v3.19: fixed touch dead zone on all four edges. The per-edge admin-tunable
// version (v3.17/v3.18) and the temporary debug overlay (v3.18-dbg) used to
// diagnose it are both gone now that a single generous margin on every side
// does the job - simpler to reason about and nothing left to mistune. Change
// this one constant (and reflash) if 50px ever turns out to be wrong for a
// different case.
static const int TOUCH_DEAD_PX = 50;

enum State { BOOT_ST, SETUP, CONNECTING, RUNNING, CALIBRATE, ERR };
static State state = BOOT_ST;
static uint32_t lastPoll=0, lastDwell=0, connectStart=0, lastTouch=0;
static Flight current; static bool haveFlight=false;
static uint32_t lockLostAt=0;          // when the locked target went out of range
static uint32_t lastLockPoll=0;        // locked aircraft refreshed at dwell rate
static uint32_t infoShownAt=0;         // device-info screen visible since
static int      infoPage=0;            // 0=hidden, 1=device info/QR, 2=LED key, 3=radar
static int      lockMisses=0;          // consecutive failed lock lookups

// radar (v3.12; sweep animation removed in v3.13 - it just caused flicker)
static uint32_t lastRadarDraw=0;
static std::vector<RadarBlip> radarBlips_;

// calibration
static int      calStep=0;
static int      calRx0=0, calRy0=0;
static uint32_t calShownAt=0;
static State    calReturnTo=RUNNING;

void beginCalibration(){
  calReturnTo = (state==CALIBRATE)? RUNNING : state;
  calStep=0; calShownAt=millis();
  state=CALIBRATE;
  drawCalibratePrompt(0);
  logf("touch calibration started");
}

// BOOT button hold tracking
static uint32_t btnDownAt=0;
static bool     btnWasDown=false, countdownShown=false;
static int      lastHeldSec=-1;

// number of set bits in a subnet mask, for compact "/24" style display
// (defined near startConnect() below; declared here so pollTouch() can use it)
static int maskBits(const IPAddress& m);

void applyLiveConfig(){
  setBrightness(cfg.brightness);
  tft.setRotation(cfg.rotation);
  lastPoll = 0;                 // re-poll immediately with the new settings
  lastDwell = 0;
}

// ---------- LED ----------
// The CYD's RGB LED is common-anode: LOW turns a channel on.
static void led(bool r,bool g,bool b){
  digitalWrite(LED_R, r?LOW:HIGH);
  digitalWrite(LED_G, g?LOW:HIGH);
  digitalWrite(LED_B, b?LOW:HIGH);
}

// Ambient LED behaviour, driven by whatever is on screen.
static uint32_t ledNext=0; static bool ledPhase=false;
static void ledUpdate(){
  if(cfg.ledMode=="off"){ led(false,false,false); return; }
  if(state!=RUNNING){                      // status colours while starting up
    if(state==CONNECTING){ if(millis()>ledNext){ ledNext=millis()+500; ledPhase=!ledPhase;
                             led(ledPhase,ledPhase,false); } }
    else if(state==SETUP)  led(false,false,true);
    return;
  }

  // Emergencies always win, whatever the mode.
  if(haveFlight && current.emergency){
    if(millis()>ledNext){ ledNext=millis()+150; ledPhase=!ledPhase; led(ledPhase,false,false); }
    return;
  }

  if(cfg.ledNightOff){                     // quiet overnight
    // no RTC on board, so use uptime-agnostic simple gate: skip if brightness dimmed
    if(cfg.brightness<=15){ led(false,false,false); return; }
  }

  if(cfg.ledMode=="status"){ led(false,true,false); return; }

  if(!haveFlight){                         // nothing in range: slow blue breathe
    if(millis()>ledNext){ ledNext=millis()+1600; ledPhase=!ledPhase; led(false,false,ledPhase); }
    return;
  }

  if(cfg.ledMode=="proximity"){
    // pulse rate rises as the aircraft gets closer; solid white when overhead
    double d = current.distKm;
    uint32_t period = d<2? 120 : (uint32_t)constrain(d*60.0, 150.0, 2000.0);
    if(millis()>ledNext){ ledNext=millis()+period; ledPhase=!ledPhase;
                          led(ledPhase,ledPhase,ledPhase); }
    return;
  }

  if(cfg.ledMode=="density"){
    int n = aircraftInRange();
    uint32_t period = n<=0? 2000 : (uint32_t)constrain(1500 - n*60, 120, 1500);
    if(millis()>ledNext){ ledNext=millis()+period; ledPhase=!ledPhase;
                          led(false,ledPhase,ledPhase); }
    return;
  }

  // default: colour by aircraft class, matching the card on screen
  const char* k = current.icon;
  if(current.distKm < 2.0){ led(true,true,true); return; }        // overhead: white
  if(!strcmp(k,"military"))  led(true,false,false);
  else if(!strcmp(k,"heli")) led(true,false,true);
  else if(!strcmp(k,"light")||!strcmp(k,"bizjet")) led(false,true,false);
  else if(!strcmp(k,"turboprop")) led(false,true,true);
  else led(false,false,true);                                     // airliner / cargo
}

// ---------- reset actions (also called from the admin page) ----------
void doForgetWifi(){
  logf("resetting Wi-Fi credentials");
  led(true,true,false);
  drawResetting("Wi-Fi reset");
  cfg.wifiSsid=""; cfg.wifiPass=""; cfg.save();
  delay(1500); ESP.restart();
}
void doFactoryReset(){
  logf("FACTORY RESET - wiping all settings");
  led(true,false,false);
  drawResetting("Factory reset");
  LittleFS.remove("/config.json");
  LittleFS.format();
  delay(1800); ESP.restart();
}

// ---------- touch ----------
static void touchInit(){
  pinMode(T_IRQ, INPUT);
  pinMode(T_CS, OUTPUT); digitalWrite(T_CS, HIGH);
  tsSPI.begin(T_CLK, T_DO, T_DIN, T_CS);
}
static uint16_t tsCmd(uint8_t c){
  tsSPI.beginTransaction(SPISettings(1500000, MSBFIRST, SPI_MODE0));
  digitalWrite(T_CS, LOW);
  tsSPI.transfer(c);
  uint16_t hi = tsSPI.transfer(0x00);
  uint16_t lo = tsSPI.transfer(0x00);
  digitalWrite(T_CS, HIGH);
  tsSPI.endTransaction();
  return ((hi<<8)|lo) >> 3;
}
static bool touchRaw(int& x,int& y){
  if(digitalRead(T_IRQ)!=LOW) return false;
  long xs=0, ys=0; int n=0;
  for(int i=0;i<4 && digitalRead(T_IRQ)==LOW;i++){ xs+=tsCmd(0xD0); ys+=tsCmd(0x90); n++; }
  if(n<2) return false;
  x=xs/n; y=ys/n; return true;
}
// v3.34: just "is a finger down right now", no coordinates - used by ota.cpp
// to wait for a confirming tap once a downloaded update has finished flashing.
bool touchDown(){ return digitalRead(T_IRQ)==LOW; }
// The touch panel always reports in the display's NATIVE portrait orientation,
// while TFT_eSPI rotates only what we draw. So for the landscape rotations the
// axes have to be swapped before the numbers mean anything on screen.
static void mapTouch(int rx,int ry,int& sx,int& sy){
  int W=tft.width(), H=tft.height();
  int px = map(rx, cfg.tsx0, cfg.tsx1, 0, 240);   // native portrait X (0..240)
  int py = map(ry, cfg.tsy0, cfg.tsy1, 0, 320);   // native portrait Y (0..320)
  // Mapping derived from a measured press: the panel's axes are a straight
  // transpose of the landscape view (verified against raw(713,3563) landing on
  // the padlock at roughly x=304, y=18).
  switch(cfg.rotation){
    case 0:  sx = px;            sy = py;            break;   // portrait
    case 1:  sx = py;            sy = px;            break;   // landscape
    case 2:  sx = 240 - px;      sy = 320 - py;      break;   // portrait flipped
    default: sx = 320 - py;      sy = 240 - px;      break;   // landscape flipped
  }
  sx = constrain(sx, 0, W);
  sy = constrain(sy, 0, H);
}

// v3.13 tried adding swipe-to-skip here by waiting for release before acting,
// so a press could be told apart from the start of a swipe. It didn't work
// in practice and added a hair of latency to ordinary taps for no benefit,
// so v3.14 reverts to firing on the initial touch, same as before v3.13.
static void handleTap(int sx,int sy){
  if(infoPage){
    // Page 1 (QR/device info) -> page 2 (LED key) -> page 3 (radar) -> dismiss.
    // Radar no longer locks on tap (removed in v3.16 - too fiddly on this
    // screen size, and there are other ways to lock onto a flight) and no
    // longer times out on its own; any tap on it just dismisses back to the
    // flight card.
    if(infoPage==3){
      infoPage=0; infoShownAt=0; lastDwell=0;
      if(haveFlight) drawFlightCard(current,true,queuePosition(),queueCount());
    } else if(infoPage==1){
      infoPage=2; infoShownAt=millis();
      drawLedKey();
      logf("LED key shown");
    } else if(infoPage==2){
      infoPage=3; infoShownAt=millis(); lastRadarDraw=millis();
      radarSnapshot(radarBlips_);
      drawRadar(radarBlips_,(float)cfg.radiusKm,cfg.imperial);
      logf("radar shown");
    }
    return;
  }
  // v3.19: locking onto a flight is admin-page only now (tap a row in the
  // traffic table, or the Lock fields under "Lock to a flight") - the
  // touchscreen no longer has a lock/unlock button to hit, so any tap here
  // just brings up the device info screen with a scannable QR. Also show
  // the live gateway/subnet - the fastest way to spot a phone that's
  // actually on a different network/VLAN than this device (open the
  // phone's own Wi-Fi details screen and compare the two).
  infoPage=1; infoShownAt=millis();
  char net[40];
  snprintf(net,sizeof(net),"gw %s /%d%s", WiFi.gatewayIP().toString().c_str(),
           maskBits(WiFi.subnetMask()), cfg.useStaticIp?" (fixed)":" (DHCP)");
  drawDeviceInfo(WiFi.localIP().toString(), cfg.wifiSsid, (int)WiFi.RSSI(),
                 millis()/1000, String("v")+FW_VERSION, String(net));
  logf("info screen shown - admin at http://%s", WiFi.localIP().toString().c_str());
}

static void pollTouch(){
  if(millis()-lastTouch < 400) return;
  int rx,ry; if(!touchRaw(rx,ry)) return;
  int sx,sy; mapTouch(rx,ry,sx,sy);
  // v3.19: fixed dead zone on all four edges (see TOUCH_DEAD_PX near the top
  // of this file) - a case pressing anywhere in that margin is ignored
  // entirely. This never updates lastTouch, so a genuine touch just after
  // still registers normally.
  if(sx > tft.width() -TOUCH_DEAD_PX)  return;
  if(sx < TOUCH_DEAD_PX)               return;
  if(sy < TOUCH_DEAD_PX)               return;
  if(sy > tft.height()-TOUCH_DEAD_PX)  return;
  logf("touch raw(%d,%d) screen(%d,%d)", rx,ry,sx,sy);
  handleTap(sx,sy);
  lastTouch=millis();
}

// ---------- BOOT button: 5s = forget Wi-Fi, 10s = factory reset ----------
// NB: GPIO0 must NOT be held at power-up (that enters flash mode), so this is
// only ever read while the firmware is running.
static void pollBootButton(){
  bool down = (digitalRead(BOOT_BTN)==LOW);

  if(down && !btnWasDown){ btnDownAt=millis(); lastHeldSec=-1; countdownShown=false; }

  if(down){
    int held = (millis()-btnDownAt)/1000;
    if(held>=1){
      if(held!=lastHeldSec){
        lastHeldSec=held;
        drawResetCountdown(held);
        countdownShown=true;
        led(held>=5, held<5, false);            // cyan-ish then amber/red
      }
      if(held>=10){ btnWasDown=false; doFactoryReset(); }
    }
  } else if(btnWasDown){
    int held=(millis()-btnDownAt)/1000;
    led(false,false,false);
    if(held>=5 && held<10){ btnWasDown=false; doForgetWifi(); return; }
    if(countdownShown){                          // cancelled - repaint
      countdownShown=false;
      if(state==RUNNING){ if(haveFlight) drawFlightCard(current,true,queuePosition(),queueCount()); else lastDwell=0; }
      else if(state==CONNECTING) connectStart=connectStart;   // help screen repaints itself
    }
  }
  btnWasDown = down;
}

// number of set bits in a subnet mask, for compact "/24" style display
static int maskBits(const IPAddress& m){
  uint32_t v=(uint32_t)m; int n=0;
  for(int i=0;i<32;i++) if(v & (1UL<<i)) n++;
  return n;
}

// ---------- app flow ----------
static void startConnect(){
  WiFi.mode(WIFI_STA);
  // v3.14: hostname has to be set before begin() to have any chance of
  // reaching the router via DHCP option 12 - setting it after WL_CONNECTED
  // (as earlier versions did) is too late, the DHCP handshake is already
  // over by then. Some routers use that DHCP hostname to serve up their own
  // "name.local" or "name" resolution independently of this device's own
  // mDNS responder, so this ordering fix may help even where mDNS itself
  // wouldn't. It's still ultimately up to the router and the client's OS -
  // see the note on drawConnected() below.
  WiFi.setHostname(HOSTNAME);
  if(cfg.useStaticIp && cfg.staticIp.length()){
    IPAddress ip, gw, mask, dns;
    if(ip.fromString(cfg.staticIp) && gw.fromString(cfg.staticGw)
       && mask.fromString(cfg.staticMask)){
      // Sanity check: the gateway has to actually sit inside the static IP's
      // own subnet. A fixed IP that was set up for a router that's since
      // been replaced or reconfigured almost always fails this - and is a
      // classic "it used to work, now it doesn't" cause, since a device
      // left with a stale lease/route can still limp along on the old
      // subnet while anything joining fresh (like a phone) gets no route
      // to it at all. Rather than silently binding to a dead address,
      // fall back to DHCP and say so loudly in the log.
      uint32_t ipn=(uint32_t)ip, gwn=(uint32_t)gw, mk=(uint32_t)mask;
      if((ipn & mk) == (gwn & mk)){
        dns = gw;
        if(WiFi.config(ip,gw,mask,dns)) logf("static IP %s (gw %s /%d)",
             cfg.staticIp.c_str(), cfg.staticGw.c_str(), maskBits(mask));
        else logf("static IP config rejected by driver - using DHCP");
      } else {
        logf("FIXED IP IGNORED: %s and gateway %s aren't on the same subnet "
             "(mask %s) - falling back to DHCP. This usually means the fixed "
             "IP was set up for a router that has since changed; update or "
             "turn off 'Use a fixed IP' on the admin page.",
             cfg.staticIp.c_str(), cfg.staticGw.c_str(), cfg.staticMask.c_str());
      }
    } else {
      logf("fixed IP fields don't parse as addresses - using DHCP");
    }
  }
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
  state=CONNECTING; connectStart=millis();
  drawConnecting(0,"Saving settings",10); delay(300);
  drawConnecting(1,"Connecting to "+cfg.wifiSsid,40);
}

// Show the next aircraft from the queue built by the last master poll.
static void showNext(){
  Flight f;
  if(selectNext(f)){
    lockLostAt=0;
    current=f; haveFlight=true;
    drawFlightCard(current,true,queuePosition(),queueCount());
  } else if(cfg.lockOn && cfg.lockTarget.length()){
    // Locked but the target is not in range. Say so plainly rather than
    // leaving a stale card on screen that looks like the wrong aircraft.
    if(lockLostAt==0) lockLostAt=millis();
    haveFlight=false;
    drawLockWaiting(cfg.lockTarget,(millis()-lockLostAt)/1000, lockWasDescendingLow());
  } else {
    haveFlight=false;
    drawNoFlights(activeSource(), WiFi.localIP().toString());
  }
  portalSetCurrent(current, haveFlight);
}

void setup(){
  Serial.begin(115200);
  logInit();
  LittleFS.begin(true);
  cfg.load();
  otaInit();
  logf("Flight Eye v%s booting", FW_VERSION);

  pinMode(BOOT_BTN, INPUT_PULLUP);
  pinMode(LED_R,OUTPUT); pinMode(LED_G,OUTPUT); pinMode(LED_B,OUTPUT);
  led(false,false,false);

  displayInit();
  drawSplash();
  touchInit();
  delay(2500);

  if(!cfg.tsCalibrated) logf("touch not calibrated - run it from the admin page");

  if(!cfg.hasWifi()){
    portalBeginAP();
    drawSetup("FlightEye-Setup", WiFi.softAPIP().toString());
    state=SETUP;
  } else {
    startConnect();
  }
}

void loop(){
  portalLoop();
  pollBootButton();
  ledUpdate();
  otaLoop();   // no-ops unless Wi-Fi is connected

  switch(state){
    case CALIBRATE: {
      int rx,ry;
      if(millis()-calShownAt > 400 && touchRaw(rx,ry)){
        if(calStep==0){
          calRx0=rx; calRy0=ry;
          calStep=1; calShownAt=millis();
          logf("cal TL raw(%d,%d)",rx,ry);
          drawCalibratePrompt(1);
        } else {
          logf("cal BR raw(%d,%d)",rx,ry);
          // sanity check: the two corners must be meaningfully apart
          if(abs(rx-calRx0)>500 && abs(ry-calRy0)>500){
            cfg.tsx0 = min(calRx0,rx); cfg.tsx1 = max(calRx0,rx);
            cfg.tsy0 = min(calRy0,ry); cfg.tsy1 = max(calRy0,ry);
            cfg.tsCalibrated = true;
            cfg.save();
            logf("cal saved x %d..%d  y %d..%d",cfg.tsx0,cfg.tsx1,cfg.tsy0,cfg.tsy1);
            drawCalibrateDone(true);
          } else {
            logf("cal rejected - corners too close");
            drawCalibrateDone(false);
          }
          delay(1800);
          state = calReturnTo;
          lastDwell=0; lastPoll=0;
        }
      }
      break;
    }

    case SETUP:
      if(g_wifiSubmitted){ delay(600); ESP.restart(); }
      break;

    case CONNECTING: {
      if(WiFi.status()==WL_CONNECTED){
        drawConnecting(2,"Fetching live flights",80);
        // Hostname is set in startConnect(), before begin() - see the note
        // there. mDNS ".local" resolution itself still depends on the
        // client OS/browser understanding mDNS at all: it's built into
        // macOS/iOS, but Windows and Android generally need extra software
        // (e.g. Bonjour) and often just fail with something like
        // DNS_PROBE_FINISHED_NXDOMAIN. The IP address and the QR code on
        // the device screen always work regardless, so treat ".local" as a
        // nice-to-have, not the primary route to the admin page.
        if(MDNS.begin(HOSTNAME)){
          MDNS.addService("http","tcp",80);
          logf("mDNS up (.local resolution needs OS/browser support - IP or QR always works)");
        } else logf("mDNS failed to start");
        logf("WiFi ok: %s  rssi %d", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
        logf("Admin: http://flighteye.local");
        drawConnected(WiFi.localIP().toString(), String(HOSTNAME)+".local");
        portalBeginSTA();
        delay(4000);
        state=RUNNING; lastPoll=0; lastDwell=0;
        break;
      }
      uint32_t elapsed=(millis()-connectStart)/1000;
      // Keep retrying forever - never silently wipe the user's credentials.
      // After a minute, show how to recover using the BOOT button.
      static uint32_t lastPaint=0, lastKick=0;
      if(elapsed>=60 && !countdownShown && millis()-lastPaint>1000){
        lastPaint=millis();
        drawWifiHelp(cfg.wifiSsid, elapsed);
      }
      if(millis()-lastKick > 30000){             // periodic reconnect attempt
        lastKick=millis();
        WiFi.disconnect(); WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
        logf("retrying Wi-Fi (%us)", (unsigned)elapsed);
      }
      break;
    }

    case RUNNING:
      if(WiFi.status()!=WL_CONNECTED){          // dropped: go back to connecting
        logf("Wi-Fi lost - reconnecting");
        startConnect(); break;
      }
      // v3.14 originally kicked off NTP (configTime()) right after Wi-Fi
      // connected, before the web server started - if that call ever stalled
      // for any reason (slow/broken DNS for the NTP pool, a flaky network),
      // it would have delayed portalBeginSTA() and left the admin page
      // completely unreachable (ERR_CONNECTION_TIMED_OUT), not just ".local"
      // unreachable. Doing it here instead, once, well after the web server
      // is already listening, means NTP can never hold up the admin page
      // again regardless of what it does.
      static bool ntpStarted=false;
      if(!ntpStarted){
        ntpStarted=true;
        configTime(0,0,"pool.ntp.org","time.nist.gov");
        logf("NTP sync requested");
      }
      if(!countdownShown) pollTouch();

      // Master poll: refresh the whole picture of the sky.
      if(lastPoll==0 || millis()-lastPoll > (uint32_t)cfg.pollSec*1000){
        lastPoll=millis();
        pollTraffic();
        lastDwell=0;                     // start the rotation from the top
      }

      // Device info / LED key time out on their own after 20s. Radar doesn't -
      // it stays up until touched, since it's meant to be watched for a while.
      if(infoPage && infoPage!=3 && millis()-infoShownAt > 20000){
        infoPage=0; infoShownAt=0; lastDwell=0;
      }
      // Radar redraws periodically to reflect dead reckoning. No offscreen
      // buffer, so every redraw is a full fillScreen+repaint (see
      // display.cpp) - v3.13 dropped the sweep animation (it flickered and
      // didn't show anything real), so there's no need for a fast tick
      // anymore; ~1s is plenty since aircraft barely move at this scale.
      if(infoPage==3 && millis()-lastRadarDraw > 1000){
        lastRadarDraw=millis();
        radarSnapshot(radarBlips_);
        drawRadar(radarBlips_,(float)cfg.radiusKm,cfg.imperial);
      }
      if(infoPage) break;                // hold whichever info page is up

      // A locked aircraft is refreshed at the dwell rate. Repeated misses back
      // off so a stale target can never monopolise the loop.
      if(cfg.lockOn && cfg.lockTarget.length()){
        uint32_t interval = (uint32_t)cfg.dwellSec*1000;
        if(lockMisses>2)  interval = 30000;
        if(lockMisses>6)  interval = 120000;
        if(lastLockPoll==0 || millis()-lastLockPoll > interval){
          lastLockPoll=millis();
          if(pollLocked()) lockMisses=0;
          else if(lockMisses<20) lockMisses++;
        }
      } else { lastLockPoll=0; lockMisses=0; }

      // Display cycle: step through the queue we already have.
      if(lastDwell==0 || millis()-lastDwell > (uint32_t)cfg.dwellSec*1000){
        lastDwell=millis();
        showNext();
      }
      break;

    default: break;
  }
}
