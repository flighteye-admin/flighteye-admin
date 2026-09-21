# Flight Eye — firmware v3.30 (ESP32 CYD / ESP32-2432S028R)

## Build & flash
Open the folder in VS Code with PlatformIO, click Upload. Serial Monitor at 115200.
Admin page: **http://flighteye.local** (or the IP shown on the Connected screen,
or tap the device screen for a QR code that opens it directly).

## New in v3.30

**Radar screen: proper aircraft glyphs, bigger per-aircraft text**
- Each blip on the radar screen was a 3px dot with a short 7px tick for
  heading - now it's a small heading-aligned plane shape (fuselage, wings,
  tailplane), still drawn with a handful of lines so it's cheap enough to
  redraw every tick for several aircraft at once.
- The callsign/altitude label next to each blip was using the smallest
  built-in font (the same one as the N/S/E/W compass marks) - bumped up a
  size so it's actually easy to read at a glance, with the label spacing
  widened to match.

**Route lookup: hexdb.io as a fallback when adsbdb has nothing**
- "- en route -" sometimes showed even for a locked/tracked aircraft, not
  because it genuinely had no filed route, but because adsbdb.com simply
  hadn't logged that particular callsign. `enrich()` now tries hexdb.io -
  a second, differently-sourced free route database - whenever adsbdb
  comes back empty, resolving the ICAO airport codes it returns into the
  IATA code + airport name the display already shows. Only runs for
  whichever single aircraft is actually on screen or locked (same as the
  existing adsbdb call), so this doesn't add per-poll traffic - just a
  couple of extra requests on the rarer occasions the first source misses.

## New in v3.29

**Removed adsb.lol from polling - feeder-only lockdown, same as airplanes.live**
- adsb.lol kept 403ing every request with "User-Agent too generic; include
  valid contact info" even with the confirmed-correct contact-info format
  (see the v3.22/v3.26 notes below). Research turned up two independent,
  unrelated ESP32/ADS-B projects hitting the identical 403 from adsb.lol in
  the same week, including one from an already-authenticated active feeder
  on adsb.lol's own feeder-only endpoint - strong evidence this is a
  deliberate, broad access-policy change on adsb.lol's side (the same
  "feed us or no API" model airplanes.live and adsb.one already moved to),
  not something fixable with a different User-Agent string.
- adsb.lol is now removed entirely from both the area poll (pollTraffic)
  and the locked-aircraft global lookup (fetchByIdent), matching how
  airplanes.live was handled in v3.26. adsb.fi remains the sole free,
  open, no-feeder-required source; adsb.one stays available as an opt-in
  toggle for anyone who does end up feeding one of these networks.

## New in v3.28

**Diagnostic: adsb.fi returning zero aircraft at a confirmed-correct location**
- After fixing the tracking centre location (it was left at the factory
  default), adsb.fi kept logging "returned 0 aircraft" every poll, even
  though querying the exact same lat/lon/radius directly (outside the
  device) returns 50+ aircraft at the same moment. That means adsb.fi's
  server is treating this specific device's requests differently from a
  normal request for the same URL - most likely some form of silent
  per-IP or per-client soft block (200 OK with an empty "ac" array,
  rather than an honest error), though it could also be a subtle parsing
  bug on our side that only shows up on adsb.fi's real response shape.
- There was no way to tell those apart from the existing logs, so
  fetchSource() now buffers the raw response body and, whenever it adds
  zero aircraft from a 200 OK response, logs a short snippet of exactly
  what the source sent back. No behaviour change otherwise - this is
  purely so the next log capture shows the actual bytes adsb.fi is
  returning to this device, instead of just "0 aircraft".

## New in v3.27

**Version bump only - same code as the actual v3.26 content**
- v3.26 was flashed to the device by USB before its later fixes (adsb.lol
  User-Agent, airplanes.live removal, log wording) were made, so those
  fixes only exist in the v3.26 *release* on GitHub, not on the device.
  Since a device already running "3.26" won't treat another release also
  labelled "3.26" as newer (OTA only moves forward), this version exists
  purely so the already-flashed device has something to actually update
  to. No functional changes beyond what v3.26 already contains.
