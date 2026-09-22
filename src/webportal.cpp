#include "webportal.h"
#include "config.h"
#include "devlog.h"
#include "ota.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include "readme_content.h"

static AsyncWebServer server(80);
static DNSServer dns;
static bool apMode = false;
volatile bool g_wifiSubmitted = false;

static Flight s_cur;
static bool   s_have = false;
void portalSetCurrent(const Flight& f, bool haveFlight){ s_cur = f; s_have = haveFlight; }

// ---------------------------------------------------------------------------
// Captive setup page (AP mode)
// ---------------------------------------------------------------------------
static const char SETUP_HTML[] PROGMEM = R"HTML(
<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>Flight Eye setup</title>
<style>
/* v3.24: border-box everywhere - without it, an input/button's own padding
   and border add ON TOP of its 100% width instead of being absorbed by it,
   so it renders a few px wider than its container. Barely visible on a wide
   desktop window; on a narrow phone screen (this page's main audience,
   since it's what loads when you join the FlightEye-Setup Wi-Fi network)
   that overflow is proportionally bigger and the fields visibly don't line
   up with the edges of the page/labels above them. */
*{box-sizing:border-box}
body{background:#0e1217;color:#c6ccd4;font-family:system-ui;margin:0;padding:22px}
h1{font-size:19px}
label{display:block;margin:14px 0 5px;font-size:13px}
select,input{width:100%;padding:11px;border-radius:9px;border:1px solid #2c3742;background:#0c1116;color:#fff;font-size:15px}
button{margin-top:20px;width:100%;padding:13px;border:0;border-radius:9px;background:#e8a33d;color:#160f04;font-weight:700;font-size:15px}
.pwrap{position:relative}
.pwrap input{padding-right:64px}
#peek{position:absolute;right:5px;top:50%;transform:translateY(-50%);width:auto;margin:0;
      padding:6px 10px;font-size:12px;font-weight:600;border-radius:6px;
      background:#1b222a;color:#c6ccd4;border:1px solid #2c3742}
.m{font-family:monospace;color:#5f6b78;font-size:12px;margin-top:16px}
</style>
<h1>Flight Eye setup</h1>
<label>Your Wi-Fi network</label>
<select id=ssid></select>
<label>Password</label>
<div class=pwrap>
  <input id=pass type=password placeholder="Wi-Fi password" autocomplete=off autocapitalize=off autocorrect=off spellcheck=false>
  <button type=button id=peek onclick=togglePeek()>Show</button>
</div>
<button onclick=save()>Connect</button>
<p class=m id=msg>Loading networks...</p>
<script>
fetch('/scan').then(r=>r.json()).then(n=>{
  var s=document.getElementById('ssid');
  s.innerHTML = n.length ? n.map(function(x){return '<option>'+x+'</option>';}).join('')
                         : '<option>(none found)</option>';
  document.getElementById('msg').textContent = n.length+' networks found';
});
function togglePeek(){
  var p=document.getElementById('pass'), b=document.getElementById('peek');
  var show = p.type==='password';
  p.type = show ? 'text' : 'password';
  b.textContent = show ? 'Hide' : 'Show';
}
function save(){
  document.getElementById('msg').textContent='Saving and rebooting...';
  fetch('/save-wifi',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({ssid:document.getElementById('ssid').value,
                         pass:document.getElementById('pass').value})});
}
</script>
)HTML";

// ---------------------------------------------------------------------------
// Admin page (STA mode)
// ---------------------------------------------------------------------------
// v3.21: standalone "View README / changelog" page. Split into head/tail so
// the (large) escaped README body from readme_content.h can be sandwiched
// between them without a second copy of it living in this file too.
static const char README_PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Flight Eye - README</title>
<style>
body{background:#0e1217;color:#c6ccd4;font-family:system-ui;margin:0 auto;padding:18px;max-width:720px}
a{color:#e8a33d;text-decoration:none;font-family:monospace;font-size:13px}
pre{background:#151b22;border:1px solid #222b34;border-radius:12px;padding:16px;font-size:12px;line-height:1.6;color:#c6ccd4;white-space:pre-wrap;word-wrap:break-word;font-family:monospace}
</style></head><body>
<p><a href="/">&lt;- back to admin</a></p>
<pre>)HTML";
static const char README_PAGE_TAIL[] PROGMEM = "</pre></body></html>";

static const char ADMIN_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Flight Eye</title>
<style>
body{background:#0e1217;color:#c6ccd4;font-family:system-ui;margin:0 auto;padding:18px;max-width:640px}
h1{font-size:19px}
h2{font-size:12px;letter-spacing:.1em;text-transform:uppercase;color:#5f6b78;margin:22px 0 6px;font-family:monospace}
.card{background:#151b22;border:1px solid #222b34;border-radius:12px;padding:14px;margin-bottom:14px}
.r{display:flex;align-items:center;justify-content:space-between;padding:8px 0;border-bottom:1px solid #1b222a;gap:10px}
.r:last-child{border:0}
label{font-size:14px}
input[type=text],input[type=password],input[type=number],select{background:#0c1116;border:1px solid #2c3742;border-radius:8px;color:#fff;padding:8px;font-family:monospace;width:130px;text-align:right}
input[type=range]{width:150px}
#mapwrap{position:relative;margin-bottom:10px}
#map{height:300px;border-radius:10px;border:1px solid #2c3742}
#cross{position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);pointer-events:none;z-index:500}
#cross:before,#cross:after{content:"";position:absolute;background:#e8a33d;box-shadow:0 0 0 1px rgba(0,0,0,.5)}
#cross:before{left:-16px;top:-1px;width:32px;height:2px}
#cross:after{top:-16px;left:-1px;width:2px;height:32px}
#crossdot{position:absolute;left:-5px;top:-5px;width:10px;height:10px;border:2px solid #e8a33d;border-radius:50%;background:rgba(232,163,61,.25)}
.btnrow{display:flex;gap:8px;margin-bottom:8px}
.btnrow button{flex:1;padding:10px;border:0;border-radius:8px;font-family:monospace;font-size:12px;cursor:pointer}
#useCross{background:#22190c;color:#e8a33d}
#useLoc2{background:#161c23;color:#c6ccd4;border:1px solid #2c3742}
.leaflet-container{background:#0c1116}
.pin{width:16px;height:16px;border-radius:50%;background:#e8a33d;border:3px solid #160f04;box-shadow:0 0 0 2px #e8a33d}
.st{font-family:monospace;font-size:13px;color:#9aa0a8;line-height:1.9}
.st b{color:#fff}
.cs{color:#FFF200}
.rt{color:#0ff}
.dim{color:#5f6b78}
.log{background:#0a0e12;border:1px solid #222b34;border-radius:8px;padding:10px;font-size:11px;line-height:1.5;color:#8fa6b8;max-height:220px;overflow:auto;white-space:pre-wrap;margin:0;font-family:monospace}
.free{color:#37d16b;font-family:monospace;font-size:11px}
table{width:100%;border-collapse:collapse;font-family:monospace;font-size:12px}
th{text-align:left;color:#5f6b78;font-weight:600;font-size:10px;letter-spacing:.06em;text-transform:uppercase;padding:0 6px 8px;border-bottom:1px solid #222b34}
td{padding:7px 6px;border-bottom:1px solid #161c22;color:#c6ccd4}
tr.out td{color:#4d5866}
tr.feat td{background:#1c1608}
tr.feat td:first-child{box-shadow:inset 3px 0 0 #e8a33d}
tr.row{cursor:pointer}
tr.row:active td{background:#1b232c}
.why{color:#5f6b78;font-size:10px}
.danger{width:100%;padding:11px;border:1px solid #3d211c;border-radius:9px;background:#22110f;color:#e5624a;font-family:monospace;font-size:13px;margin-top:8px}
#useLoc{width:100%;padding:10px;border:0;border-radius:8px;background:#22190c;color:#e8a33d;font-family:monospace;margin-bottom:8px}
#save{position:sticky;bottom:0;width:100%;padding:13px;border:0;border-radius:9px;background:#e8a33d;color:#160f04;font-weight:700;font-size:15px;margin-top:8px}
</style></head><body>

<h1>Flight Eye</h1>

<div class=card><h2>Status</h2><div class=st id=status>loading...</div></div>

<div class=card><h2>Live device screen</h2>
  <canvas id=screenCanvas width=320 height=240 style="width:100%;max-width:420px;display:block;margin:0 auto;border-radius:8px;border:1px solid #2c3742;background:#000"></canvas>
  <div class="st dim" style="font-size:11px;margin-top:8px">A live read of what's on the device's screen right now - same colours and layout, updated alongside Status above. Fonts are a close approximation, not pixel-identical (the device draws its own bitmap fonts).</div>
</div>

<div class=card><h2>Live radar</h2>
  <canvas id=radarCanvas width=260 height=260 style="width:100%;max-width:320px;display:block;margin:0 auto;border-radius:8px;border:1px solid #2c3742;background:#000"></canvas>
  <div class="st dim" style="font-size:11px;margin-top:8px">Mirrors the device's radar page (tap the display 3 times from the flight card to see it there) - same aircraft, filters and range, refreshed every 3s.</div>
</div>

<div class=card><h2>Tracking centre</h2>
  <div id=mapwrap><div id=map></div><div id=cross><div id=crossdot></div></div></div>
  <div class=btnrow>
    <button id=useCross onclick=useCross()>Use crosshairs</button>
    <button id=useLoc2 onclick=useLoc()>Use my location</button>
  </div>
  <div class="st dim" style="font-size:11px;margin-bottom:8px">Pan the map to anywhere in the world, then tap "Use crosshairs" to track flights over that spot.</div>
  <div id=geonote class=st style="font-size:11px;margin-bottom:8px;display:none;color:#e8a33d"></div>
  <div class=r><label>Centre latitude</label><input id=homeLat type=text></div>
  <div class=r><label>Centre longitude</label><input id=homeLon type=text></div>
  <div class=r><label>Radius km</label><input id=radiusKm type=range min=5 max=80><span id=radv class=st></span></div>
  <div class=r><label>Rotation order</label><select id=featured>
    <option value=nearest>Closest first</option><option value=lowest>Lowest first</option></select></div>
  <div class=r><label>Poll sky every</label><input id=pollSec type=range min=30 max=120 step=10><span id=pollv class=st></span></div>
  <div class=r><label>Seconds per aircraft</label><input id=dwellSec type=number min=3 max=60></div>
  <div class="st dim" style="font-size:11px;margin-top:6px">The device fetches all traffic once per poll, then rotates through the list on screen. This radius is also the outer ring on the device's radar screen (tap the display three times from the flight card).</div>
</div>

<div class=card><h2>Display</h2>
  <div class=r><label>Brightness percent</label><input id=brightness type=number></div>
  <div class=r><label>Orientation</label><select id=rotation>
    <option value=1>Landscape</option><option value=3>Landscape flipped</option>
    <option value=0>Portrait</option><option value=2>Portrait flipped</option></select></div>
  <div class=r><label>Imperial units</label><input id=imperial type=checkbox></div>
  <div class=r><label>IATA callsign (UA263)</label><input id=callsignIata type=checkbox></div>
  <div class="st dim" style="font-size:11px;margin-top:6px">Touches within 50px of any edge of the screen are always ignored, to stop a snug case from triggering false taps. This is fixed in firmware, not adjustable here.</div>
</div>

<div class=card><h2>Aircraft filter</h2>
  <div class=r><label>Commercial</label><input id=fCommercial type=checkbox></div>
  <div class=r><label>Cargo</label><input id=fCargo type=checkbox></div>
  <div class=r><label>Private / GA</label><input id=fPrivate type=checkbox></div>
  <div class=r><label>Military</label><input id=fMilitary type=checkbox></div>
  <div class=r><label>Helicopters</label><input id=fHeli type=checkbox></div>
  <div class=r><label>Emergency</label><input id=fEmergency type=checkbox></div>
  <div class=r><label>Jump to emergencies</label><input id=fJumpEmerg type=checkbox></div>
</div>

<div class=card><h2>Data sources <span class=free>all free, no keys</span></h2>
  <div class=r><label>Merge all sources</label><input id=mergeSources type=checkbox></div>
  <div class=r><label>adsb.fi</label><input id=sAdsbFi type=checkbox></div>
  <div class=r><label>adsb.one</label><input id=sAdsbOne type=checkbox></div>
  <div class="st dim" style="font-size:11px;margin-top:8px">Routes and airline names via adsbdb. Sources rate limited to 1 request per second, personal non-commercial use.</div>
</div>

<div class=card><h2>LED</h2>
  <div class=r><label>Mode</label><select id=ledMode>
    <option value=class>Aircraft class</option><option value=proximity>Proximity pulse</option>
    <option value=density>Traffic density</option><option value=status>Status only</option>
    <option value=off>Off</option></select></div>
  <div class=r><label>Dim overnight with screen</label><input id=ledNightOff type=checkbox></div>
  <div class="st dim" style="font-size:11px;margin-top:6px">Emergency squawks always flash red, whatever the mode.</div>
</div>

<div class=card><h2>Network</h2>
  <div class=r><label>Use a fixed IP</label><input id=useStaticIp type=checkbox></div>
  <div class=r><label>IP address</label><input id=staticIp type=text placeholder="192.168.1.85"></div>
  <div class=r><label>Router / gateway</label><input id=staticGw type=text placeholder="192.168.1.1"></div>
  <div class=r><label>Subnet mask</label><input id=staticMask type=text></div>
  <div class="st dim" style="font-size:11px;margin-top:6px">A fixed IP means the admin address never changes. Tap the device screen any time to see its address and a QR code that opens this page. If the gateway you enter isn't on the same subnet as the IP, the device ignores this and falls back to DHCP automatically (check the Device log below if that happens).</div>
</div>

<div class=card><h2>Lock to a flight</h2>
  <div class=r><label>Lock display</label><input id=lockOn type=checkbox></div>
  <div class=r><label>Callsign or hex</label><input id=lockTarget type=text></div>
  <button class=danger style="color:#c6ccd4;border-color:#2c3742;background:#0c1116" onclick=unlock()>Clear lock and resume rotation</button>
  <div class="st dim" style="font-size:11px;margin-top:8px">Tapping a row in the traffic list locks immediately - no need to press Save. A locked aircraft is followed <b>worldwide</b> for its whole flight, not just inside your search radius. It stays locked until you clear it.</div>
</div>

<div class=card><h2>Traffic in range <span class=free id=trafcount></span></h2>
  <table><thead><tr><th>Callsign</th><th>Type</th><th>Alt</th><th>Spd</th><th>Dist</th></tr></thead>
  <tbody id=traffic><tr><td colspan=5>loading...</td></tr></tbody></table>
  <div class="st dim" style="font-size:11px;margin-top:8px">Nearest 25. Highlighted row is on the display; greyed rows are excluded by your filters. Tap a row to lock onto it.</div>
</div>

<div class=card><h2>Device log</h2><pre id=log class=log>loading...</pre></div>

<div class=card><h2>About</h2>
  <div class="st dim" style="margin-bottom:8px">Flight Eye v3.34</div>
  <button class=danger style="color:#c6ccd4;border-color:#2c3742;background:#0c1116" onclick="window.open('/readme','_blank')">View README / changelog</button>
</div>

<div class=card><h2>Reset</h2>
  <button class=danger style="color:#c6ccd4;border-color:#2c3742;background:#0c1116" onclick=calibrate()>Calibrate touchscreen</button>
  <button class=danger onclick=forgetWifi()>Forget Wi-Fi and restart setup</button>
  <button class=danger onclick=factoryReset()>Full factory reset</button>
  <div class="st dim" style="font-size:11px;margin-top:10px">You can also hold the BOOT button on the board: 5 seconds resets Wi-Fi, 10 seconds wipes everything.</div>
</div>

<button id=save onclick=save()>Save changes</button>

<script>
var NUM = ["radiusKm","refreshSec","brightness","rotation","pollSec","dwellSec"];
var FLOATS = ["homeLat","homeLon"];
var F = ["homeLat","homeLon","radiusKm","featured","pollSec","dwellSec","brightness","rotation",
         "imperial","callsignIata","fCommercial","fCargo","fPrivate","fMilitary","fHeli",
         "fEmergency","fJumpEmerg","sAdsbFi","sAdsbOne",
         "mergeSources","lockOn","lockTarget","ledMode","ledNightOff",
         "useStaticIp","staticIp","staticGw","staticMask"];

var map, marker, circle, box;
function el(id){ return document.getElementById(id); }
function coords(){ return [parseFloat(el('homeLat').value)||51.47, parseFloat(el('homeLon').value)||-0.45]; }

function initMap(){
  try{
    var c = coords();
    map = L.map('map',{attributionControl:false}).setView(c,9);
    L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:18}).addTo(map);
    var ic = L.divIcon({className:'',html:'<div class=pin></div>',iconSize:[16,16],iconAnchor:[8,8]});
    marker = L.marker(c,{draggable:true,icon:ic}).addTo(map);
    circle = L.circle(c,{radius:el('radiusKm').value*1000,color:'#e8a33d',weight:1,fillOpacity:0.12}).addTo(map);
    box = L.rectangle(circle.getBounds(),{color:'#39c2d7',weight:1,dashArray:'5 4',fill:false}).addTo(map);
    fitMap();
    setTimeout(function(){ map.invalidateSize(); },300);
    marker.on('drag', function(){
      var p = marker.getLatLng();
      el('homeLat').value = p.lat.toFixed(4);
      el('homeLon').value = p.lng.toFixed(4);
      redraw(false);
    });
    marker.on('dragend', fitMap);
    ['homeLat','homeLon'].forEach(function(k){
      el(k).addEventListener('change', function(){ map.setView(coords()); redraw(true); });
    });
    el('radiusKm').addEventListener('input', function(){
      el('radv').textContent = el('radiusKm').value+' km';
      redraw(true);
    });
  }catch(e){}
}
function redraw(refit){
  if(!map) return;
  var c = coords();
  marker.setLatLng(c);
  circle.setLatLng(c).setRadius(el('radiusKm').value*1000);
  box.setBounds(circle.getBounds());
  if(refit) fitMap();
}
function fitMap(){ map.fitBounds(box.getBounds().pad(0.25)); }

function useCross(){
  if(!map) return;
  var c = map.getCenter();
  el('homeLat').value = c.lat.toFixed(4);
  el('homeLon').value = c.lng.toFixed(4);
  redraw(true);
  var b = el('useCross'); b.textContent = 'Centre set';
  setTimeout(function(){ b.textContent = 'Use crosshairs'; },1500);
}
function useLoc(auto_){
  if(!navigator.geolocation) return;
  navigator.geolocation.getCurrentPosition(function(p){
    el('homeLat').value = p.coords.latitude.toFixed(4);
    el('homeLon').value = p.coords.longitude.toFixed(4);
    if(map){ map.setView(coords(),11); redraw(true); }
    if(auto_){
      var n = el('geonote');
      if(n){ n.textContent = 'No tracking centre saved yet - used your phone\'s current location as a starting point. Tap "Save changes" below to keep it, or set your own with the map first.'; n.style.display='block'; }
    }
  }, function(){ /* denied/unavailable - fine, the existing default stands */ });
}

function pad3(n){ n = String(n); while(n.length<3){ n = '0'+n; } return n; }
function upTime(s){ return Math.floor(s/3600)+'h '+(Math.floor(s/60)%60)+'m'; }

function status(){
  fetch('/api/status').then(function(r){ return r.json(); }).then(function(s){
    var h = 'host <b>'+s.host+'</b> / <b>'+s.ip+'</b> / '+s.rssi+' dBm / up '+upTime(s.uptime)
          + ' / heap '+Math.round(s.heap/1024)+'KB (block '+Math.round(s.heapBlock/1024)+'KB)<br>'
          + 'gw <b>'+s.gw+'</b> / mask <b>'+s.mask+'</b>'
          + ' <span class=dim>('+(s.fixedIp?'fixed':'DHCP')+')</span><br>'
          + 'last poll <b>'+(s.lastPollEpoch? new Date(s.lastPollEpoch*1000).toLocaleString() : 'not yet synced')+'</b><br>'
          + 'source <b>'+s.source+'</b> / <b>'+s.count+'</b> in range'
          + (s.locked ? ' / <span style="color:#e8a33d">LOCKED</span>' : '')
          + '<hr style="border:0;border-top:1px solid #222b34;margin:8px 0">';
    if(s.have){
      var f = s.flight;
      h += '<span class=cs style="font-size:18px">'+(f.callsign||f.reg)+'</span>'
        +  (f.iata ? ' <span class=dim>('+f.iata+')</span>' : '') + '<br>'
        +  (f.airline || '<span class=dim>unknown operator</span>') + '<br>'
        +  '<span class=rt>'+(f.from||'?')+' to '+(f.to||'?')+'</span> '
        +  (f.fromCity ? '<span class=dim>'+f.fromCity+' to '+f.toCity+'</span>' : '') + '<br>'
        +  'ALT <b>'+(f.alt ? f.alt+'ft' : 'GND')+'</b> / SPD <b>'+f.gs+'kt</b> / HDG <b>'+pad3(f.track)+'</b>'
        +  ' / VS <b>'+(f.vs>0?'+':'')+f.vs+'fpm</b><br>'
        +  (f.type||'----')+' / '+(f.reg||'----')+' / '+f.icon+' / hex '+f.hex
        +  ' / squawk '+(f.squawk||'----')+' / '+f.distKm.toFixed(1)+'km away'
        +  (f.emergency ? '<br><b style="color:#e5624a">EMERGENCY</b>' : '');
    } else {
      h += '<i class=dim>no flight being shown</i>';
    }
    el('status').innerHTML = h;
    renderScreen(s);   // keep the device-screen mirror on the same poll, no extra requests
  });
}

// ---------------------------------------------------------------------------
// Live device-screen mirror. Approximates drawFlightCard() in display.cpp:
// same layout, same colours (converted from the panel's RGB565 palette),
// close-enough fonts (a browser canvas can't reproduce the device's bitmap
// fonts exactly). Piggybacks on the existing 4s status() poll, so this adds
// zero extra load on the device.
// ---------------------------------------------------------------------------
function roundRect(ctx,x,y,w,h,r){
  ctx.beginPath();
  ctx.moveTo(x+r,y);
  ctx.arcTo(x+w,y,x+w,y+h,r); ctx.arcTo(x+w,y+h,x,y+h,r);
  ctx.arcTo(x,y+h,x,y,r);     ctx.arcTo(x,y,x+w,y,r);
  ctx.closePath();
}
function renderScreen(s){
  var cv = el('screenCanvas'); if(!cv) return;
  var ctx = cv.getContext('2d');
  var W=320, H=240;
  ctx.fillStyle='#000'; ctx.fillRect(0,0,W,H);
  ctx.textAlign='left'; ctx.textBaseline='top';

  if(!s.have){
    ctx.fillStyle='#7B7D7B'; ctx.font='16px sans-serif'; ctx.textAlign='center';
    ctx.fillText('No aircraft in range', W/2, H/2-16);
    ctx.fillText('via '+s.source, W/2, H/2+6);
    ctx.textAlign='left';
    return;
  }
  var f = s.flight;

  // callsign + lock icon (only if locked, matching the device now)
  ctx.fillStyle='#FFFF00'; ctx.font='bold 26px sans-serif';
  var cs = f.callsign || f.reg || ('HEX '+f.hex);
  ctx.fillText(cs, 12, 4);
  var csW = ctx.measureText(cs).width;
  if(s.locked){
    var lx=12+csW+14, ly=8;
    ctx.fillStyle='#FFB600'; roundRect(ctx,lx,ly,28,22,5); ctx.fill();
    ctx.fillStyle='#000'; ctx.fillRect(lx+9,ly+11,10,7);
    ctx.beginPath(); ctx.arc(lx+14,ly+11,4,0,2*Math.PI); ctx.fill();
  }
  ctx.beginPath(); ctx.arc(W-14,17,4,0,2*Math.PI); ctx.fillStyle='#00FF00'; ctx.fill();

  // operator
  var ga = (f.icon=='light'||f.icon=='bizjet'), hl=(f.icon=='heli'), ml=(f.icon=='military');
  var opLine = f.airline || (ga?'Private / GA':hl?'Rotary':ml?'Military':'');
  if(opLine){ ctx.fillStyle='#ADAAAD'; ctx.font='12px monospace'; ctx.fillText(opLine,14,48); }

  // route
  var haveRoute = f.from || f.to;
  ctx.fillStyle='#00FFFF'; ctx.font='16px monospace';
  ctx.fillText(haveRoute? ((f.from||'?')+' -> '+(f.to||'?')) : (ga||hl?'no route filed':'- en route -'), 14, 66);
  if(f.fromCity||f.toCity){
    ctx.fillStyle='#838183'; ctx.font='12px monospace';
    ctx.fillText((f.fromCity||'?')+' -> '+(f.toCity||'?'), 14, 94);
  }

  // ALT / SPD / HDG columns
  var spd = s.imperial? f.gs+'kt' : Math.round(f.gs*1.852)+'kmh';
  var cols=[[62,'ALT', f.alt? f.alt+'ft':'GND'],[164,'SPD',spd],[266,'HDG',pad3(f.track)]];
  ctx.textAlign='center';
  cols.forEach(function(c){
    ctx.fillStyle='#7B7D7B'; ctx.font='12px monospace'; ctx.fillText(c[1],c[0],124);
    ctx.fillStyle='#fff';    ctx.font='bold 18px monospace'; ctx.fillText(c[2],c[0],144);
  });

  // vertical rate (left) + distance (right) + queue position (centre)
  ctx.textAlign='left';
  var vsCol = f.vs>50?'#00FF00':f.vs<-50?'#FFB600':'#7B7D7B';
  ctx.fillStyle=vsCol; ctx.font='12px monospace';
  ctx.fillText(Math.abs(f.vs)<50?'level':(Math.abs(f.vs)+' fpm'), 38, 180);
  ctx.textAlign='right';
  var distDisp = s.imperial? f.distKm*0.621371 : f.distKm;
  var distUnit = s.imperial? 'mi' : 'km';
  ctx.fillStyle='#00FFFF';
  ctx.fillText((distDisp<100? distDisp.toFixed(1) : Math.round(distDisp))+' '+distUnit+' away', W-14, 180);
  if(!s.locked && s.qtotal>1){
    ctx.textAlign='center'; ctx.fillStyle='#528A9E';
    ctx.fillText(s.qpos+'/'+s.qtotal, W/2, 180);
  }

  // footer bar: type/name + reg, LOCKED banner if locked
  ctx.fillStyle='#0c1420'; ctx.fillRect(0,206,W,28);
  ctx.textAlign='left'; ctx.fillStyle='#fff'; ctx.font='12px monospace';
  var name = f.type || '----';
  ctx.fillText(name, 30, 214);
  if(f.reg){
    var nameW = ctx.measureText(name).width;
    ctx.fillText(f.reg, 30+nameW+18, 214);
  }
  if(s.locked){
    ctx.textAlign='right'; ctx.fillStyle='#FFB600';
    ctx.fillText('LOCKED', W-12, 214);
    ctx.strokeStyle='#FFB600'; ctx.lineWidth=2; ctx.strokeRect(1,1,W-2,H-2);
  }
  ctx.textAlign='left';
}

// ---------------------------------------------------------------------------
// Live radar mirror. Same polar-plot math as radarXY()/radarGeom() in
// display.cpp (distance/range -> pixel radius, bearing -> angle, N-up).
// Polled separately from Status/traffic on its own 3s timer - light enough
// (a handful of small numbers per aircraft) not to be worth coupling to
// anything else, and slow enough not to add meaningful load to the device.
// ---------------------------------------------------------------------------
function blipColour(b){
  if(b.emergency || b.icon=='military') return '#FF3B30';
  if(b.icon=='heli') return '#FF00FF';
  if(b.icon=='light' || b.icon=='bizjet') return '#00FF00';
  if(b.icon=='turboprop') return '#00FFFF';
  return '#3366FF';                 // airliner / cargo
}
function renderRadar(d){
  var cv = el('radarCanvas'); if(!cv) return;
  var ctx = cv.getContext('2d');
  var W=260, H=260;
  var cx=W/2, cy=H/2+8, rPix=Math.min(W,H)/2-26;
  ctx.fillStyle='#000'; ctx.fillRect(0,0,W,H);

  ctx.textAlign='left'; ctx.textBaseline='top';
  ctx.fillStyle='#FFFF00'; ctx.font='bold 15px sans-serif'; ctx.fillText('Radar',12,6);
  ctx.textAlign='right'; ctx.fillStyle='#7B7D7B'; ctx.font='11px monospace';
  ctx.fillText(String(d.blips.length), W-12, 10);

  ctx.strokeStyle='#1b2e45';
  for(var i=1;i<=3;i++){ ctx.beginPath(); ctx.arc(cx,cy,rPix*i/3,0,2*Math.PI); ctx.stroke(); }

  ctx.textAlign='center'; ctx.textBaseline='middle'; ctx.fillStyle='#528A9E'; ctx.font='10px monospace';
  ctx.fillText('N',cx,cy-rPix-9); ctx.fillText('S',cx,cy+rPix+9);
  ctx.fillText('E',cx+rPix+10,cy); ctx.fillText('W',cx-rPix-10,cy);

  var disp = d.imperial? d.rangeKm*0.621371 : d.rangeKm;
  ctx.fillStyle='#3A4552';
  ctx.fillText(Math.round(disp)+(d.imperial?'mi':'km'), cx, cy-rPix+9);

  ctx.fillStyle='#fff'; ctx.beginPath(); ctx.arc(cx,cy,3,0,2*Math.PI); ctx.fill();

  d.blips.forEach(function(b){
    var r = d.rangeKm>0 ? (b.distKm/d.rangeKm)*rPix : 0;
    if(r>rPix) r=rPix; if(r<0) r=0;
    var rad = b.bearingDeg*Math.PI/180;
    var sx = cx + r*Math.sin(rad), sy = cy - r*Math.cos(rad);
    var c = blipColour(b);
    ctx.fillStyle=c; ctx.beginPath(); ctx.arc(sx,sy,3,0,2*Math.PI); ctx.fill();
    var trad = b.track*Math.PI/180;
    ctx.strokeStyle=c; ctx.beginPath(); ctx.moveTo(sx,sy);
    ctx.lineTo(sx+7*Math.sin(trad), sy-7*Math.cos(trad)); ctx.stroke();

    if(b.callsign){
      var nearRight = sx > W-46;
      ctx.textAlign = nearRight? 'right' : 'left';
      var tx = nearRight? sx-6 : sx+6;
      var alt = b.altFt>=18000 ? ('FL'+pad3(Math.round(b.altFt/100)))
              : (d.imperial? b.altFt+'ft' : Math.round(b.altFt*0.3048)+'m');
      ctx.fillStyle='#fff';    ctx.font='9px monospace'; ctx.textBaseline='bottom'; ctx.fillText(b.callsign,tx,sy-2);
      ctx.fillStyle='#838183'; ctx.textBaseline='top';    ctx.fillText(alt,tx,sy+2);
    }
  });
  ctx.textAlign='left'; ctx.textBaseline='alphabetic';
}
function loadRadar(){
  fetch('/api/radar').then(function(r){ return r.json(); }).then(renderRadar);
}

function icon(k){
  if(k=='heli') return 'H';
  if(k=='military') return 'M';
  if(k=='bizjet') return 'J';
  if(k=='light') return 'L';
  if(k=='turboprop') return 'T';
  return 'A';
}
function loadTraffic(){
  fetch('/api/traffic').then(function(r){ return r.json(); }).then(function(list){
    var t = el('traffic');
    el('trafcount').textContent = list.length ? list.length+' shown' : '';
    if(!list.length){ t.innerHTML = '<tr><td colspan=5>nothing in range</td></tr>'; return; }
    t.innerHTML = list.map(function(a){
      var cls = 'row' + (a.featured ? ' feat' : '') + (a.included ? '' : ' out');
      var why = (!a.included && a.reason) ? ' <span class=why>'+a.reason+'</span>' : '';
      return '<tr class="'+cls+'" onclick="lockTo(\''+a.callsign+'\')">'
           + '<td>'+icon(a.icon)+' '+a.callsign+why+'</td>'
           + '<td>'+(a.type||'--')+'</td>'
           + '<td>'+(a.alt ? a.alt+'ft' : 'GND')+'</td>'
           + '<td>'+a.gs+'kt</td>'
           + '<td>'+a.distKm.toFixed(1)+'km</td></tr>';
    }).join('');
  });
}
function lockTo(cs){
  if(!confirm('Lock the display to '+cs+'?')) return;
  el('lockTarget').value = cs;
  el('lockOn').checked = true;
  var o = {lockOn:true, lockTarget:cs};
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify(o)}).then(function(){
      var b = el('save');
      b.textContent = 'Locked to '+cs;
      setTimeout(function(){ b.textContent = 'Save changes'; },2000);
      status(); loadTraffic();
    });
}
function unlock(){
  el('lockOn').checked = false;
  el('lockTarget').value = '';
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({lockOn:false,lockTarget:''})}).then(function(){
      status(); loadTraffic();
    });
}
function calibrate(){
  if(!confirm('Start touch calibration on the device? Tap the two markers shown on its screen.')) return;
  fetch('/api/calibrate',{method:'POST'});
  alert('Look at the device and tap the markers.');
}
function forgetWifi(){
  if(!confirm('Clear the saved Wi-Fi and restart into setup mode?')) return;
  fetch('/api/forget-wifi',{method:'POST'});
  alert('Resetting. Reconnect to the FlightEye-Setup hotspot.');
}
function factoryReset(){
  if(!confirm('Wipe ALL settings and start completely fresh?')) return;
  if(!confirm('This cannot be undone. Continue?')) return;
  fetch('/api/factory-reset',{method:'POST'});
  alert('Factory resetting. Reconnect to the FlightEye-Setup hotspot.');
}
function loadLog(){
  fetch('/api/log').then(function(r){ return r.json(); }).then(function(lines){
    var e = el('log');
    e.textContent = lines.join('\n');
    e.scrollTop = e.scrollHeight;
  });
}

// v3.10: Leaflet + the OSM tiles are fetched from the internet, not the
// device. Loading them as blocking <head> tags used to stall the ENTIRE page
// - including status/log/traffic/config, none of which need internet - on
// any network that can't reach the CDN quickly (a common failure on phones:
// ad-block DNS, MDM/content filters, a guest or IoT VLAN with no WAN route).
// So Leaflet is now fetched lazily in the background and the map is the only
// thing that degrades if it can't load; everything else above never waits on it.
var leafletReady=false, cfgReady=false;
function tryInitMap(){ if(leafletReady && cfgReady) initMap(); }

function mapUnavailable(){
  var w = el('mapwrap');
  if(w) w.innerHTML = '<div class="st dim" style="padding:36px 10px;text-align:center">'
    + 'Map needs internet access and could not load from this device.<br>'
    + 'You can still set the centre point using the fields below.</div>';
}

(function loadLeaflet(){
  var link = document.createElement('link');
  link.rel = 'stylesheet';
  link.href = 'https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.css';
  document.head.appendChild(link);

  var s = document.createElement('script');
  s.src = 'https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.js';
  var done = false;
  s.onload = function(){ done = true; leafletReady = true; tryInitMap(); };
  s.onerror = function(){ if(!done){ done = true; mapUnavailable(); } };
  document.head.appendChild(s);
  setTimeout(function(){ if(!done){ done = true; mapUnavailable(); } }, 8000);
})();

function load(){
  fetch('/api/config').then(function(r){ return r.json(); }).then(function(c){
    F.forEach(function(k){
      var e = el(k);
      if(!e) return;
      if(e.type === 'checkbox'){ e.checked = !!c[k]; } else { e.value = c[k]; }
    });
    el('radv').textContent = el('radiusKm').value+' km';
    el('pollv').textContent = el('pollSec').value+' s';
    el('pollSec').addEventListener('input', function(){
      el('pollv').textContent = el('pollSec').value+' s';
    });
    cfgReady = true;
    tryInitMap();
    // v3.25: nothing saved here yet on this device - use the phone's location
    // as a starting point instead of the hardcoded factory default. Fires only
    // while homeSet is still false (i.e. until the first real Save), so it
    // never silently moves an already-configured tracking centre.
    if(!c.homeSet) useLoc(true);
  });
}

function save(){
  var o = {};
  F.forEach(function(k){
    var e = el(k);
    if(!e) return;
    if(e.type === 'checkbox'){ o[k] = e.checked; }
    else if(NUM.indexOf(k) >= 0 || FLOATS.indexOf(k) >= 0){ o[k] = parseFloat(e.value); }
    else { o[k] = e.value; }
  });
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify(o)}).then(function(){
      var b = el('save');
      b.textContent = 'Saved';
      setTimeout(function(){ b.textContent = 'Save changes'; },1200);
      var n = el('geonote'); if(n) n.style.display = 'none';
    });
}

load(); status(); loadLog(); loadTraffic(); loadRadar();
setInterval(status,4000);
setInterval(loadLog,4000);
setInterval(loadTraffic,4000);
setInterval(loadRadar,3000);
</script></body></html>
)HTML";

// ---------------------------------------------------------------------------
// API handlers
// ---------------------------------------------------------------------------
static void handleStatus(AsyncWebServerRequest* r){
  JsonDocument d;
  d["ip"]     = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  d["host"]   = "flighteye.local";
  d["rssi"]   = WiFi.RSSI();
  // v3.11: live gateway/subnet, so a phone that can't reach this page can be
  // compared against its own Wi-Fi details to spot a network/VLAN mismatch.
  d["gw"]     = WiFi.gatewayIP().toString();
  d["mask"]   = WiFi.subnetMask().toString();
  d["fixedIp"]= cfg.useStaticIp;
  // v3.14: UTC seconds of the last poll (0 = none yet / NTP not synced) -
  // the browser renders this in the viewer's own local time and timezone.
  d["lastPollEpoch"] = lastPollEpoch();
  d["source"] = activeSource();
  d["count"]  = aircraftInRange();
  d["uptime"] = (uint32_t)(millis()/1000);
  d["heap"]   = (uint32_t)ESP.getFreeHeap();
  // v3.33: total free heap can look healthy while still being too fragmented
  // for a TLS handshake's one big allocation to succeed - this is the
  // number that actually predicts that (see devlog.h).
  d["heapBlock"]  = largestFreeBlock();
  d["fwVersion"]  = FW_VERSION;
  d["otaState"]   = otaStateString();
  d["otaChecked"] = otaLastCheckedAgo();
  d["locked"] = cfg.lockOn;
  d["qpos"]   = queuePosition();
  d["qtotal"] = queueCount();
  d["lockTarget"] = cfg.lockTarget;
  d["lockInRange"] = lockedInRange();
  d["have"]   = s_have;
  if(s_have){                       // full mirror of what the CYD is showing
    JsonObject f = d["flight"].to<JsonObject>();
    f["callsign"] = s_cur.callsign;   f["iata"]     = s_cur.csIata;
    f["airline"]  = s_cur.airline;    f["hex"]      = s_cur.hex;
    f["type"]     = s_cur.type;       f["reg"]      = s_cur.reg;
    f["icon"]     = s_cur.icon;       f["cat"]      = s_cur.cat;
    f["from"]     = s_cur.originIata; f["to"]       = s_cur.destIata;
    f["fromCity"] = s_cur.originCity; f["toCity"]   = s_cur.destCity;
    f["alt"]      = s_cur.altFt;      f["gs"]       = s_cur.gs;
    f["track"]    = s_cur.track;      f["vs"]       = s_cur.vsFpm;
    f["squawk"]   = s_cur.squawk;     f["distKm"]   = s_cur.distKm;
    f["emergency"]= s_cur.emergency;
  }
  String out; serializeJson(d,out);
  r->send(200,"application/json",out);
}

// Admin pages must never be cached: a stale copy makes the device look broken.
static void sendNoCache(AsyncWebServerRequest* r, int code,
                        const char* type, const String& body){
  AsyncWebServerResponse* res = r->beginResponse(code,type,body);
  res->addHeader("Cache-Control","no-store, no-cache, must-revalidate, max-age=0");
  res->addHeader("Pragma","no-cache");
  res->addHeader("Expires","0");
  r->send(res);
}

static void handleGetConfig(AsyncWebServerRequest* r){
  JsonDocument d; cfg.toJson(d);
  String out; serializeJson(d,out);
  r->send(200,"application/json",out);
}

// ---------------------------------------------------------------------------
void portalBeginAP(){
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("FlightEye-Setup");
  dns.start(53,"*",WiFi.softAPIP());

  server.on("/scan",HTTP_GET,[](AsyncWebServerRequest* r){
    int n = WiFi.scanNetworks();
    JsonDocument d; JsonArray a = d.to<JsonArray>();
    for(int i=0;i<n && i<20;i++) a.add(WiFi.SSID(i));
    String out; serializeJson(d,out);
    r->send(200,"application/json",out);
  });

  server.addHandler(new AsyncCallbackJsonWebHandler("/save-wifi",
    [](AsyncWebServerRequest* r, JsonVariant j){
      cfg.wifiSsid = j["ssid"] | "";
      cfg.wifiPass = j["pass"] | "";
      cfg.save();
      r->send(200,"application/json","{\"ok\":true}");
      g_wifiSubmitted = true;
    }));

  server.on("/",HTTP_GET,[](AsyncWebServerRequest* r){ r->send(200,"text/html",SETUP_HTML); });
  server.onNotFound([](AsyncWebServerRequest* r){ r->send(200,"text/html",SETUP_HTML); });
  server.begin();
}

void portalBeginSTA(){
  apMode = false;
  server.on("/",HTTP_GET,[](AsyncWebServerRequest* r){
    AsyncWebServerResponse* res = r->beginResponse(200,"text/html",ADMIN_HTML);
    res->addHeader("Cache-Control","no-store, no-cache, must-revalidate, max-age=0");
    res->addHeader("Pragma","no-cache");
    r->send(res);
  });
  server.on("/api/status",HTTP_GET,handleStatus);
  server.on("/api/config",HTTP_GET,handleGetConfig);
  server.on("/readme",HTTP_GET,[](AsyncWebServerRequest* r){
    String page; page.reserve(sizeof(README_PAGE_HEAD)+sizeof(kReadmeMdEscaped)+sizeof(README_PAGE_TAIL));
    page += FPSTR(README_PAGE_HEAD);
    page += FPSTR(kReadmeMdEscaped);
    page += FPSTR(README_PAGE_TAIL);
    r->send(200,"text/html",page);
  });
  server.on("/api/log",HTTP_GET,[](AsyncWebServerRequest* r){
    r->send(200,"application/json",logAsJson());
  });
  server.on("/api/traffic",HTTP_GET,[](AsyncWebServerRequest* r){
    JsonDocument d; JsonArray a = d.to<JsonArray>();
    for(const auto& t : trafficList()){
      JsonObject o = a.add<JsonObject>();
      o["callsign"]=t.callsign; o["type"]=t.type;   o["icon"]=t.icon;
      o["alt"]=t.altFt;         o["gs"]=t.gs;       o["distKm"]=t.distKm;
      o["included"]=t.included; o["featured"]=t.featured; o["reason"]=t.reason;
    }
    String out; serializeJson(d,out);
    r->send(200,"application/json",out);
  });
  // v3.19: powers the admin page's live radar mirror. Same dead-reckoned
  // snapshot the physical radar page uses (radarSnapshot()), so it's exactly
  // the same aircraft/filters/range - just a different renderer.
  server.on("/api/radar",HTTP_GET,[](AsyncWebServerRequest* r){
    std::vector<RadarBlip> blips;
    radarSnapshot(blips);
    JsonDocument d;
    d["rangeKm"]  = cfg.radiusKm;
    d["imperial"] = cfg.imperial;
    JsonArray a = d["blips"].to<JsonArray>();
    for(const auto& b : blips){
      JsonObject o = a.add<JsonObject>();
      o["callsign"]=b.callsign;     o["distKm"]=b.distKm;
      o["bearingDeg"]=b.bearingDeg; o["track"]=b.track;
      o["gs"]=b.gs;                 o["altFt"]=b.altFt;
      o["icon"]=b.icon;             o["emergency"]=b.emergency;
    }
    String out; serializeJson(d,out);
    r->send(200,"application/json",out);
  });
  server.on("/api/calibrate",HTTP_POST,[](AsyncWebServerRequest* r){
    r->send(200,"application/json","{\"ok\":true}");
    extern void beginCalibration();
    beginCalibration();
  });
  server.on("/api/forget-wifi",HTTP_POST,[](AsyncWebServerRequest* r){
    r->send(200,"application/json","{\"ok\":true}");
    extern void doForgetWifi();
    delay(300); doForgetWifi();
  });
  server.on("/api/factory-reset",HTTP_POST,[](AsyncWebServerRequest* r){
    r->send(200,"application/json","{\"ok\":true}");
    extern void doFactoryReset();
    delay(300); doFactoryReset();
  });
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/config",
    [](AsyncWebServerRequest* r, JsonVariant j){
      JsonDocument d; d.set(j);
      cfg.fromJson(d);
      cfg.homeSet = true;   // any admin-page save from here on is a deliberate one
      cfg.save();
      extern void applyLiveConfig();
      applyLiveConfig();
      logf("settings saved from admin page");
      r->send(200,"application/json","{\"ok\":true}");
    }));
  server.begin();
}

void portalLoop(){ if(apMode) dns.processNextRequest(); }
