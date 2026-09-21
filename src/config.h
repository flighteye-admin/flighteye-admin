#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// Bumped whenever a release goes out. Compared (as major.minor.patch) against
// GitHub release tag names by ota.cpp - keep it in sync with the git tag you
// push (tag "v3.26" <-> FW_VERSION "3.26").
#define FW_VERSION "3.26"

// Everything the admin portal can change, persisted to /config.json in LittleFS.
struct Config {
  // network
  String wifiSsid, wifiPass;
  // location & tracking
  float  homeLat = 51.4700f, homeLon = -0.4543f;
  int    radiusKm = 30;
  // v3.25: true once a home location has actually been saved (by hand, or
  // via the auto-geolocate-on-first-setup below) - lets the admin page tell
  // "never configured yet" apart from "deliberately left at the default",
  // so the phone-location auto-fill only ever fires once, not on every visit.
  bool   homeSet = false;
  String featured = "nearest";          // sort order: nearest | lowest
  int    pollSec    = 60;               // master traffic poll, 30..120
  int    dwellSec   = 8;                // seconds each aircraft stays on screen
  int    refreshSec = 8;                // legacy, kept so old config.json still loads
  // display
  int    brightness = 80;               // %
  int    rotation = 1;                  // 0..3
  bool   imperial = true;
  bool   dimOvernight = true;
  bool   callsignIata = false;          // false = ICAO (UAL263), true = IATA (UA263)
  // aircraft filters
  bool   fCommercial=true, fPrivate=false, fCargo=true,
         fMilitary=false, fHeli=false, fEmergency=true, fJumpEmerg=true;
  // data sources (all keyless / free community feeds)
  bool   sAdsbLol=true, sAdsbFi=true, sAdsbOne=false;   // adsb.one returns 403 for non-feeders; airplanes.live removed in v3.26 (feeder-only now)
  bool   mergeSources=true;             // true = query all enabled & merge; false = failover
  // lock
  bool   lockOn=false; String lockTarget;
  // atc (stub)
  bool   atcOn=false; int atcVol=45;
  // networking
  bool   useStaticIp=false;
  String staticIp, staticGw, staticMask="255.255.255.0";
  // LED
  String ledMode="class";       // off | status | class | proximity | density
  bool   ledNightOff=true;
  // touch calibration (raw XPT2046 values at the two opposite corners)
  bool   tsCalibrated=false;
  int    tsx0=250, tsy0=250, tsx1=3850, tsy1=3850;
  // v3.17/v3.18 had a per-edge, admin-adjustable touch dead zone here. v3.19
  // replaced it with a single fixed 50px margin on all four edges (see
  // TOUCH_DEAD_PX in main.cpp) - simpler, and there was nothing left worth
  // tuning once all four edges needed covering anyway. No longer persisted.

  void toJson(JsonDocument& d) const {
    d["wifiSsid"]=wifiSsid;       d["wifiPass"]=wifiPass;
    d["homeLat"]=homeLat;         d["homeLon"]=homeLon;       d["radiusKm"]=radiusKm;
    d["homeSet"]=homeSet;
    d["featured"]=featured;       d["refreshSec"]=refreshSec;
    d["pollSec"]=pollSec;         d["dwellSec"]=dwellSec;
    d["brightness"]=brightness;   d["rotation"]=rotation;
    d["imperial"]=imperial;       d["dimOvernight"]=dimOvernight;  d["callsignIata"]=callsignIata;
    d["fCommercial"]=fCommercial; d["fPrivate"]=fPrivate;     d["fCargo"]=fCargo;
    d["fMilitary"]=fMilitary;     d["fHeli"]=fHeli;           d["fEmergency"]=fEmergency;
    d["fJumpEmerg"]=fJumpEmerg;
    d["sAdsbLol"]=sAdsbLol;
    d["sAdsbFi"]=sAdsbFi;         d["sAdsbOne"]=sAdsbOne;      d["mergeSources"]=mergeSources;
    d["lockOn"]=lockOn;           d["lockTarget"]=lockTarget;
    d["atcOn"]=atcOn;             d["atcVol"]=atcVol;
    d["useStaticIp"]=useStaticIp; d["staticIp"]=staticIp;
    d["staticGw"]=staticGw;       d["staticMask"]=staticMask;
    d["ledMode"]=ledMode;         d["ledNightOff"]=ledNightOff;
    d["tsCalibrated"]=tsCalibrated;
    d["tsx0"]=tsx0; d["tsy0"]=tsy0; d["tsx1"]=tsx1; d["tsy1"]=tsy1;
  }
  void fromJson(JsonDocument& d) {
    wifiSsid=d["wifiSsid"]|wifiSsid;         wifiPass=d["wifiPass"]|wifiPass;
    homeLat=d["homeLat"]|homeLat;            homeLon=d["homeLon"]|homeLon;   radiusKm=d["radiusKm"]|radiusKm;
    homeSet=d["homeSet"]|homeSet;
    featured=d["featured"]|featured;         refreshSec=d["refreshSec"]|refreshSec;
    pollSec=d["pollSec"]|pollSec;            dwellSec=d["dwellSec"]|dwellSec;
    if(pollSec<30) pollSec=30;  if(pollSec>120) pollSec=120;
    if(dwellSec<3) dwellSec=3;  if(dwellSec>60) dwellSec=60;
    brightness=d["brightness"]|brightness;   rotation=d["rotation"]|rotation;
    imperial=d["imperial"]|imperial;         dimOvernight=d["dimOvernight"]|dimOvernight;
    callsignIata=d["callsignIata"]|callsignIata;
    fCommercial=d["fCommercial"]|fCommercial; fPrivate=d["fPrivate"]|fPrivate; fCargo=d["fCargo"]|fCargo;
    fMilitary=d["fMilitary"]|fMilitary;      fHeli=d["fHeli"]|fHeli;         fEmergency=d["fEmergency"]|fEmergency;
    fJumpEmerg=d["fJumpEmerg"]|fJumpEmerg;
    sAdsbLol=d["sAdsbLol"]|sAdsbLol;
    sAdsbFi=d["sAdsbFi"]|sAdsbFi;            sAdsbOne=d["sAdsbOne"]|sAdsbOne;
    mergeSources=d["mergeSources"]|mergeSources;
    lockOn=d["lockOn"]|lockOn;               lockTarget=d["lockTarget"]|lockTarget;
    atcOn=d["atcOn"]|atcOn;                  atcVol=d["atcVol"]|atcVol;
    useStaticIp=d["useStaticIp"]|useStaticIp; staticIp=d["staticIp"]|staticIp;
    staticGw=d["staticGw"]|staticGw;          staticMask=d["staticMask"]|staticMask;
    ledMode=d["ledMode"]|ledMode;             ledNightOff=d["ledNightOff"]|ledNightOff;
    tsCalibrated=d["tsCalibrated"]|tsCalibrated;
    tsx0=d["tsx0"]|tsx0; tsy0=d["tsy0"]|tsy0; tsx1=d["tsx1"]|tsx1; tsy1=d["tsy1"]|tsy1;
  }
  bool load() {
    File f=LittleFS.open("/config.json","r"); if(!f) return false;
    JsonDocument d; auto err=deserializeJson(d,f); f.close();
    if(err) return false; fromJson(d); return true;
  }
  bool save() {
    JsonDocument d; toJson(d);
    File f=LittleFS.open("/config.json","w"); if(!f) return false;
    serializeJson(d,f); f.close(); return true;
  }
  bool hasWifi() const { return wifiSsid.length()>0; }
};

extern Config cfg;
