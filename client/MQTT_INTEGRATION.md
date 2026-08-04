# MQTT/TLS integration

The Qt client consumes the wire formats used by the supplied broker, command client, and `MqttTopViewSink`.

## Channel numbering

| Message | Wire channel | Qt index |
| --- | ---: | ---: |
| `veda/hw/ch/+/status` | topic/payload channel 0..3 | 0..3 (UI CH 01..04) |
| `veda/hw/status` | `channelId` 1..4 | 0..3 |
| `veda/qt/event` | `channelId` 1..4 | 0..3 |
| `veda/ch/{ch}/alive` | topic `ch` 0..3 | 0..3 |
| TopView | topic/payload `ch` 0..3 | 0..3 |
| Head blur metadata | topic/payload `ch` 1..4 | decoded video ROI box blur (Qt index 0..3) |

The central broker server rejects `channelId <= 0`, so central status and event messages must never be interpreted as zero-based values. The previous implementation did that and displayed wire channel 1 on `CH 02` while rejecting wire channel 4.

## Subscriptions

- `veda/hw/ch/+/status` (QoS 1)
- `veda/hw/status` (QoS 1)
- `veda/ch/+/alive` (QoS 1)
- `veda/ch/+/topview` (QoS 0, direct `MqttTopViewSink` path)
- `veda/qt/ch/+/topview` (QoS 0, central relay path)
- `veda/qt/event` (QoS 1)
- `veda/ch/+/blur` (QoS 0, `BlurFrame` JSON)

The direct and relayed TopView topics can be enabled at the same time. Qt drops an equal or older `ts` per channel, so a relayed copy of a direct frame is not rendered twice.

## Runtime flow

1. `MqttTopViewSink` publishes a `TopViewFrame` to `veda/ch/{0..3}/topview`.
2. Qt validates `v`, `ts`, topic/payload channel equality, object class, position, confidence, and edge fields.
3. The first valid live frame stops the built-in demo. The latest frames from all four channels are combined. Empty frames remove the channel's objects, and a channel is removed if no new frame arrives for five seconds.
4. `mqtt_tls_broker_server.cpp` consumes `veda/metadata/event`, applies hardware state, and publishes `veda/hw/status` plus `veda/qt/event`.
5. Qt applies the event to the correct channel card, event log, digital-twin risk color, and danger overlay. A failed hardware result is shown as failed feedback without accepting the unconfirmed state.
6. Status produced after a command from `mqtt_tls_command_client.c` is consumed through `veda/hw/ch/{0..3}/status`. When `channelId` is present, Qt validates it against the topic channel. The internal 0-based index is displayed as `CH 01..04`.

## Environment

Existing broker settings:

```text
VEDA_MQTT_HOST=100.73.128.114
VEDA_MQTT_PORT=8883
VEDA_MQTT_CA_FILE=/etc/veda/certs/ca.crt
VEDA_MQTT_CLIENT_ID=<optional unique id>
VEDA_MQTT_DEBUG=1
```

`VEDA_MQTT_DEBUG=1` prints connection, subscription, status payload, and rate-limited TopView summaries.
Set it to `0` to disable MQTT console logging. TopView logging is limited to once per second per channel so
debug output does not flood the UI and video threads.
`veda/ch/{channelId}/blur` carries `BlurFrame` JSON. The topic channel and payload `ch` are both 1-based. Qt validates
their equality, converts the wire channel to its 0-based video index, keeps only `Head` targets, matches their UTC `ts`
to the delayed RTSP frame, expands each normalized bbox by 18%, and applies a two-pass box blur before
`d3d11videosink`. Set
`QTCCTV_BLUR_SYNC_OFFSET_MS` when the actual video delay differs from the default RTSP latency (2500 ms).

```json
{"v":1,"ts":1753060000123,"ch":1,"blurs":[{"id":3022,"cls":"Head","box":{"l":0.31,"t":0.18,"r":0.38,"b":0.31}}]}
```

An empty `blurs` array is a valid frame and must still be published so an earlier Head box is not reused for a
later video frame.

One client owns one 4-channel CCTV unit and the 10 x 10 m it covers, each channel taking one 5 x 5 m quadrant of it.
A larger site is covered by deploying more clients, not by adding channels to one, so the world extent below is the
area of *this* client, not of the whole site — `digitalTwin.world` ships as a 10 x 10 m square centred on the unit
(`-5..5` on both axes). The four cameras look outward from the centre of that square, so the channel a risk object
belongs to is the wedge it sits in, cut by the two 45-degree diagonals through the centre — not by the axes:
CH01 = north, CH02 = east, CH03 = south, CH04 = west. This holds exactly as long as the unit is centred on the
world origin. An object stays on its previous channel until it is `channelBoundaryHysteresisMeters` clear of a
diagonal, so a track running along a boundary does not flip back and forth.

TopView positions are world coordinates. For a stable production map, set the calibrated world extent:

```text
VEDA_MAP_MIN_X=<left bound in meters>
VEDA_MAP_MIN_Y=<top bound in meters>
VEDA_MAP_MAX_X=<right bound in meters>
VEDA_MAP_MAX_Y=<bottom bound in meters>
```