- Also fixed the release pipeline itself while cutting v3.26's release:
  the tag-match pattern only accepted three-part tags (v3.26.0) and never
  matched this project's two-part ones, and the workflow's default
  permissions couldn't publish a Release even once it did trigger. Both
  fixed - this is the first version to go out as a real, working release.

## New in v3.26

**Fix: adsb.lol still rejecting the User-Agent as "too generic"**
- v3.22's User-Agent added a real contact email but no link, and adsb.lol
  kept 403ing it. Checked against a confirmed-working real-world example
  (github.com/NIKX-Tech/karshipta PR #202, which hit and fixed the exact
  same "too generic" rejection): the accepted shape is
  `AppName/version (+https://url; contact@email)` - an identifying link
  *and* contact address, not an address alone. Now that this project has
  a real public repo (since v3.23), that's the link:
  `FlightEye-ESP32/3.26 (+https://github.com/flighteye-admin/flighteye-admin; ...)`.

**airplanes.live removed as a source entirely**
- Confirmed: their free API is now feeder-only. Getting your own feed
  counted requires buying/running receiver hardware (SDR dongle, antenna,
  a always-on Pi or similar) - a real cost and commitment for what this
  project needs, not just an email or a header tweak. Rather than leave a
  permanently-403ing, unfixable source sitting in the code (and on the
  admin page as a checkbox that does nothing useful), it's gone: removed
  from both the area poll and the locked-aircraft lookup in flight.cpp,
  the config field, and the admin page's Data sources list.
- adsb.lol and adsb.fi remain, both free and open, no feeder requirement.
  If that ever stops being enough traffic, adsb.one is already wired up
  (off by default - same free-tier 403 story) and could be a third
  free-tier option to revisit.

**Log: distinguish "genuinely 0 aircraft" from "deduped"**
- A source that's the first one to succeed in a poll and finds nothing no
  longer logs as "deduped" (there was nothing to dedupe against yet) - it
  now says "returned 0 aircraft", so a quiet source at your location
  reads correctly instead of looking like it silently discarded real
  traffic.

## New in v3.25

**Confirmed: a lower-or-equal GitHub release is always ignored**
- `ota.cpp`'s version check only ever treats a release as an update when its
  tag is strictly newer (major.minor.patch) than the running `FW_VERSION`.
  An equal or older "latest" release is logged as up to date and nothing
  downloads - this was already the behaviour, just confirming it here.

**Admin page: uses your phone's location as a starting point, once**
- If this device has never had a tracking centre saved (a fresh device, or
  straight after a factory reset), opening the admin page now asks your
  phone/browser for its location and uses that as the starting point,
  instead of the hardcoded factory default - no need to hunt for the "Use
  my location" button on first use.
- This only fires until the first real "Save changes" - once a tracking
  centre has actually been saved, it's never silently overridden again, so
  opening the admin page later while away from home (travelling, at work)
  won't shift your tracking centre out from under you.
- The "Use my location" / "Use crosshairs" buttons on the map still work
  exactly as before for changing it deliberately at any time.

## New in v3.24

**Fix: setup portal stuck in an endless "let's get set up" loop**
- v3.23's new OTA partition table renamed the LittleFS storage partition
  from "spiffs" to "littlefs" for readability. Turns out Arduino's
  `LittleFS.begin()` looks for a partition literally named "spiffs" by
  default, regardless of what filesystem actually lives there - so
  LittleFS could never mount, Wi-Fi credentials could never actually save,
  and every reboot looked like a first boot. Renamed it back to "spiffs"
  in `partitions_ota_4mb.csv` (offsets/sizes unchanged). If you flashed
  v3.23 and got stuck on the setup screen, this is why.

**Setup page: "Show" button on the Wi-Fi password field**
- Lets you peek at what you've typed before hitting Connect, same as most
  Wi-Fi password prompts elsewhere.

