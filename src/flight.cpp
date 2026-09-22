#include "flight.h"
#include "devlog.h"
#include "aircraft_types.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <math.h>
#include <time.h>

// Sent on every outbound HTTP request. A bare "no User-Agent" or a generic
// placeholder one gets 403'd by some of these free aggregators (adsb.lol
// spells out why: "User-Agent too generic; include valid contact info").
// v3.22 tried an email-only contact address, but adsb.lol kept rejecting it
// as "too generic" - turns out the accepted shape (confirmed against a
// working real-world example, github.com/NIKX-Tech/karshipta PR #202) is
// "AppName/version (+https://url; contact@email)" - an app identity *and*
// a link, not just an address. v3.23 gave this project a real public repo,
// so v3.26 uses that as the link.
//
// (airplanes.live was dropped entirely as a source in this same v3.26, and
// adsb.lol itself followed in v3.29 - both went feeder-only; see README.md.)
static const char* kUserAgent =
  "FlightEye-ESP32/3.35 (+https://github.com/flighteye-admin/flighteye-admin; genereynolds.uk+flighteye@gmail.com)";

static int    s_count = 0;
static String s_source = "-";
static std::vector<TrafficRow> s_traffic;
static uint32_t s_lastPollAt = 0;    // millis() of the last successful poll, for dead reckoning
static uint32_t s_lastPollEpoch = 0; // wall-clock UTC seconds of the last poll (0 = unknown/not synced)
uint32_t lastPollEpoch(){ return s_lastPollEpoch; }

// The display queue: aircraft that passed the filters, held between master
// polls so the screen can rotate through them without re-fetching.
static std::vector<Flight> s_queue;
static int s_qpos = -1;
// The locked aircraft is held separately: it must survive even when the
// filters would exclude it, and we need to know whether it is still in range.
static Flight s_locked;
static bool   s_lockedFound = false;
static int    s_lastAlt = -1;        // last known altitude of the locked aircraft
static int    s_lastVs  = 0;         // last known vertical rate
bool lockWasDescendingLow(){ return s_lastAlt>0 && s_lastAlt<6000 && s_lastVs<-100; }
bool lockedInRange(){ return s_lockedFound; }

int         aircraftInRange() { return s_count; }
const char* activeSource()    { return s_source.c_str(); }
const std::vector<TrafficRow>& trafficList(){ return s_traffic; }
int         queueCount()    { return (int)s_queue.size(); }
int         queuePosition() { return s_queue.empty()? 0 : s_qpos+1; }

// ---- helpers ---------------------------------------------------------------
static double haversineKm(double la1,double lo1,double la2,double lo2){
  double R=6371.0, dLa=radians(la2-la1), dLo=radians(lo2-lo1);
  double a=sin(dLa/2)*sin(dLa/2)+cos(radians(la1))*cos(radians(la2))*sin(dLo/2)*sin(dLo/2);
  return R*2*atan2(sqrt(a),sqrt(1-a));
}
// Forward azimuth from (la1,lo1) to (la2,lo2), degrees, 0=N 90=E 180=S 270=W.
static double bearingDeg(double la1,double lo1,double la2,double lo2){
  double p1=radians(la1), p2=radians(la2), dl=radians(lo2-lo1);
  double y=sin(dl)*cos(p2);
  double x=cos(p1)*sin(p2)-sin(p1)*cos(p2)*cos(dl);
  double b=degrees(atan2(y,x));
  return fmod(b+360.0,360.0);
}
// Destination point given a start, a bearing (degrees) and a distance (km) -
// the "project forward" half of dead reckoning. Same great-circle formula
// family as haversineKm/bearingDeg above, just run the other way.
static void project(double la1,double lo1,double bearing,double distKm,double& la2,double& lo2){
  double R=6371.0, d=distKm/R, br=radians(bearing), p1=radians(la1);
  double p2=asin(sin(p1)*cos(d)+cos(p1)*sin(d)*cos(br));
  double l2=radians(lo1)+atan2(sin(br)*sin(d)*cos(p1), cos(d)-sin(p1)*sin(p2));
  la2=degrees(p2); lo2=degrees(l2);
}
static bool hasAny(const String& T, std::initializer_list<const char*> keys){
  for(auto k:keys) if(T.indexOf(k)>=0) return true; return false;
}
static String upper(const String& s){ String t=s; t.toUpperCase(); return t; }

