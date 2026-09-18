#pragma once
#include <Arduino.h>
#include <vector>
#include "config.h"

struct Flight {
  String hex, callsign, csIata, type, typeName, reg, airline, cat;
  String originIata, destIata, originCity, destCity;
  double lat=0, lon=0;
  int    altFt=0, gs=0, track=0, squawk=0, vsFpm=0;
  uint32_t dbFlags=0;
  bool   emergency=false, onGround=false, enriched=false;
  double distKm=0;
  const char* icon="airliner";     // airliner|turboprop|bizjet|light|heli|military
  bool   valid=false;
};

// One row of the "traffic in range" list shown on the admin page.
struct TrafficRow {
  String hex, callsign, type, icon, reason;
  int    altFt=0, gs=0;
  double distKm=0;
  bool   included=false, featured=false;
};

#define TRAFFIC_MAX 25

// v3.12: a single aircraft's position on the radar screen, dead-reckoned to
// "now" from its last known fix. Only one String field (callsign, needed so
// a tap on the radar can lock onto it) to keep this cheap to rebuild on
// every redraw - deliberately not reusing the heavier Flight/TrafficRow.
struct RadarBlip {
  String callsign;
  double distKm=0, bearingDeg=0;    // from home; bearingDeg 0=N, 90=E, ...
  int    track=0, gs=0, altFt=0;
  const char* icon="airliner";
  bool   emergency=false;
};

// --- master poll: fetch all sources, filter, sort, build the display queue ---
bool pollTraffic();

// --- step to the next aircraft in the queue (call every dwellSec) ---
bool selectNext(Flight& out);

int          aircraftInRange();
const char*  activeSource();
const std::vector<TrafficRow>& trafficList();
// UTC seconds of the last successful poll (0 if none yet, or NTP hasn't
// synced) - the admin page converts this to the viewer's local time.
uint32_t     lastPollEpoch();
int          queueCount();        // aircraft in the display rotation
int          queuePosition();     // 1-based position currently shown
bool         lockedInRange();     // is the locked target currently visible?
bool         pollLocked();        // refresh just the locked aircraft (fast path)
void         resetLockCache();    // forget the cached endpoint
bool         lockWasDescendingLow();   // last seen low and descending?

// Current (dead-reckoned) positions of everything in the display rotation -
// the same aircraft, and the same filters, as the flight card shows. Pure
// math against the last poll's data, no network - cheap enough to call on
// every radar redraw.
void         radarSnapshot(std::vector<RadarBlip>& out);
