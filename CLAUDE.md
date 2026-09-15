# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP32 firmware (ESP-IDF + ESP-ADF) that turns a turntable/line-in audio source into a
network stream, playable by Google Cast devices (e.g. a Nest speaker) or any HTTP
client. On boot it either connects to a saved Wi-Fi network and starts streaming, or
falls back to a captive-portal AP for first-time setup.

## Build

Requires the ESP-IDF toolchain (`IDF_PATH`, target `esp32`, IDF 5.3.3) and ESP-ADF
v2.7 (`ADF_PATH` — root `CMakeLists.txt` includes `$ADF_PATH/CMakeLists.txt`, and
`main/CMakeLists.txt` links `audio_pipeline`/`esp_peripherals`/`audio_hal`/
`audio_stream`/`esp-adf-libs`/`driver` from it). With both env vars set and `idf.py`
on PATH:

```
idf.py build
idf.py -p <PORT> flash monitor
```

There is no test suite — verification is build + on-device flash. See
`docs/superpowers/specs/2026-09-14-nest-streaming-hardening-design.md` for the design
rationale and `docs/superpowers/plans/` for the implementation plans this codebase was
built from — both document real ESP-ADF constraints (e.g. ESP-ADF has no MP3 encoder,
and `http_stream` is an HTTP client, not a server) worth reading before assuming an API
exists.

Flash: this board has 4MB flash with a custom two-OTA-slot partition table
(`partitions.csv`: `nvs`/`otadata`/`phy_init`/`ota_0`/`ota_1`, 1.75MB per app slot).

## Architecture

A single config struct (`app_config_t` in `app_config.h`: SSID, password, AAC
bitrate, input gain) persisted to NVS under namespace `"storage"`, key
`CONFIG_NVS_KEY`.

**`main.c`** — inits NVS, calls `wifi_manager_start()`. That's the whole entry point.

**`wifi_manager.c`** — the boot-mode switch, plus setup UI, Wi-Fi reliability, and
physical controls:
- On start, `init_controls()` sets up the physical REC-button (long-press = factory
  reset: erase the saved config and reboot to the captive portal) and the status LED
  (slow blink = setup mode, fast blink = connecting, solid = streaming), then tries to
  read `app_config_t` from NVS.
- **Found** → `start_sta_mode()`: connects to the saved Wi-Fi (auto-retries with
  exponential backoff, capped at 30s, on disconnect), and once `IP_EVENT_STA_GOT_IP`
  fires, calls `audio_streamer_start(config)`, then starts its own setup/status web
  server on port 8080 (reconfigure without a factory reset, or check status).
- **Not found** → `start_captive_portal()`: brings up a `Turntable-Setup` open AP, a
  hand-rolled DNS server (`dns_server_task`, answers every query with the AP's own IP
  — captive-portal redirect, with bounds-checked parsing against the actual received
  packet length) on UDP/53, and the same setup web server on port 80 (the DNS handler
  and root path also register a wildcard so phones' captive-portal detection lands on
  the setup page). Submitting the form (`connect_post_handler`) writes `app_config_t`
  to NVS and calls `esp_restart()`, which puts the device into STA mode on the next
  boot.
- Neither the setup form's fields nor the physical/web factory-reset paths require
  authentication — this is a hobby device on a trusted home network, not a
  multi-tenant or internet-facing one (see the spec's "Captive portal" section for the
  explicit tradeoff this implies once the setup server is also reachable in STA mode).

**`audio_streamer.c`** — builds and runs one ESP-ADF `audio_pipeline`:
`i2s_stream_reader → aac_encoder → raw_stream`. ESP-ADF has no MP3 encoder at all, and
its `http_stream` element is an HTTP *client*, not a server — so this file runs its
own `esp_http_server` instance on port 80, with a `GET /stream.aac` handler that pulls
encoded bytes out of the pipeline via `raw_stream_read()` and streams them out via
`httpd_resp_send_chunk()`. Only one client gets a coherent stream at a time (the
`raw_stream` ring buffer is a single shared FIFO) — fine for a single-listener hobby
stream, not multi-room. Advertises itself over mDNS as `turntable.local` /
`_http._tcp`. To point a Cast device at it: point Google Home / Home Assistant / a
cast script at `http://turntable.local/stream.aac` — this firmware never speaks the
Cast protocol itself.

Note: comments in `wifi_manager.c` and `audio_streamer.c` are a mix of Polish
(older sections) and English (newer additions) — match whichever convention the
function you're editing already uses.

## Dependencies

Managed via ESP-IDF's component manager (`main/idf_component.yml`), pinned in
`dependencies.lock` (target `esp32`, IDF `5.3.3`). Don't hand-edit
`dependencies.lock`; change `idf_component.yml` and let `idf.py` regenerate the lock
file. ESP-ADF component names don't always match their header's directory name —
check `~/esp/esp-adf/components/<name>/CMakeLists.txt` before adding a new
`REQUIRES` entry.