// ---- airline / operator lookup (ICAO prefix -> name), incl. cargo ----------
static String airlineFromPrefix(const String& cs){
  String p=upper(cs.substring(0,3));
  if(p=="BAW")return"British Airways";   if(p=="EZY")return"easyJet";
  if(p=="RYR")return"Ryanair";           if(p=="VIR")return"Virgin Atlantic";
  if(p=="TOM")return"TUI Airways";       if(p=="EXS")return"Jet2";
  if(p=="LOG")return"Loganair";          if(p=="BEE")return"Blue Islands";
  if(p=="DLH")return"Lufthansa";         if(p=="AFR")return"Air France";
  if(p=="KLM")return"KLM";               if(p=="IBE")return"Iberia";
  if(p=="SWR")return"SWISS";             if(p=="AUA")return"Austrian";
  if(p=="SAS")return"SAS";               if(p=="FIN")return"Finnair";
  if(p=="TAP")return"TAP Portugal";      if(p=="AEE")return"Aegean";
  if(p=="WZZ")return"Wizz Air";          if(p=="VLG")return"Vueling";
  if(p=="EIN")return"Aer Lingus";        if(p=="NAX"||p=="NOZ")return"Norwegian";
  if(p=="THY")return"Turkish Airlines";  if(p=="ITY")return"ITA Airways";
  if(p=="UAL")return"United";            if(p=="AAL")return"American Airlines";
  if(p=="DAL")return"Delta";             if(p=="ACA")return"Air Canada";
  if(p=="JBU")return"JetBlue";           if(p=="WJA")return"WestJet";
  if(p=="UAE")return"Emirates";          if(p=="QTR")return"Qatar Airways";
  if(p=="ETD")return"Etihad";            if(p=="SIA")return"Singapore Airlines";
  if(p=="QFA")return"Qantas";            if(p=="ANA")return"All Nippon";
  if(p=="JAL")return"Japan Airlines";    if(p=="CPA")return"Cathay Pacific";
  if(p=="AIC")return"Air India";
  if(p=="CLX")return"Cargolux";          if(p=="FDX")return"FedEx";
  if(p=="UPS")return"UPS Airlines";      if(p=="GTI")return"Atlas Air";
  if(p=="BOX")return"AeroLogic";         if(p=="ABW")return"AirBridgeCargo";
  if(p=="CKS")return"Kalitta Air";       if(p=="GEC")return"Lufthansa Cargo";
  if(p=="BCS")return"European Air Transport"; if(p=="DHK")return"DHL Air UK";
  if(p=="MPH")return"Martinair";         if(p=="TAY")return"ASL Airlines";
  if(p=="SQC")return"Singapore Cargo";   if(p=="CAO")return"Air China Cargo";
  if(p=="NCA")return"Nippon Cargo";      if(p=="ETH")return"Ethiopian";
  if(p=="SWN")return"West Air";          if(p=="QAC")return"Qatar Cargo";
  if(p=="RRR")return"Royal Air Force";   if(p=="RCH")return"US Air Mobility";
  if(p=="NJE")return"NetJets";
  return "";
}
static bool isCargoPrefix(const String& cs){
  String p=upper(cs.substring(0,3));
  return p=="CLX"||p=="FDX"||p=="UPS"||p=="GTI"||p=="BOX"||p=="ABW"||p=="CKS"||
         p=="GEC"||p=="BCS"||p=="DHK"||p=="MPH"||p=="TAY"||p=="SQC"||p=="CAO"||
         p=="NCA"||p=="QAC"||p=="SWN";
}

static const char* classify(const String& cat, const String& type,
                            uint32_t dbFlags, const String& cs, const String& reg){
  String T=upper(type);
  if(dbFlags & 1) return "military";
  if(hasAny(T,{"EUFI","TYPH","F15","F16","F18","F22","F35","HAWK","GR4","RFAL","A400","C130","C30J","C17","H47","K35"}))
    return "military";
  if(cat=="A7"||cat=="B1") return "heli";
  if(cat=="A1")            return "light";
  if(cat=="A2")            return "light";
  if(cat=="A3"||cat=="A4"||cat=="A5"){
    if(hasAny(T,{"AT4","AT7","AT5","DH8","DHC","SF3","D328"})) return "turboprop";
    return "airliner";
  }
  if(hasAny(T,{"H145","H160","EC","AS35","AS55","AS65","R22","R44","R66","B06","A109","A119","A139","A169","A189","PUMA","H175","S76","S92","EH10","LYNX","UH60"})) return "heli";
  if(hasAny(T,{"AT4","AT7","AT5","DH8","DHC","SF3","SB20","B190","E110","D228","J328","TBM","PC12","C208","SW4","L410","P180"})) return "turboprop";
  if(hasAny(T,{"GLF","GLEX","GL5","GL6","GL7","CL30","CL35","CL60","LJ","C25","C50","C51","C52","C55","C56","C65","C68","C70","C75","E50","E55","E54","F2TH","F900","FA7X","FA8X","H25","HA4T","PRM","GALX","BE40","BE20","BE9L","B350","HDJT","SF50"})) return "bizjet";
  if(hasAny(T,{"C15","C16","C17","C18","C20","C21","C31","C33","C40","C42","P28","P32","PA1","PA2","PA3","PA4","SR2","S22","DA4","DA6","DV20","DR40","DR22","AA5","BE3","BE5","BE7","M20","RV","EUPA","EV97","AT3","SLNG","YK52","GLID","BALL","ULAC","GYRO"})) return "light";
  if(cs.length() && reg.length()){
    String a=upper(cs), b=upper(reg); b.replace("-","");
    if(a==b) return "light";
  }
  return "airliner";
}

