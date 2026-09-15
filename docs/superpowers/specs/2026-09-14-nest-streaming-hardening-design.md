# Turntable Streamer: Nest streaming + hardening pass

Date: 2026-09-14
Status: approved, ready for implementation planning

## Goal

Make the existing ESP32/LyraT V4.3 turntable streamer solid enough to be the
permanent audio source for a Google Nest speaker, and add the reliability,
control, and observability a hobby device left running unattended needs.

## Non-goals

- **No onboard Cast sender.** Getting audio *playing* on the Nest is
  triggered externally (phone, Home Assistant, Google Home app "Cast" to the
  device's stream URL). The board only needs to serve a stream Cast's default
  media receiver can play — it never speaks the Cast protocol.
- **No sub-second sync.** Cast's default media receiver buffers a live HTTP
  stream by roughly 2–5s regardless of who triggers playback. That's accepted
  as "near-real-time"; no custom Cast Web Receiver app.
- **No RTSP, no WAV.** Nest can't play RTSP at all, and can't reliably play
  an unbounded live WAV stream. Both modes are removed rather than hardened.
- **No component-per-concern restructuring.** New code stays as flat files
  in `main/`, matching the project's current size (~600 lines).

## Codec: AAC, not MP3 (revised 2026-09-14)

The original design (and the code as it existed before this pass) assumed
MP3. That's not implementable: **ESP-ADF has never shipped an MP3 encoder**,
only a decoder (`mp3_decoder.h` exists in `esp-adf-libs`; `mp3_encoder.h`
does not, in any version) — confirmed against a local ESP-ADF v2.7 checkout.
The pre-existing `audio_streamer.c`'s `mp3_encoder_init()`/`rtsp_stream_init()`
calls reference APIs that were never part of any real ESP-ADF release, so
this project could never have compiled against stock ESP-ADF.

Investigated and rejected: a community MP3 encoder port for ESP32
(`myvobot/esp32_mp3_encoder`, wrapping the Shine encoder). Measured in its
own README: ~18s to encode a 10s mono clip — slower than real-time by a wide
margin, unoptimized, unclear license/maintenance. A from-scratch integration
of standalone `libshine` as a custom `audio_element` was also considered and
rejected as an open-ended spike with no guaranteed real-time payoff, when a
known-good alternative exists.

**Decision: AAC.** ESP-ADF ships a real, actively-maintained AAC encoder
(`aac_encoder.h` / `aac_encoder_init()`), and Google Cast's default media
receiver plays a live AAC HTTP stream exactly the way it plays a live MP3
one. Same architecture, same Cast-compatibility, no unproven engineering.
If a genuine need for MP3 specifically ever shows up, the libshine spike
above is the documented starting point — not attempted here.

## Serving mechanism: raw_stream + esp_http_server, not http_stream (revised 2026-09-14)

A second load-bearing assumption in the original design was also wrong.
ESP-ADF's `http_stream` audio element — used as `AUDIO_STREAM_WRITER` — is an
**HTTP client**, not a server: its `_http_open()` calls `esp_http_client_open()`,
and `http_stream_cfg_t` has no server-mode flag at all. ESP-ADF's own example
using this exact pipeline shape (`i2s_stream → http_stream`,
`examples/recorder/pipeline_raw_http`) *uploads* recorded audio to a remote
server — it does not expose a `GET` endpoint. The pre-existing code's
`http_stream_set_uri(writer, "/stream.mp3")` (a bare relative path, no host)
was never a working server endpoint against real ESP-ADF, and
`http_stream_set_uri` itself doesn't exist in ESP-ADF v2.7 either
(`audio_element_set_uri` is the real function, but setting it on a
client-only element doesn't make it a server).

**Decision:** end the pipeline in a `raw_stream` reader element instead
(`raw_stream_read()` pulls encoded AAC bytes out on demand — this is
ESP-ADF's documented mechanism for "obtain the pipeline data without an
output stream"). The device's own `esp_http_server` instance (already used
for the setup UI) gets a `GET /stream.aac` handler that loops
`raw_stream_read()` → `httpd_resp_send_chunk()` for each connected client.
No new dependency — `esp_http_server` is already linked.

Accepted limitation, not engineered around: `raw_stream`'s ring buffer is a
single shared FIFO, so two simultaneous `GET` clients would split one
stream's bytes rather than each getting the full thing. Fine for a
single-listener hobby stream (and Cast normally has one active receiver);
revisit only if real multi-room playback is ever needed.

## Audio pipeline

- `app_config_t` (`app_config.h`) drops `stream_mode_t` and `mode`; keeps
  `bitrate` (128/192/256/320 kbps, default 256 — all within AAC's valid
  stereo-at-44.1kHz range) and gains an `input_gain` field.
- `audio_streamer.c` keeps only the AAC branch:
  `i2s_stream_reader → aac_encoder → raw_stream`, served at `/stream.aac`
  (content-type `audio/aac`) by the device's own `esp_http_server` instance
  on port 80, advertised via mDNS as `turntable.local` / `_http._tcp`.
- I2S reader stays at ADF's default 44.1kHz/16-bit/stereo — matches both
  vinyl's practical bandwidth and AAC's expected input.
- Input gain is set explicitly via the codec's HAL at startup from
  `config->input_gain` instead of relying on power-on defaults, so a hot
  turntable preamp doesn't clip the ADC. This is user-tunable from the web
  UI, not a hardcoded constant — the one piece of this system that a fixed
  software default can't get right for every turntable/preamp pairing.
- `sdkconfig`: `CONFIG_ESP32_DEFAULT_CPU_FREQ` 160 MHz → 240 MHz, for margin
  now that MQTT/OTA/button/LED handling share the core with the encoder.
- `sdkconfig`/`partitions.csv`: flash size corrected from the checked-in
  (wrong) `2MB` to the confirmed real `4MB`, with a custom two-OTA-slot
  partition table (`nvs`/`otadata`/`phy_init`/`ota_0`/`ota_1`, 1.75MB per
  app slot). Moved here from the OTA section below because the linked
  binary (ESP-ADF + Wi-Fi + HTTP server, even before MQTT/OTA/button/LED
  code) already doesn't fit IDF's default single-app partition sizing — this
  was always going to be needed before the plan finished, not a new need
  from the AAC switch. Doing the partition-table work once, in its final
  two-slot shape, avoids redoing it again in the OTA pass.

## Wi-Fi reliability

`wifi_manager.c`'s `wifi_event_handler` gets a `WIFI_EVENT_STA_DISCONNECTED`
case: retry `esp_wifi_connect()` with exponential backoff (capped at 30s)
instead of leaving the stream dead until a manual power cycle.

## Captive portal

- Bug fix: `dns_server_task`'s `sizeof(dns_header)` takes the size of the
  *pointer* (4 bytes) instead of `sizeof(struct dns_hdr)`, corrupting the
  offset it walks to find the end of the query. Fix while touching this
  file for the reliability work above.
- The setup `httpd` (currently only started in `start_captive_portal()`) now
  also starts in STA mode, on its own path/port separate from the audio
  `http_stream`'s `/stream.aac`. It serves: Wi-Fi/mode reconfiguration (as
  today), the OTA firmware-URL field, MQTT broker fields, and input gain —
  and a status view (Wi-Fi RSSI, streaming state).
- **Accepted tradeoff (flagged in final review, recorded here deliberately):**
  this makes `/connect` (rewrite Wi-Fi credentials + reboot) reachable,
  unauthenticated, from anywhere on the home LAN for the device's entire
  operational life — not just from the `Turntable-Setup` AP during initial
  setup, as before. No endpoint in this design has authentication (the
  whole premise is a hobby device on a trusted home network), so this isn't
  a new category of exposure, just a wider window for the existing one.
  Restricting `/connect` to AP-mode-only would remove the "reconfigure
  without a factory reset" capability this section exists to provide, which
  isn't worth it for this threat model. Revisit only if this device is ever
  deployed somewhere the LAN itself isn't trusted.

## Physical control & status

- Long-press the LyraT's `REC` button (ESP-ADF `input_key_service`, already
  wired to this board) → erase the NVS `"storage"` config blob and
  `esp_restart()`. Same effect as the web "Save and Restart" flow's reset
  path, but doesn't require network access to recover a device with bad
  Wi-Fi credentials.