**Setup page: fixed field alignment on mobile**
- The network/password fields were rendering slightly wider than their
  container on narrow screens (a `box-sizing` default, not a phone-specific
  bug - just far more visible on a phone-width page, which is what this
  screen mostly gets viewed on, since it only ever loads when you're
  joining the FlightEye-Setup Wi-Fi network to configure the device).

## New in v3.23

**Over-the-air updates from GitHub Releases**
- Once connected to Wi-Fi, the device checks this repo's latest GitHub
  Release every 6 hours (and once, ~20s after boot). If the release's tag
  is newer than the running `FW_VERSION` (see `src/config.h`), it downloads
  the `flighteye-firmware.bin` asset and flashes it via `Update.h`, then
  reboots. No manual button or admin-page trigger - it's fully automatic.
- Cutting a release is the entire "ship an update" step from then on: bump
  `FW_VERSION` in `src/config.h`, commit, `git tag v3.24 && git push --tags`.
  `.github/workflows/release.yml` builds the firmware and attaches the
  binary to the GitHub Release automatically.
- Admin page's status feed now includes the running version and OTA state
  (`fwVersion` / `otaState` / `otaChecked` in `/api/status`).
- **One-time catch:** this required switching the partition table from
  `huge_app.csv` (a single ~3MB app slot, no room for an OTA update to land
  in) to `partitions_ota_4mb.csv` (two ~1.94MB app slots). A partition
  table change can't be delivered over OTA - it needs one last USB flash.
  That flash also relocates the LittleFS region, so the device won't find
  its old `config.json` and will drop into the Wi-Fi setup portal once;
  re-enter Wi-Fi and your settings and everything's back to normal, with
  every release after that arriving over the air.
- **Before flashing:** build once (`pio run`) and check the reported flash
  usage. The two OTA slots are ~1.94MB each - if the current build is
  already close to or over that, either slot won't fit the full image.
  Worth checking before relying on it.

## New in v3.22

**Fix attempt: adsb.lol / adsb.fi rejecting a "too generic" User-Agent**
- The v3.21 device log caught adsb.lol explaining itself plainly: `User-Agent too
  generic; include valid contact info.` Our previous User-Agent was a placeholder
  ("FlightEye-ESP32/3.20 (+https://github.com/)") that didn't actually point at
  anything - understandably read as generic/anonymous.
- Changed to `FlightEye-ESP32/3.22 (genereynolds.uk+flighteye@gmail.com)` - a real,
  working contact address (a Gmail "+" alias, so it's filterable/identifiable
  without touching the primary inbox address). This project has no public repo or
  site yet, so a URL wasn't an option; the rejection message asked for "contact
  info", not specifically a website, and an email satisfies that on its own.
- adsb.fi's persistent "deduped (total 0)" despite real nearby traffic is the
  working theory this might also fix, on the reasoning that it's silently
  applying similar screening rather than 403ing outright - unconfirmed, watch the
  device log after flashing to see whether it starts reporting real counts.