// ---- route enrichment: adsbdb.com first, hexdb.io as a fallback ------------
// (both free, both keyless), cached by callsign so this only ever runs once
// per aircraft actually shown on screen or locked - never once per poll for
// everything in range.
static String s_key, s_org, s_dst, s_orgCity, s_dstCity, s_air, s_iata;
static void applyCache(Flight& f){
  f.originIata=s_org; f.destIata=s_dst; f.originCity=s_orgCity; f.destCity=s_dstCity;
  if(s_air.length())  f.airline=s_air;
  if(s_iata.length()) f.csIata=s_iata;
}

// v3.30: adsbdb doesn't have every callsign - it's the fuller source when it
// does have a hit (airline name, IATA-format callsign too), but "no route
// filed" from adsbdb sometimes just means adsbdb hasn't logged this one,
// not that no route exists. hexdb.io is a second, differently-sourced free
// route database; when it has the callsign it only gives ICAO airport
// codes, so a couple of follow-up airport lookups turn those into the
// IATA code + name the display wants. Only reached when adsbdb came back
// empty, so the extra requests are rare, not per-poll.
static bool hexdbAirport(const String& icao, String& iata, String& name){
  if(icao.length()!=4) return false;
  if(!heapOkForTls()){ logf("hexdb: skipped (heap block %uB)", largestFreeBlock()); return false; }
  WiFiClientSecure c; c.setInsecure();
  HTTPClient http; http.setConnectTimeout(4000); http.setTimeout(4000);
  if(!http.begin(c,"https://hexdb.io/api/v1/airport/icao/"+icao)) return false;
  http.addHeader("User-Agent",kUserAgent);
  bool ok=false;
  if(http.GET()==200){
    JsonDocument d;
    if(!deserializeJson(d,http.getStream())){
      iata=d["iata"]|""; name=d["airport"]|"";
      ok = iata.length()>0 || name.length()>0;
    }
  }
  http.end();
  return ok;
}
static bool hexdbEnrich(Flight& f){
  if(!heapOkForTls()){ logf("hexdb: skipped (heap block %uB)", largestFreeBlock()); return false; }
  WiFiClientSecure c; c.setInsecure();
  HTTPClient http; http.setConnectTimeout(4000); http.setTimeout(4000);
  if(!http.begin(c,"https://hexdb.io/api/v1/route/icao/"+f.callsign)) return false;
  http.addHeader("User-Agent",kUserAgent);
  String oIcao,dIcao;
  if(http.GET()==200){
    JsonDocument d;
    if(!deserializeJson(d,http.getStream())){
      String route=d["route"]|"";
      int dash=route.indexOf('-');
      if(dash>0){ oIcao=route.substring(0,dash); dIcao=route.substring(dash+1); }
    }
  }
  http.end();
  if(oIcao.length()!=4 && dIcao.length()!=4) return false;

  String oIata,oName,dIata,dName;
  hexdbAirport(oIcao,oIata,oName);
  hexdbAirport(dIcao,dIata,dName);
  if(!oIata.length() && !dIata.length()) return false;

  f.originIata=oIata; f.destIata=dIata;
  f.originCity=oName; f.destCity=dName;
  return true;
}

// v3.31: tryHexdbFallback is only ever passed true for the locked aircraft
// (see the call sites below). Early on, this ran for every aircraft that
// rotated onto screen - with 50+ aircraft in range near a busy airport, that
// meant a new, never-before-seen callsign roughly every dwellSec, each one
// adsbdb misses on stacking up to 3 more back-to-back secure connections on
// top. Repeated back-to-back TLS connections are a known source of random
// ESP32 reboots (well documented in the arduino-esp32 core's own issue
// tracker), and the timing lined up exactly with when this started - so the
// fallback now only fires for the one aircraft you've deliberately locked
// onto, not for everything passing through the rotation.
// v3.33: the rotation branch (tryHexdbFallback=false) used to open a fresh
// adsbdb TLS connection for literally every never-before-seen aircraft that
// dwelled onto screen - at an 8s dwell and 25+ distinct aircraft in a busy
// hour, that's a new handshake every few seconds, hour after hour. That's
// the same "repeated back-to-back TLS connections" pattern that caused the
// v3.30 hexdb crash, just spread out more - v3.31 only removed the *extra*
// hexdb calls on top of it, not this baseline one. A locked aircraft is
// re-enriched at most once per callsign change (see selectNext()) so it's
// exempt from this; the rotation gets a floor between attempts instead.
static uint32_t s_lastRotationEnrichMs = 0;
static const uint32_t kRotationEnrichGapMs = 12000;