`RiskFrame.pos` is always in meters on the shared drawing frame (origin at the intersection centre), so negative
values are normal and are never interpreted as already-normalized `[0,1]` input. If all four bounds are absent or
invalid, Qt estimates bounds from the first `digitalTwin.world.automaticBoundsWarmupMs` of samples and then widens
them whenever a position falls outside — the estimate from a short warmup window is narrower than the real site, and
without that widening the surplus area would be clamped onto the map edges. The bounds only ever grow, so the map
scale settles once the site has been covered. Watch `[TOPVIEW MAP] Automatic world bounds estimated|expanded` in the
log to see the extent Qt is actually using.

## TopView coordinate diagnostics

Set `logging.topview` for a one-second reception summary and `logging.topviewDetail` for a per-object, per-frame line
showing every stage of the coordinate path. `VEDA_TOPVIEW_DEBUG` (`0`/`1`/`2`) overrides both without touching the
config file. Leave the high-volume categories (`mqttStatusPayload`, `mqttBlur`, `blurApply`) off while capturing, or
the topview lines drown in them.

Capture with `VEDA_LOG_FILE` rather than by redirecting stderr:

```powershell
$env:VEDA_TOPVIEW_DEBUG = "2"; $env:VEDA_LOG_FILE = "topview.log"; .\Qtcctvclient.exe
```

A GUI-subsystem executable has no console, so Qt's default handler sends messages to `OutputDebugString`
instead of stderr. That path goes through the debugger's shared buffer, which cuts long messages and
interleaves them when the MQTT gateway, the RTSP receivers, and the GUI thread all log at once — the reason
captures came out cut off or empty even with `QT_FORCE_STDERR_LOGGING=1`. `VEDA_LOG_FILE` installs a
`qInstallMessageHandler` that serialises every message with a mutex and writes it whole to the named file
(still echoing to stderr), so a line arrives intact no matter how it is collected.

Lines are also kept under 60 characters so a capture survives being pasted around.

```text
[TV] on lvl=2 inv=1 cap=8.0m/s
[TV] bounds fix x=-5.0..5.0 y=-5.0..5.0
[TV] rx f=10 dup=0 bts=3 arr=49/102/153ms obj=2
[TV] flt lim=1 med=2 ts=-200..400ms
[TV] latest ts=1700000006100 age=38ms obj=2
[TV] g1 raw=2.83,2.83 d=0.14 ch=2
[TV] g19 raw=-1.20,3.40 d=10.72 ch=1 cut med=10.72 lim=0.00
[TV] g19 jump=10.72m cap=0.48m
[TV] disp rx=20 tx=10 coal=10
```

`raw` is the world coordinate the broker sent and `d` how far it moved since that object's previous frame — the
honest measure of upstream stability. `ch` is the channel the quadrant rule derived. `cut` appears only when the
median filter or the speed cap actually corrected an outlier, with how many metres each removed. `rx` counts frames,
duplicates, backward timestamps and arrival intervals (min/median/max); `flt` counts filter interventions in the same
second. `disp` counts frames the dispatcher received, delivered, and overwrote before delivery (`coal`), the last of
which the map never sees.

Per-object lines are throttled per gid (`topviewDetailIntervalMs`, 1 s by default), except that a frame where a
filter intervened is always logged — at most four times a second per object, so a long catch-up after one outlier
cannot flood the capture.

`RiskFrame.ts` is **not** monotonic. `ConcatFuser` stamps the fused frame with the *oldest* observation timestamp in
the window, and each channel's timestamp comes from its own CCTV clock, so the value moves backwards whenever the
set of channels in a window changes. Qt therefore orders risk frames by arrival (QoS 1 preserves per-topic order) and
uses `ts` only to drop a redelivered copy of the frame it already has. Ordering by `ts` instead would discard every
frame carrying the lagging channel's clock, freezing the map and then jumping it forward in one step.

Qt also caps how far an object may move between frames, at 8 m/s in world units — deliberately not in normalized
units, where the same constant would mean a different speed on every differently sized site. One client covers a
10 x 10 m area, so the cap has to sit above the fastest thing that crosses it (walking 1.4 m/s, site driving 3-5 m/s)
and no higher: what an outlier can do to the picture is the cap times the frame interval, 0.8 m here. Multi-camera fusion can
represent one object by a different observation from frame to frame, which arrives as a single-frame teleport across
the site and back. An isolated one is dropped outright by the three-sample median; the cap bounds what a repeated one
can do to at most 25 m/s × the frame interval. Genuine movement keeps arriving in the same direction and is not
slowed. Repeated `[TOPVIEW] Rate-limited jump` lines mean the upstream fusion or camera calibration is unstable — the
client is only hiding it.

The shared world-coordinate contract has positive Y pointing north/up. Qt therefore flips the
Y axis when mapping it to screen coordinates. Set `VEDA_MAP_INVERT_Y=0` only when an upstream source already provides
screen-style coordinates whose positive Y points down. Calibrated `VEDA_MAP_MIN_*` and `VEDA_MAP_MAX_*` values are
required for exact CH-01 through CH-04 wedge assignment; automatic bounds are only a safe fallback.

## Build

The checked-in preset expects Qt 6.11.1 with the Qt MQTT module and MinGW 13.1:

```powershell
cmake --preset debug-ninja
cmake --build --preset debug-ninja
```

Before running on Windows, point `VEDA_MQTT_CA_FILE` to the actual CA certificate path.