- If adsb.lol still rejects this specific format, the fuller error text is now
  visible in the device log (thanks to v3.21's response-body logging) rather than
  just a bare status code, so the actual reason will be visible for a follow-up.

## New in v3.21

**Diagnostic: log the actual response body on a non-200 source fetch**
- `fetchSource()` previously only logged the bare HTTP status ("adsb.lol HTTP 403"),
  which doesn't distinguish a generic block from a source explicitly telling you
  why. It now logs the response body too, e.g. airplanes.live currently returns a
  plain-text `{"error": "Please contact us at ..."}` message for unregistered
  callers, rather than an unexplained block - this is a source-side access policy,
  not a bug, and previously there was no way to see that from the device log alone.
- `LOG_LEN` (devlog.h) bumped from 96 to 160 characters per line so these longer
  messages aren't truncated before they say anything useful. Costs about 2.5 KB of
  RAM for the larger ring buffer - negligible.

**Admin page: "View README / changelog" button**
- Added an About card to the admin page with the current firmware version and a
  button that opens `/readme` - the full contents of this file, served straight off
  the device, so you can check what changed without needing the source tree open.
  The page is baked into the firmware at build time (it's this exact file), so it's
  always in sync with whatever's actually flashed.

## New in v3.20

**Fix: adsb.fi area poll always returned zero aircraft**
- The area poll (`pollTraffic()`) queried adsb.fi via its `v2/lat/{lat}/lon/{lon}/dist/{dist}`
  endpoint. That specific endpoint is adsb.fi's older, now-deprecated format: it wraps
  results under a JSON key literally called `"aircraft"`, not `"ac"` like every other
  endpoint/source this firmware talks to. Because `fetchSource()`'s ArduinoJson filter
  only keeps an `"ac"` subtree, the whole response was silently dropped during parsing -
  adsb.fi would report HTTP 200 and "deduped (total 0)" on every single poll, no matter
  how much real traffic was in range.
- Fixed by switching to the `v3/lat/{lat}/lon/{lon}/dist/{dist}` endpoint, which returns
  the standard `"ac"` schema (matching adsb.fi's own guidance to move new integrations
  off the deprecated v2 form). No parser changes needed elsewhere.

**Fix: airplanes.live / adsb.lol / adsb.one returning HTTP 403**
- All outbound requests (`fetchSource()`, `fetchByIdent()`'s lookups, and the adsbdb
  route-enrichment call) sent no `User-Agent` header at all - the ESP32 `HTTPClient`
  default. Several of the free aggregators appear to have tightened bot/anti-abuse
  screening recently and now reject requests with a blank User-Agent outright (403,
  before ever reaching their app logic).
- Fixed by sending an explicit `User-Agent: FlightEye-ESP32/3.20 (+https://github.com/)`
  header on every request. If you fork this, consider pointing that URL at your own
  repo so a maintainer can tell whose traffic is whose.

## New in v3.19

**Touch dead zone: fixed 50px on all four edges**
- The admin-adjustable per-edge dead zones from v3.17/v3.18 (and the
  temporary v3.18-dbg debug overlay used to diagnose them) are gone. In
  their place: a single fixed 50px margin on every edge of the screen,
  hardcoded as `TOUCH_DEAD_PX` near the top of `main.cpp`. Nothing to tune
  from the admin page any more - if 50px ever turns out wrong for a
  different case, change that one constant and reflash.

**Locking a flight is admin-page only now**
- The touchscreen no longer has a lock/unlock button. Tapping the flight
  card (outside the info/LED/radar page cycle) always opens the device
  info/QR screen - there's no other touch target on it any more. To lock
  onto a specific flight, use the admin page's "Lock to a flight" card or
  tap a row in the traffic table, same as before.
- The padlock icon on the device screen is now a pure status indicator: it
  only appears at all when a lock is actually active (both on the flight
  card and the "searching worldwide" locked-but-out-of-range screen). No
  lock means no icon, rather than an always-visible tappable button.

**Admin page: live device screen + live radar mirrors**
- New "Live device screen" card shows a close approximation of exactly
  what's on the device's screen right now - callsign, lock indicator,
  route, ALT/SPD/HDG, vertical rate, distance, footer - redrawn on a canvas
  using the same colours as the physical panel. It piggybacks on the
  existing 4-second status poll, so it adds no extra load on the device.
  Fonts are a close match, not pixel-identical, since a browser canvas can't
  reproduce the device's own bitmap fonts.
- New "Live radar" card mirrors the device's radar page (tap the display
  three times from the flight card to see it there) - same aircraft, same
  filters, same range ring, refreshed every 3 seconds via a new
  `/api/radar` endpoint. 3s was chosen as a safe middle ground: fast enough
  to feel live, slow enough not to add meaningful extra request load
  alongside the existing status/log/traffic polls.

## New in v3.18

**Touch dead zone now covers all four edges**
- v3.17 only let you ignore touches near the right edge, on the assumption a
  case would only press there. Turns out a case can press on the top or
  sides too, and the screen kept cycling through pages on its own. The
  single right-edge setting is now four independent settings under Display:
  "Ignore touches within (px) of right/left/top/bottom edge" - each 0 by
  default (off), each tunable live from the admin page with no reflash
  needed, same as before.
- Set only the sides that are actually misbehaving - if your case only
  presses the top, leave left/right/bottom at 0. Right/left cap at 100px,
  top/bottom cap at 60px (the screen is only 240px tall in landscape, so a
  bottom or top zone bigger than that would swallow the whole display).
- The lock button still sits right after the callsign rather than in a
  corner, so a right or top dead zone won't cover it.

## New in v3.17

**Distance sanity check**
- The area poll asks each source for aircraft within your configured radius,
  but that filtering happens server-side and this firmware never re-checked
  it locally - a stale or glitched position fix (common for MLAT-derived
  positions on weak-signal aircraft) could slip through a source's own
  filter and show up looking "hundreds of km away" despite being reported as
  nearby. Aircraft are now rejected locally if their computed distance is
  well beyond your radius (radius x1.15 + 3km, generous enough not to
  exclude a real aircraft near the edge). Excluded ones show up in the admin
  page's traffic list with reason "bad position?" so you can confirm this is
  what happened, rather than it silently vanishing. Emergency squawks are
  unaffected - they still always get through, as before.
- I couldn't confirm this was the exact mechanism behind what you saw
  without the screenshot (it didn't come through - happy to take another
  look if you can re-attach it), but this is a real gap regardless of the
  exact cause.

**Case-friendly touch handling**
- New admin setting under Display: "Ignore touches within (px) of right
  edge" (0 = off). If a case presses the panel's edge hard enough to
  register as a touch, raise this until the false cycling through pages
  stops - no need to touch the (possibly misbehaving) screen to set it,
  since it's just a normal admin page field.
- To make that dead zone possible without losing the ability to lock a
  flight by touch, the lock button moved from the top-right corner to
  immediately after the callsign (top-left, next to the yellow flight ID) -
  on both the flight card and the "searching worldwide" locked-but-out-of-
  range screen.

## New in v3.16

**Radar screen tweaks**
- No longer auto-dismisses after ~20s - it now stays up until you tap it,
  since it's meant to be watched for a while rather than glanced at.
- Removed tap-to-lock on a blip (too fiddly to hit reliably at this screen
  size). Any tap on the radar now just returns to the flight card; use the
  lock button on the flight card or the admin page's traffic list instead.

## New in v3.15

**Fix: admin page completely unreachable (ERR_CONNECTION_TIMED_OUT) on v3.14**
- v3.14 called `configTime()` (to kick off NTP for the new "last poll"
  timestamp) *before* the web server started. If that call ever stalled for
  any reason - a slow or broken DNS lookup for the NTP pool, a flaky network -
  it would have delayed `portalBeginSTA()` from ever running, leaving nothing
  listening on port 80 at all. That fits the reported symptom (timeout on
  the IP address directly, not just ".local") better than anything else I
  could find in the v3.14 diff.
- Fix: NTP now kicks off once, from inside the main loop, well after the web
  server is already listening - so regardless of what NTP does, the admin
  page can no longer be held up by it.
- I can't fully confirm this was the exact mechanism without a serial log
  from the affected boot, but this change is safe either way and directly
  removes the one thing in v3.14 that ran ahead of the web server starting.
  If the admin page is still unreachable after this, the serial monitor
  output from boot to "Admin: http://flighteye.local" would help narrow it
  down further.

## New in v3.14

**Swipe-to-skip removed**
- It didn't work reliably in practice, and cost every tap a bit of latency
  (had to wait for release to tell tap from swipe apart). Reverted to firing
  on the initial touch, same as before v3.13. May revisit with a different
  approach later.

**Radar: flight level added**
- Each blip's callsign now has its flight level (FLxxx, or ft/m below
  18,000ft) printed just underneath, same tiny font.