static void enrich(Flight& f, bool tryHexdbFallback){
  if(f.callsign.length()<3){ if(!f.airline.length()) f.airline=airlineFromPrefix(f.callsign); return; }
  if(f.callsign==s_key){ applyCache(f); return; }

  if(!tryHexdbFallback){
    uint32_t now=millis();
    if(now - s_lastRotationEnrichMs < kRotationEnrichGapMs){
      if(!f.airline.length()) f.airline=airlineFromPrefix(f.callsign);
      return;                          // too soon - this one just shows without route/airline
    }
    s_lastRotationEnrichMs = now;
  }

  if(!heapOkForTls()){
    logf("enrich: skipped %s (heap block %uB)", f.callsign.c_str(), largestFreeBlock());
    if(!f.airline.length()) f.airline=airlineFromPrefix(f.callsign);
    return;
  }

  bool gotRoute=false;
  WiFiClientSecure c; c.setInsecure();
  HTTPClient http; http.setConnectTimeout(4000); http.setTimeout(4000);
  String url="https://api.adsbdb.com/v0/callsign/"+f.callsign;
  if(http.begin(c,url)){
    http.addHeader("User-Agent",kUserAgent);
    int code=http.GET();
    if(code==200){
      JsonDocument d;
      if(!deserializeJson(d,http.getStream())){
        auto fr=d["response"]["flightroute"];
        f.originIata =fr["origin"]["iata_code"]|"";
        f.destIata   =fr["destination"]["iata_code"]|"";
        f.originCity =fr["origin"]["municipality"]|"";
        f.destCity   =fr["destination"]["municipality"]|"";
        String al  =fr["airline"]["name"]|"";
        if(al.length()) f.airline=al;
        String iata=fr["callsign_iata"]|"";  if(iata.length()) f.csIata=iata;
        gotRoute = f.originIata.length()>0 || f.destIata.length()>0;
      }
    }
    http.end();
  }
  if(!f.airline.length()) f.airline=airlineFromPrefix(f.callsign);

  if(!gotRoute && tryHexdbFallback && hexdbEnrich(f)) gotRoute=true;

  s_key=f.callsign; s_org=f.originIata; s_dst=f.destIata;
  s_orgCity=f.originCity; s_dstCity=f.destCity; s_air=f.airline; s_iata=f.csIata;
  (void)gotRoute;
}

// ---- sources ---------------------------------------------------------------
struct Source { const char* name; const char* fmt; bool enabled; };

static void mergeInto(std::vector<Flight>& all, Flight& f){
  for(auto& e:all){
    if(e.hex==f.hex && e.hex.length()){
      if(!e.callsign.length() && f.callsign.length()) e.callsign=f.callsign;
      if(!e.reg.length()      && f.reg.length())      e.reg=f.reg;
      if(!e.type.length()     && f.type.length())     e.type=f.type;
      if(!e.cat.length()      && f.cat.length())      e.cat=f.cat;
      if(e.gs==0     && f.gs!=0)     e.gs=f.gs;
      if(e.track==0  && f.track!=0)  e.track=f.track;
      if(e.altFt==0  && f.altFt!=0)  e.altFt=f.altFt;
      if(e.vsFpm==0  && f.vsFpm!=0)  e.vsFpm=f.vsFpm;
      if(e.squawk==0 && f.squawk!=0) e.squawk=f.squawk;
      if(!e.dbFlags  && f.dbFlags)   e.dbFlags=f.dbFlags;
      return;
    }
  }
  all.push_back(f);
}

