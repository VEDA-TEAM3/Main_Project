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

If all four values are absent or invalid, Qt auto-expands bounds from received positions. Values already in `[0,1]` are used as normalized coordinates.

`RiskFrame.pos` follows the shared world-coordinate contract, where positive Y points north/up. Qt therefore flips the
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