**flighteye.local**
- The device's hostname is now set *before* Wi-Fi connects rather than
  after, which is the order the DHCP handshake actually needs it in - it was
  simply too late before to reliably reach the router.
- That said: `.local` resolution fundamentally depends on the client OS/
  browser understanding mDNS, which is built into macOS/iOS but usually
  needs extra software on Windows (e.g. Apple's Bonjour) and is patchy on
  Android - `DNS_PROBE_FINISHED_NXDOMAIN` on a PC without Bonjour installed
  is expected, not a bug in this firmware. The IP address and the QR code on
  the device screen are the reliable route and always work.

**Admin page**
- Trimmed the verbose "Can't open this page from a phone?" paragraph from
  the Network card.
- Status now shows the last poll's date/time (needs internet access for NTP
  to sync in the background after Wi-Fi connects; shows "not yet synced"
  until then). Sent as UTC and converted to your browser's local time
  automatically, since the device itself has no idea what timezone you're in.

## New in v3.13

**Radar polish**
- Removed the rotating sweep line - it looked artificial and was the main
  source of flicker on real hardware. The radar now just redraws about once
  a second to reflect dead reckoning, no animation gimmick.
- Each blip is now labelled with its callsign, in the same tiny font used
  for the N/S/E/W marks and range label.

