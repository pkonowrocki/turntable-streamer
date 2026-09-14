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

## Audio pipeline

- `app_config_t` (`app_config.h`) drops `stream_mode_t` and `mode`; keeps
  `bitrate` (128/192/256/320 kbps, default 256) and gains an `input_gain`
  field.
- `audio_streamer.c` keeps only the MP3-over-HTTP branch:
  `i2s_stream_reader → mp3_encoder → http_stream` at `/stream.mp3`,
  advertised via mDNS as `turntable.local` / `_http._tcp`.
- I2S reader stays at ADF's default 44.1kHz/16-bit/stereo — matches both
  vinyl's practical bandwidth and MP3's expected input.
- Input gain is set explicitly via the codec's HAL at startup from
  `config->input_gain` instead of relying on power-on defaults, so a hot
  turntable preamp doesn't clip the ADC. This is user-tunable from the web
  UI, not a hardcoded constant — the one piece of this system that a fixed
  software default can't get right for every turntable/preamp pairing.
- `sdkconfig`: `CONFIG_ESP32_DEFAULT_CPU_FREQ` 160 MHz → 240 MHz, for margin
  now that MQTT/OTA/button/LED handling share the core with the encoder.

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
  `http_stream`'s `/stream.mp3`. It serves: Wi-Fi/mode reconfiguration (as
  today), the OTA firmware-URL field, MQTT broker fields, and input gain —
  and a status view (Wi-Fi RSSI, streaming state).

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

- `sdkconfig`: `CONFIG_PARTITION_TABLE_SINGLE_APP` → `PARTITION_TABLE_TWO_OTA`
  (no OTA slot exists today).
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
  | Stream URL | `sensor` | State = `http://turntable.local/stream.mp3` |
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
   resolves, `/stream.mp3` plays through a Cast trigger to a real Nest
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