- The LyraT's general-purpose LED (driven via the board API) blinks a
  pattern per state: slow = AP/setup mode, fast = connecting, solid =
  streaming.

## OTA updates

- The custom two-OTA-slot partition table (`ota_0`/`ota_1`) was moved into
  the core-hardening pass's audio-pipeline work (see above) — it turned out
  to be needed before this plan even starts, not something to defer here.
- New `ota_manager.c/h`, built on ESP-IDF's built-in `esp_https_ota`
  component (ships with IDF — no new dependency). Manually triggered: user
  pastes a firmware `.bin` URL into the setup web page and submits, or
  (see MQTT below) presses the "Install update" HA button, which re-runs
  the last URL saved via the web form.
- `esp_https_ota` validates the fetched image before switching the boot
  partition, so a bad URL or interrupted download doesn't brick the device —
  it stays on the current firmware.

## Home Assistant / MQTT

New `mqtt_manager.c/h`, using ESP-IDF's built-in `mqtt` component (ships
with IDF). Entirely optional — if the broker field is left blank in setup,
MQTT never starts and streaming is unaffected either way; MQTT connects and
reconnects independently and never blocks the audio pipeline.

- **Config additions** (`app_config_t` + setup web form): MQTT broker URI,
  optional username/password. Device ID for topics/discovery is derived
  from the chip's MAC (e.g. `turntable-a1b2c3`) — no extra field needed.