**Swipe to skip**
- On the flight card, a clear left-right or right-left swipe jumps straight
  to the next aircraft in the rotation instead of waiting for the dwell
  timer. Taps still work exactly as before, just with a hair more latency
  (up to ~400ms) since the firmware now has to wait for the finger to lift
  before it can tell a tap from the start of a swipe - the touch controller
  has no built-in gesture support, so this is done by tracking press and
  release ourselves.
- The swipe distance threshold (45px) and the "clearly horizontal" test are
  first-pass values - I couldn't tune these against a real finger on real
  glass, so give them a try and let me know if a swipe needs to be less/more
  deliberate to register.

## New in v3.12

**Radar screen**
- A fourth tap on the display (flight card -> QR/device info -> LED key ->
  **radar** -> back to the flight card) shows a north-up radar sweep: aircraft
  within the "Radius km" range set on the admin page, plotted by bearing and
  distance from home, colour-coded the same as the LED "aircraft class" mode,
  with a short heading tick on each blip.
- Respects the same aircraft filters as the flight card - if Military is
  switched off there, it won't show up on the radar either.
- Between polls, blips creep along their last known track/speed (dead
  reckoning) rather than jumping only when the sky is re-polled - though in
  practice most aircraft move only a pixel or two between 30-120s polls at
  this scale, so don't expect fast-moving dots.
- Tap a blip to lock the display onto it, same as tapping a row in the
  admin page's traffic table. Tapping empty space returns to the flight card.
- No offscreen framebuffer is used (this board has no PSRAM configured), so
  the whole screen redraws every ~200ms rather than using a smooth
  double-buffered animation. Worth an eye on real hardware for flicker -
  I can't test actual rendering on physical hardware from here.

## New in v3.11

**Making phone access bomb-proof**
Follow-up to v3.10: even with the map fix, a fixed IP set up for a router
you've since replaced or reconfigured can leave the device unreachable from
any client that's on the *real*, current subnet - while a PC that's been
connected a long time (stale lease/ARP entry) can keep working, making it
look like a phone-only problem. That mismatch throws `ERR_ADDRESS_UNREACHABLE`
in Chrome (no route to the address at all), not a timeout.
- **Fixed-IP sanity check**: at connect time, the gateway you entered must
  actually sit on the same subnet as the fixed IP (per the mask). If it
  doesn't, the device now ignores the fixed IP, falls back to DHCP, and logs
  a clear explanation instead of silently binding to a dead address.