// returns: 1 ok, 0 soft failure (retryable), -1 hard failure
static int fetchSource(const Source& src, std::vector<Flight>& all, int& added){
  double nm = cfg.radiusKm/1.852; if(nm>250) nm=250;
  char url[192];
  snprintf(url,sizeof(url),src.fmt,cfg.homeLat,cfg.homeLon,nm);

  if(!heapOkForTls()){ logf("%s: skipped, low heap (block %uB)", src.name, largestFreeBlock()); return 0; }
  WiFiClientSecure c; c.setInsecure();
  HTTPClient http; http.setConnectTimeout(5000); http.setTimeout(7000);
  if(!http.begin(c,url)) return -1;
  http.addHeader("User-Agent",kUserAgent);
  int code=http.GET();
  if(code!=200){
    // v3.21: log the response body too, not just the status - a bare "HTTP 403"
    // doesn't say whether that's a generic block or the source telling us
    // something specific (a few of these free aggregators return a plain-text
    // reason, e.g. asking you to register a project before they'll serve you).
    String body=http.getString();
    body.replace('\r',' '); body.replace('\n',' ');
    http.end();
    logf("%s HTTP %d: %s",src.name,code,body.c_str());
    return -1;
  }

  // Guard against HTML error pages / truncated bodies being fed to the parser.
  String ctype = http.header("Content-Type");
  if(ctype.length() && ctype.indexOf("json")<0){ http.end(); return 0; }

  // v3.28: buffer the raw body instead of streaming straight into the parser,
  // so that if this source ends up adding zero aircraft we can log a snippet
  // of what it actually sent back. A source silently soft-blocking a request
  // (200 OK, but an empty "ac") looks identical to a genuinely quiet sky in
  // every log line we had before this - and the two need completely
  // different fixes, so we need to be able to tell them apart.
  String raw = http.getString();
  http.end();

  JsonDocument filter;
  JsonObject fa=filter["ac"].add<JsonObject>();
  for(const char* k:{"hex","flight","r","t","alt_baro","gs","track","lat","lon",
                     "squawk","category","dbFlags","baro_rate","geom_rate","emergency"}) fa[k]=true;

  JsonDocument doc;
  auto err=deserializeJson(doc,raw,DeserializationOption::Filter(filter));
  if(err) return 0;                       // soft: caller may retry once

  int before=all.size();
  for(JsonObject a: doc["ac"].as<JsonArray>()){
    Flight f;
    f.hex=a["hex"]|"";
    f.callsign=((const char*)(a["flight"]|"")); f.callsign.trim();
    f.reg=a["r"]|""; f.type=a["t"]|""; f.cat=a["category"]|"";
    f.lat=a["lat"]|0.0; f.lon=a["lon"]|0.0;
    JsonVariant alt=a["alt_baro"];
    if(alt.is<const char*>()){ f.onGround = strcmp(alt.as<const char*>(),"ground")==0; f.altFt=0; }
    else { f.altFt=(int)lround(alt.as<float>()); }
    f.gs    = (int)lround(a["gs"].as<float>());
    f.track = (int)lround(a["track"].as<float>());
    f.vsFpm = (int)lround(a["baro_rate"].as<float>());
    if(f.vsFpm==0) f.vsFpm=(int)lround(a["geom_rate"].as<float>());
    f.squawk = atoi((const char*)(a["squawk"]|"0"));
    f.dbFlags=a["dbFlags"]|0;
    String em=a["emergency"]|"none";
    f.emergency = (em!="none"&&em!="") || f.squawk==7500||f.squawk==7600||f.squawk==7700;
    f.distKm=haversineKm(cfg.homeLat,cfg.homeLon,f.lat,f.lon);
    f.valid=true;
    mergeInto(all,f);
  }
  added = all.size()-before;
  if(added==0){
    String snippet = raw.substring(0, 180);
    snippet.replace('\r',' '); snippet.replace('\n',' ');
    logf("%s 0 added - body: %s (len %u)", src.name, snippet.c_str(), (unsigned)raw.length());
  }
  return 1;
}

// ---------------------------------------------------------------------------
// Global lookup for a locked aircraft. Unlike the area poll this is NOT bound
// to the search radius: /v2/callsign, /v2/hex and /v2/reg match worldwide, so a
// locked aircraft is followed for its whole flight, however far away it goes.
// ---------------------------------------------------------------------------
static bool parseOne(HTTPClient& http, Flight& out){
  JsonDocument filter;
  JsonObject fa=filter["ac"].add<JsonObject>();
  for(const char* k:{"hex","flight","r","t","alt_baro","gs","track","lat","lon",
                     "squawk","category","dbFlags","baro_rate","geom_rate","emergency"}) fa[k]=true;
  JsonDocument doc;
  if(deserializeJson(doc,http.getStream(),DeserializationOption::Filter(filter))) return false;
  JsonArray arr = doc["ac"].as<JsonArray>();
  if(arr.isNull() || arr.size()==0) return false;

  JsonObject a = arr[0];
  Flight f;
  f.hex=a["hex"]|"";
  f.callsign=((const char*)(a["flight"]|"")); f.callsign.trim();
  f.reg=a["r"]|""; f.type=a["t"]|""; f.cat=a["category"]|"";
  f.lat=a["lat"]|0.0; f.lon=a["lon"]|0.0;
  JsonVariant alt=a["alt_baro"];
  if(alt.is<const char*>()){ f.onGround = strcmp(alt.as<const char*>(),"ground")==0; f.altFt=0; }
  else { f.altFt=(int)lround(alt.as<float>()); }
  f.gs    = (int)lround(a["gs"].as<float>());
  f.track = (int)lround(a["track"].as<float>());
  f.vsFpm = (int)lround(a["baro_rate"].as<float>());
  if(f.vsFpm==0) f.vsFpm=(int)lround(a["geom_rate"].as<float>());
  f.squawk = atoi((const char*)(a["squawk"]|"0"));
  f.dbFlags=a["dbFlags"]|0;
  String em=a["emergency"]|"none";
  f.emergency = (em!="none"&&em!="") || f.squawk==7500||f.squawk==7600||f.squawk==7700;
  f.distKm=haversineKm(cfg.homeLat,cfg.homeLon,f.lat,f.lon);
  f.icon=classify(f.cat,f.type,f.dbFlags,f.callsign,f.reg);
  f.typeName=aircraftFullName(f.type);
  f.valid=true;
  out=f;
  return true;
}

static bool looksLikeHex(const String& s){
  if(s.length()!=6) return false;
  for(size_t i=0;i<s.length();i++) if(!isxdigit(s[i])) return false;
  return true;
}

