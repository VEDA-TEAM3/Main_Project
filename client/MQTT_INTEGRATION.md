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

`RiskFrame.ts` is **not** monotonic. `ConcatFuser` stamps the fused frame with the *oldest* observation timestamp in
the window, and each channel's timestamp comes from its own CCTV clock, so the value moves backwards whenever the
set of channels in a window changes. Qt therefore orders risk frames by arrival (QoS 1 preserves per-topic order) and
uses `ts` only to drop a redelivered copy of the frame it already has. Ordering by `ts` instead would discard every
frame carrying the lagging channel's clock, freezing the map and then jumping it forward in one step.

Qt also caps how far an object may move between frames, at 25 m/s in world units — deliberately not in normalized
units, where the same constant would mean 54 m/s on a 60 m site and 13.5 m/s on a 15 m one. Multi-camera fusion can
represent one object by a different observation from frame to frame, which arrives as a single-frame teleport across
the site and back. An isolated one is dropped outright by the three-sample median; the cap bounds what a repeated one
can do to at most 25 m/s × the frame interval. Genuine movement keeps arriving in the same direction and is not
slowed. Repeated `[TOPVIEW] Rate-limited jump` lines mean the upstream fusion or camera calibration is unstable — the
client is only hiding it.

The shared world-coordinate contract has positive Y pointing north/up. Qt therefore flips the
Y axis when mapping it to screen coordinates. Set `VEDA_MAP_INVERT_Y=0` only when an upstream source already provides
screen-style coordinates whose positive Y points down. Calibrated `VEDA_MAP_MIN_*` and `VEDA_MAP_MAX_*` values are
required for exact CH-01 through CH-04 quadrant assignment; automatic bounds are only a safe fallback.

## Build

The checked-in preset expects Qt 6.11.1 with the Qt MQTT module and MinGW 13.1:

```powershell
cmake --preset debug-ninja
cmake --build --preset debug-ninja
```

Before running on Windows, point `VEDA_MQTT_CA_FILE` to the actual CA certificate path.