- **Live network info, always visible**: the device-info screen (tap the
  display) and the admin page's Status panel now show the current gateway
  and subnet mask (and whether it's DHCP or fixed). Compare these against
  your phone's own Wi-Fi details screen to spot a network/VLAN mismatch in
  seconds.
- **Troubleshooting guidance on the admin page**: the Network card now spells
  out the three most common Android causes for "can't reach this page" -
  an active VPN or private-DNS/ad-block app (routes local traffic through a
  tunnel by default), the phone being on a different Wi-Fi band/guest
  network, and a stale fixed-IP setup.

## New in v3.10

**Admin page no longer depends on internet access to be usable**
- The map widget (Leaflet + OpenStreetMap tiles) was previously loaded as a
  blocking `<head>` tag. If that fetch was slow or blocked - common on phones
  behind ad-block DNS, MDM/content filtering, or an isolated guest/IoT Wi-Fi
  network - the **entire admin page** would stall before status, log,
  filters, or reset controls ever appeared, even though none of those need
  internet at all. This is the most likely explanation for admin access
  failing on some Android phones while working fine on a PC.
- The map now loads lazily in the background. If it can't load within 8s, the
  rest of the page still works normally and the map area shows a short
  message instead of hanging.

**LED colour key**
- Tap the device screen once for the QR/device-info page (as before), tap
  again for a new LED key page listing what each colour and pattern means
  for every LED mode. A third tap returns to the flight card.

## From v3.4 (changes through v3.9 were not recorded in this file)

**Poll and rotate, instead of re-picking the same aircraft**
- The sky is now polled **once every 60s** (adjustable 30-120s on the admin page).
- Between polls the display **rotates through the whole list**, one aircraft every
  `Seconds per aircraft` (default 8s), so you actually see everything in range.
- Rotation order follows "Rotation order": **closest first** or **lowest first**.
  Emergencies always jump to the front.
- Route lookups happen lazily, only for aircraft actually shown, once per poll.
- Footer shows the rotation position (e.g. "3/10"); the admin status shows it too.

**Display fixes**
- Fixed the footer overlap: the footer now shows the **short type code** and
  registration; the full aircraft name stays on the operator line where it fits.
- **Distance from the device** now shown on the card, right of the climb/descent rate
  (miles or km, following the units setting).

## From v3.3

**Aircraft names**
- ~190 ICAO type codes now spell out in full ("P28A" -> "Piper PA-28 Cherokee",
  "B744" -> "Boeing 747-400"). The table lives in flash, not RAM: ~6 KB, zero heap cost.
- Shown on the operator line for GA/private traffic, and in the footer for airliners.

**Traffic list on the admin page**
- Nearest 25 aircraft: callsign, type, altitude, speed, distance.
- Featured aircraft highlighted; filtered-out aircraft greyed with the reason
  ("GA off", "on ground", "no identity") - handy for working out why something
  isn't showing.
- Tap any row to lock the display onto that aircraft.

**Data sources**
- adsb.one **off by default** (it returns HTTP 403 for non-feeders).
- adsb.lol parse failures now retry once quietly instead of logging an error;
  non-JSON responses are detected before parsing.
- No rate-limit pause after the last enabled source (saves ~2s per cycle).
- Dedupe merges log as "deduped" rather than looking like a failure.

**Recovery / reset**
- The old behaviour of silently wiping your Wi-Fi after 3 failed attempts is **gone**.
  It now retries indefinitely and never destroys credentials on its own.
- After ~60s of failure a help screen appears showing the network, elapsed time,
  and the BOOT button instructions.
- **BOOT button: hold 5s = forget Wi-Fi, hold 10s = factory reset.** On-screen
  countdown, release early to cancel, RGB LED feedback.
  (Do NOT hold BOOT at power-up - that enters flash mode. Press it while running.)
- Admin page has matching "Forget Wi-Fi" and "Factory reset" buttons.
- Auto-reconnect if Wi-Fi drops while running.

**Labels**
- "unknown operator" -> aircraft name, or "Private / GA".
- "? to ?" -> "no route filed" for GA traffic.

## Still to come
- "Inbound" featured rule (currently behaves as nearest)
- Touch calibration screen reachable from the admin page
- SD-card flight history
