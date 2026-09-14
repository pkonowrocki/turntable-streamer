# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP32 firmware (ESP-IDF + ESP-ADF) that turns a turntable/line-in audio source into a
network stream. On boot it either connects to a saved Wi-Fi network and starts streaming,
or falls back to a captive-portal AP for first-time setup.

## Build

Requires the ESP-IDF toolchain (`IDF_PATH`, target `esp32`, IDF version 5.3.3 per
`dependencies.lock`) and ESP-ADF (`ADF_PATH` — `main/CMakeLists.txt` includes
`$ADF_PATH/CMakeLists.txt` directly, and `main/CMakeLists.txt` links `audio_pipeline`/
`esp_peripherals` from it). With both env vars set and `idf.py` on PATH:

```
idf.py set-target esp32   # first time only
idf.py build
idf.py -p <PORT> flash monitor
```

There is no test suite — verification is build + on-device flash.

## Architecture

Three files in `main/`, wired together by a single config struct (`app_config_t` in
`app_config.h`: SSID, password, `stream_mode_t`, bitrate) persisted to NVS under
namespace `"storage"`, key `CONFIG_NVS_KEY`.

**`main.c`** — inits NVS, calls `wifi_manager_start()`. That's the whole entry point.

**`wifi_manager.c`** — the boot-mode switch:
- On start, tries to read `app_config_t` from NVS.
- **Found** → `start_sta_mode()`: connects to the saved Wi-Fi, and once
  `IP_EVENT_STA_GOT_IP` fires, calls `audio_streamer_start(config)`.
- **Not found** → `start_captive_portal()`: brings up a `Turntable-Setup` open AP, a
  hand-rolled DNS server (`dns_server_task`, answers every query with the AP's own IP —
  captive-portal redirect) on UDP/53, and an HTTP server serving a setup form. Submitting
  the form (`connect_post_handler`) writes `app_config_t` to NVS and calls `esp_restart()`,
  which puts the device into STA mode on the next boot.

**`audio_streamer.c`** — builds and runs one ESP-ADF `audio_pipeline` based on
`config->mode`, always `i2s_stream_reader → encoder → stream_writer`:
- `STREAM_MODE_MP3` — mp3 encoder → `http_stream` at `/stream.mp3`
- `STREAM_MODE_WAV` — wav encoder → `http_stream` at `/stream.wav`
- `STREAM_MODE_RTSP_MP3` — mp3 encoder → `rtsp_stream` on port 554

Advertises itself over mDNS as `turntable.local` (service `_http._tcp` or `_rtsp._tcp`
depending on mode). To add a new stream mode: extend `stream_mode_t` in `app_config.h`,
add a case in the `switch` in `audio_streamer_start`, and add the matching radio option in
`wifi_manager.c`'s `root_get_handler` HTML form.

Note: comments in `wifi_manager.c` and `audio_streamer.c` are in Polish.

## Dependencies

Managed via ESP-IDF's component manager (`main/idf_component.yml`), pinned in
`dependencies.lock` (target `esp32`, IDF `5.3.3`). Don't hand-edit `dependencies.lock`;
change `idf_component.yml` and let `idf.py` regenerate the lock file.
