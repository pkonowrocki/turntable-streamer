# Turntable Streamer

Turns a turntable (or any line-in audio source) plugged into an ESP32 Audio Kit
V2.2 (Ai-Thinker ESP32-A1S) board into a live network audio stream — playable
on a Google Nest speaker, a computer, a phone, or anything else that can open
an HTTP URL.

## What you need

- An ESP32 Audio Kit V2.2 board (Ai-Thinker ESP32-A1S module, ES8388 codec "variant 5" —
  the common AliExpress version), flashed with this firmware
- A turntable (or other line-in audio source) connected to the board's line-in jack
- Your home Wi-Fi network

## First-time setup

1. Power on the board. With no saved Wi-Fi config yet, it starts its own open
   Wi-Fi network called **`Turntable-Setup`**.
2. On your phone or laptop, connect to `Turntable-Setup` (no password).
   - Phones commonly auto-disconnect from Wi-Fi networks that have "no
     internet" — this is normal phone/OS behavior, not necessarily a problem
     with the board. If that happens: choose "stay connected" / "use without
     internet" if your phone offers it, or connect from a **laptop instead**
     (laptops rarely do this). If your phone still drops the connection, open
     a browser and go to `http://192.168.4.1/` immediately after connecting,
     before it has a chance to auto-disconnect.
3. A setup page should open automatically (captive portal). If it doesn't, go
   to `http://192.168.4.1/` manually. Fill in:
   - **WiFi SSID / Password** — your home network's credentials
   - **AAC Bitrate** — audio quality; 256 kbps is a good default
     (128/192/256/320 kbps available)
   - **Line-in Gain** — how hot your turntable's output is; start at 12 dB and
     raise it if the stream is too quiet, lower it if it sounds distorted/clipped
4. Submit. The board saves your settings and reboots, joining your home Wi-Fi.

## Once it's on your Wi-Fi

- The live stream is at: **`http://turntable.local/stream.aac`**
  - If `turntable.local` doesn't resolve on your network, find the board's IP
    in your router's device list and use `http://<ip>/stream.aac` instead.
- To change settings later without a full reset: visit
  `http://turntable.local:8080/` from a device on the same Wi-Fi network. It
  shows connection status and lets you resubmit Wi-Fi/bitrate/gain.
- To wipe the saved config and go back to the `Turntable-Setup` AP (e.g. wrong
  Wi-Fi password, moved to a new network): hold the board's **REC** button for
  3+ seconds.
- The board's status LED: slow blink = setup mode, fast blink = connecting to
  Wi-Fi, solid = streaming.

## Playing it on a Google Nest / Cast speaker

This firmware *serves* the audio stream — it doesn't cast it anywhere itself.
To play it on a Nest speaker, cast the stream URL to it from something else on
your network:

- **Google Home app** — cast a compatible media app pointed at
  `http://turntable.local/stream.aac`
- **Home Assistant** — add a `media_player` entity pointed at that URL, or a
  script/automation that calls `media_player.play_media` with it
- Any Cast-sender tool (e.g. Python's `pychromecast`) can load that URL onto a
  Cast device directly

Expect roughly 2–5 seconds of latency once playing — that's normal Cast
live-stream buffering, not a bug.

## Building from source

See [`CLAUDE.md`](CLAUDE.md) for the toolchain/build details, and
[`docs/superpowers/`](docs/superpowers/) for the design spec and implementation
plans this firmware was built from.