// remembered winning endpoint for the locked aircraft
static int    s_lockBase = -1;
static String s_lockPath = "";
void resetLockCache(){ s_lockBase=-1; s_lockPath=""; }

static bool fetchByIdent(const String& ident, Flight& out){
  if(ident.length()==0) return false;
  if(!heapOkForTls()){
    logf("lock: skipped %s (heap block %uB)", ident.c_str(), largestFreeBlock());
    return false;
  }
  String id = ident; id.trim(); id.toUpperCase();

  // Try the most specific endpoint first for the shape of the identifier.
  const char* paths[3];
  int np=0;
  if(looksLikeHex(ident)){ paths[np++]="hex"; paths[np++]="callsign"; paths[np++]="reg"; }
  else if(id.indexOf('-')>=0){ paths[np++]="reg"; paths[np++]="callsign"; }
  else { paths[np++]="callsign"; paths[np++]="reg"; paths[np++]="hex"; }

  struct { const char* name; const char* base; bool on; } bases[] = {
    {"adsb.fi",       "https://opendata.adsb.fi/api", cfg.sAdsbFi},
    {"adsb.one",      "https://api.adsb.one",         cfg.sAdsbOne},
  };

  const int NB = sizeof(bases)/sizeof(bases[0]);
  String target = looksLikeHex(ident)? ident : id;

  // Fast path: go straight back to whatever answered last time. A locked
  // aircraft then costs exactly one request per poll.
  if(s_lockBase>=0 && s_lockBase<NB && bases[s_lockBase].on && s_lockPath.length()){
    String url = String(bases[s_lockBase].base)+"/v2/"+s_lockPath+"/"+target;
    WiFiClientSecure c; c.setInsecure();
    HTTPClient http; http.setConnectTimeout(5000); http.setTimeout(7000);
    if(http.begin(c,url)){
      http.addHeader("User-Agent",kUserAgent);
      int code=http.GET();
      if(code==200){ bool got=parseOne(http,out); http.end(); if(got) return true; }
      else http.end();
    }
    resetLockCache();                    // stale - fall through to a full search
  }

  for(int bi=0; bi<NB; bi++){
    if(!bases[bi].on) continue;
    for(int i=0;i<np;i++){
      String url = String(bases[bi].base)+"/v2/"+paths[i]+"/"+target;
      WiFiClientSecure c; c.setInsecure();
      HTTPClient http; http.setConnectTimeout(5000); http.setTimeout(7000);
      if(!http.begin(c,url)) continue;
      http.addHeader("User-Agent",kUserAgent);
      int code=http.GET();
      if(code==200){
        bool got = parseOne(http,out);
        http.end();
        if(got){
          s_lockBase=bi; s_lockPath=paths[i];
          logf("lock: found %s via %s/%s", ident.c_str(), bases[bi].name, paths[i]);
          return true;
        }
      } else http.end();
      delay(1100);                       // 1 req/sec courtesy limit
    }
  }
  return false;
}

// Refresh only the locked aircraft. Called at the dwell rate so a followed
// flight updates far more often than the 60s area poll.
bool pollLocked(){
  if(!(cfg.lockOn && cfg.lockTarget.length())) return false;
  Flight lf;
  if(fetchByIdent(cfg.lockTarget,lf)){
    lf.enriched = s_locked.enriched && (s_locked.callsign==lf.callsign);
    if(lf.enriched){                     // carry the route details across
      lf.originIata=s_locked.originIata; lf.destIata=s_locked.destIata;
      lf.originCity=s_locked.originCity; lf.destCity=s_locked.destCity;
      lf.airline=s_locked.airline;       lf.csIata=s_locked.csIata;
    }
    s_locked=lf; s_lockedFound=true;
    if(!lf.onGround && lf.altFt>0){ s_lastAlt=lf.altFt; s_lastVs=lf.vsFpm; }
    return true;
  }
  s_lockedFound=false;
  return false;
}

// ---- filtering (returns reason when excluded) ------------------------------
static bool included(const Flight& f, String& reason){
  if(f.onGround)                                   { reason="on ground";      return false; }
  if(f.callsign.length()==0 && f.reg.length()==0)  { reason="no identity";    return false; }
  if(f.emergency && cfg.fEmergency)                { reason="";               return true;  }
  // Sanity check on distance. The "point search" query already asks each
  // source for aircraft within radiusKm, but that's the source's own
  // server-side filtering, which this firmware never re-verifies locally -
  // a stale/glitched position fix (common for MLAT-derived positions on
  // weak-signal aircraft) can slip through the source's own filter and show
  // up "hundreds of km away" even though it was reported as being nearby.
  // A generous tolerance (not a hard radiusKm cutoff) avoids excluding a
  // real aircraft that's only marginally outside the ring by the time this
  // runs. Emergencies are checked above and always bypass this, deliberately.
  double maxKm = cfg.radiusKm*1.15 + 3.0;
  if(f.distKm > maxKm)                             { reason="bad position?";  return false; }
  bool mil  = (f.dbFlags&1) || strcmp(f.icon,"military")==0;
  bool heli = strcmp(f.icon,"heli")==0;
  bool ga   = strcmp(f.icon,"light")==0 || strcmp(f.icon,"bizjet")==0;
  bool cargo= isCargoPrefix(f.callsign);
  if(mil)  { if(!cfg.fMilitary)  { reason="military off";  return false; } return true; }
  if(heli) { if(!cfg.fHeli)      { reason="heli off";      return false; } return true; }
  if(ga)   { if(!cfg.fPrivate)   { reason="GA off";        return false; } return true; }
  if(cargo){ if(!cfg.fCargo)     { reason="cargo off";     return false; } return true; }
  if(!cfg.fCommercial)           { reason="commercial off";return false; }
  return true;
}