- **Discovery**: on every broker connect, publish retained Home Assistant
  MQTT Discovery config payloads under
  `homeassistant/<component>/turntable-xxxxxx/<object_id>/config`, so the
  device appears in HA automatically, no YAML.
- **Entities**:

  | Entity | Type | Behavior |
  |---|---|---|
  | Stream URL | `sensor` | State = `http://turntable.local/stream.aac` |
  | Streaming | `binary_sensor` | on = audio pipeline running, off = idle/AP mode |
  | Wi-Fi signal | `sensor` | RSSI |
  | Restart | `button` | → `esp_restart()` |
  | Factory reset | `button` | → same NVS-erase-and-reboot as the physical long-press |
  | Install update | `button` | → `ota_manager` against the last-saved firmware URL |

- Command topics subscribed on connect. State/availability published with a
  Last Will and Testament (retained `offline`) so HA shows the device
  unavailable rather than stale when it drops off MQTT.

## File structure

```
main/
  main.c            (unchanged)
  app_config.h       + input_gain, mqtt fields; - mode
  wifi_manager.c/h    + reconnect, button, LED, STA-mode web UI
  audio_streamer.c/h  - WAV/RTSP branches; + input gain
  ota_manager.c/h     (new)
  mqtt_manager.c/h    (new)
```

No subdirectories/components — matches current project size.

## Error handling summary

- Wi-Fi drop: auto-retry with backoff, never a required manual power cycle.
- Bad/interrupted OTA: `esp_https_ota` validates before switching boot
  partition; failure leaves current firmware running.
- MQTT broker unreachable or unset: feature no-ops, audio streaming
  unaffected; LWT marks the device `offline` in HA once it does connect and
  later drops.
- Bad Wi-Fi credentials with no network to reconfigure over: physical
  long-press recovers without needing connectivity.

## Verification (manual, on-device — no existing test harness)

1. Build + flash; confirm captive portal `Turntable-Setup` AP still comes up
   with no saved config.
2. Submit Wi-Fi + gain + MQTT fields; confirm STA connect, `turntable.local`
   resolves, `/stream.aac` plays through a Cast trigger to a real Nest
   speaker.
3. Kill the AP momentarily; confirm the stream recovers without a reboot.
4. Confirm the device appears in Home Assistant via MQTT discovery with all
   six entities; press each button entity and confirm the corresponding
   action.
5. Long-press the physical `REC` button; confirm NVS is cleared and the
   device returns to AP mode.
6. Host a `.bin` locally, submit its URL via the web form (and again via the
   HA "Install update" button); confirm the device flashes and reboots on
   the new version. Point it at a bad/unreachable URL; confirm it stays on
   the current firmware.
7. Watch the LED through setup → connecting → streaming.