// ---------------------------------------------------------------------------
// Master poll: query every enabled source, merge, classify, filter, sort,
// and build both the admin traffic snapshot and the display queue.
// ---------------------------------------------------------------------------
bool pollTraffic(){
  Source srcs[] = {
    {"adsb.fi",       "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%.0f",cfg.sAdsbFi},
    {"adsb.one",      "https://api.adsb.one/v2/point/%.4f/%.4f/%.0f",               cfg.sAdsbOne},
  };
  const int N = sizeof(srcs)/sizeof(srcs[0]);

  int lastEnabled=-1;
  for(int i=0;i<N;i++) if(srcs[i].enabled) lastEnabled=i;

  std::vector<Flight> all;
  String used="";
  for(int i=0;i<N;i++){
    if(!srcs[i].enabled) continue;
    int added=0;
    int rc = fetchSource(srcs[i],all,added);
    if(rc==0){
      delay(400);
      rc = fetchSource(srcs[i],all,added);
      if(rc!=1) logf("%s no data (retried)",srcs[i].name);
    }
    if(rc==1){
      used += (used.length()? "+" : "") + String(srcs[i].name);
      // v3.26: "deduped" used to also cover a source genuinely returning 0
      // aircraft (nothing to actually dedupe against on the first successful
      // source of a poll) - split so a quiet source at your location doesn't
      // read like it silently found traffic and threw it all away.
      if(added>0)          logf("%s +%d (total %d)", srcs[i].name, added, (int)all.size());
      else if(all.size()>0) logf("%s deduped (total %d)", srcs[i].name, (int)all.size());
      else                 logf("%s returned 0 aircraft", srcs[i].name);
      if(!cfg.mergeSources) break;
      if(i!=lastEnabled) delay(1100);
    }
  }
  s_source = used.length()? used : "none";
  s_count  = all.size();

  if(all.empty()){
    s_traffic.clear(); s_queue.clear(); s_qpos=-1;
    logf("poll: no aircraft returned");
    return false;
  }

  for(auto& f:all){
    f.icon     = classify(f.cat,f.type,f.dbFlags,f.callsign,f.reg);
    f.typeName = aircraftFullName(f.type);
  }

  // sort everything by distance for the admin snapshot
  std::vector<Flight*> sorted;
  for(auto& f:all) sorted.push_back(&f);
  std::sort(sorted.begin(),sorted.end(),
            [](Flight* a, Flight* b){ return a->distKm < b->distKm; });

  // ---- build the display queue from aircraft that pass the filters ----
  std::vector<Flight> q;
  for(auto* f:sorted){
    String why;
    if(included(*f,why) && q.size()<TRAFFIC_MAX) q.push_back(*f);
  }

  // order the rotation
  if(cfg.featured=="lowest"){
    std::sort(q.begin(),q.end(),[](const Flight& a, const Flight& b){
      int aa = a.altFt>0? a.altFt : 999999;
      int bb = b.altFt>0? b.altFt : 999999;
      return aa < bb;
    });
  }   // else already nearest-first

  // emergencies always lead the rotation
  std::stable_sort(q.begin(),q.end(),[](const Flight& a, const Flight& b){
    return a.emergency && !b.emergency;
  });

  s_queue = q;
  s_qpos  = -1;                       // selectNext() will step to the first
  s_lastPollAt = millis();            // fix time for the radar's dead reckoning
  // Wall-clock time of this poll, if NTP has synced (see configTime() in
  // main.cpp). Before it syncs, time(nullptr) reads back a small value
  // (seconds since boot, not since 1970) - treat anything obviously not a
  // real recent date as "not synced yet" rather than showing a bogus 1970.
  time_t nowT = time(nullptr);
  s_lastPollEpoch = (nowT > 1700000000) ? (uint32_t)nowT : 0;

  // Locked aircraft: look it up globally, so it is followed for the whole
  // flight regardless of the search radius. Prefer the local copy if it
  // happens to be nearby (saves a request), otherwise query directly.
  s_lockedFound = false;
  if(cfg.lockOn && cfg.lockTarget.length()){
    String want=upper(cfg.lockTarget);
    for(auto* f:sorted){
      if(upper(f->callsign)==want || upper(f->hex)==want){
        s_locked=*f; s_lockedFound=true; break;
      }
    }
    if(!s_lockedFound){
      Flight lf;
      if(fetchByIdent(cfg.lockTarget,lf)){ s_locked=lf; s_lockedFound=true; }
    }
    if(s_lockedFound)
      logf("lock %s: tracking, %.0fkm away", cfg.lockTarget.c_str(), s_locked.distKm);
    else
      logf("lock %s: no signal", cfg.lockTarget.c_str());
  }

  // ---- admin snapshot ----
  s_traffic.clear();
  for(size_t i=0;i<sorted.size() && s_traffic.size()<TRAFFIC_MAX;i++){
    Flight* f=sorted[i];
    TrafficRow r;
    r.hex=f->hex;
    // prefer the properly punctuated registration when the callsign is just the tail
    if(f->callsign.length() && f->reg.length()){
      String a=upper(f->callsign), b=upper(f->reg); b.replace("-","");
      r.callsign = (a==b)? f->reg : f->callsign;
    } else r.callsign = f->callsign.length()? f->callsign : (f->reg.length()? f->reg : f->hex);
    r.type=f->type; r.icon=f->icon;
    r.altFt=f->altFt; r.gs=f->gs; r.distKm=f->distKm;
    String why; r.included = included(*f,why); r.reason = why;
    r.featured = false;
    s_traffic.push_back(r);
  }

  logf("poll: %d seen, %d in rotation", (int)all.size(), (int)s_queue.size());
  // v3.33: the number that actually predicts a TLS crash is the largest free
  // *block*, not total free heap (see devlog.h) - logged every poll (60s) so
  // a fragmentation trend shows up here well before it causes a reset.
  logf("heap: %uB free, %uB largest block", (unsigned)ESP.getFreeHeap(), (unsigned)largestFreeBlock());
  return !s_queue.empty();
}

// ---------------------------------------------------------------------------
// Step to the next aircraft in the queue. Enriches lazily (once per aircraft
// per poll) so we only make a route lookup for aircraft actually shown.
// ---------------------------------------------------------------------------
bool selectNext(Flight& out){
  if(s_queue.empty() && !(cfg.lockOn && s_lockedFound)) return false;

  // Locked: return the locked aircraft, wherever it was found.
  if(cfg.lockOn && cfg.lockTarget.length()){
    if(!s_lockedFound) return false;            // caller shows the waiting screen
    if(!s_locked.enriched){ enrich(s_locked,true); s_locked.enriched=true; }
    for(auto& t:s_traffic) t.featured = (t.hex==s_locked.hex);
    out=s_locked;
    return true;
  }

  s_qpos++;
  if(s_qpos >= (int)s_queue.size()) s_qpos = 0;   // wrap around

  Flight& f = s_queue[s_qpos];
  if(!f.enriched){ enrich(f,false); f.enriched=true; }

  for(auto& t:s_traffic) t.featured = (t.hex==f.hex);

  logf("[%d/%d] %s (%s) %.1fkm", s_qpos+1, (int)s_queue.size(),
       f.callsign.c_str(), f.icon, f.distKm);
  out=f;
  return true;
}

// ---------------------------------------------------------------------------
// Radar snapshot: the display-rotation aircraft (already filtered to the
// same set the flight card shows), dead-reckoned forward from their last
// poll-time fix using track + ground speed. Real aircraft barely move on a
// radar this small over a few hundred milliseconds, so it's fine to call
// this on every redraw tick - it's just a handful of trig calls per aircraft.
// ---------------------------------------------------------------------------
// v3.35: see flight.h - swap-with-empty actually releases the vectors'
// heap-allocated backing storage, unlike clear() alone.
void freeForOta(){
  std::vector<Flight>().swap(s_queue);
  std::vector<TrafficRow>().swap(s_traffic);
  s_qpos = -1;
}

void radarSnapshot(std::vector<RadarBlip>& out){
  out.clear();
  uint32_t now = millis();
  double elapsedHr = s_lastPollAt? (now - s_lastPollAt)/3600000.0 : 0;

  for(const auto& f : s_queue){
    double lat=f.lat, lon=f.lon;
    if(f.gs>0 && elapsedHr>0){
      double d = f.gs * elapsedHr * 1.852;   // knots*hours -> nm -> km
      if(d>0.02){                            // skip the projection below ~20m
        double la2, lo2;
        project(lat,lon,(double)f.track,d,la2,lo2);
        lat=la2; lon=lo2;
      }
    }
    RadarBlip b;
    b.callsign   = f.callsign.length()? f.callsign : f.reg;
    b.distKm     = haversineKm(cfg.homeLat,cfg.homeLon,lat,lon);
    b.bearingDeg = bearingDeg(cfg.homeLat,cfg.homeLon,lat,lon);
    b.track=f.track; b.gs=f.gs; b.altFt=f.altFt;
    b.icon=f.icon; b.emergency=f.emergency;
    out.push_back(b);
  }
}
