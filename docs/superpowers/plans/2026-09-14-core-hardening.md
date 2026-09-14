# Core Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Simplify the audio pipeline to the one codec/mode Nest/Cast can actually play, and harden the firmware (Wi-Fi reconnect, a real DNS bug, a physical recovery button, LED status) — the foundation the OTA and MQTT plans build on.

**Architecture:** No new files. `app_config.h` drops the multi-mode struct down to the fields AAC-only streaming needs; `audio_streamer.c` keeps only the AAC→HTTP branch; `wifi_manager.c` gains reconnect logic, a bug fix, a second (STA-mode) instance of its existing web server, and board-control init (button + LED) shared via one `esp_periph_set`.

**Tech Stack:** ESP-IDF 5.3.3 (target `esp32`), ESP-ADF v2.7, board `ESP-LyraT V4.3` (`CONFIG_ESP_LYRAT_V4_3_BOARD=y`).

**Spec:** `docs/superpowers/specs/2026-09-14-nest-streaming-hardening-design.md`

## Global Constraints

- Toolchain is installed at `~/esp/esp-idf` (v5.3.3) and `~/esp/esp-adf` (v2.7); `IDF_PATH`/`ADF_PATH` are set as persistent user env vars, but the toolchain's own PATH additions (compiler, cmake, ninja) are **not** persistent — every build command must first dot-source `~/esp/esp-idf/export.ps1` (PowerShell) in the same invocation, e.g.: `. "$env:USERPROFILE\esp\esp-idf\export.ps1"; $env:ADF_PATH = "$env:USERPROFILE\esp\esp-adf"; idf.py build`.
- No existing test framework (confirmed in `CLAUDE.md`). Verification per task is `idf.py build` (must compile clean) plus the referenced step from the spec's "Verification" section (manual, on-device — a human runs the on-device parts, not the agent).
- **ESP-ADF has no MP3 encoder** (confirmed against the local v2.7 checkout — see the spec's "Codec: AAC, not MP3" section). This plan uses AAC (`aac_encoder.h`, component `esp-adf-libs`) throughout. Don't reintroduce `mp3_encoder.h`/`rtsp_stream.h` — neither exists in ESP-ADF.
- **ESP-ADF's `http_stream` is an HTTP client, not a server** (confirmed by reading `http_stream.c` and ESP-ADF's own `examples/recorder/pipeline_raw_http` — see the spec's "Serving mechanism" section). Don't use `http_stream` as the thing clients connect to. The pipeline ends in a `raw_stream` reader element instead; a `GET /stream.aac` handler on the project's own `esp_http_server` pulls bytes out via `raw_stream_read()`. Neither `http_stream_set_uri` nor `audio_element_set_uri` makes `http_stream` serve anything — don't reach for either here.
- ESP-ADF component names verified against the local `~/esp/esp-adf` v2.7 checkout (not just headers — actual `idf_component_register`/`register_component` component names, since IDF resolves `REQUIRES` by component name, not header path): `i2s_stream.h`/`raw_stream.h` → component **`audio_stream`**; `aac_encoder.h`/`wav_encoder.h` → component **`esp-adf-libs`**; `es8388.h` → component **`audio_hal`**. `esp_http_server` is ESP-IDF's own component (already in `REQUIRES`, already used by `wifi_manager.c`'s setup UI). If your `$ADF_PATH` checkout is a materially different version, `grep` its `components/*/CMakeLists.txt` for these before compiling.
- ESP-ADF symbol names for the button/LED work (`periph_button_cfg_t`, `periph_button_event_id_t`, `GREEN_LED_GPIO`, `BUTTON_REC_ID`/`GPIO_NUM_36`) were verified against `github.com/espressif/esp-adf` commit `49f80aa` (same v2.7 release).
- Board confirmed: LyraT V4.3, 4MB flash, ESP-IDF 5.3.3, target `esp32` (all from `sdkconfig`).
- **Flash size and the OTA partition table move into this plan's Task 1** (originally scoped to the OTA plan). The checked-in `CONFIG_ESPTOOLPY_FLASHSIZE=2MB` was already wrong (real hardware is 4MB, confirmed), and the linked binary — ESP-ADF + Wi-Fi + HTTP server, even before MQTT/OTA/button/LED code — doesn't fit IDF's default single-app partition sizing regardless of codec choice. Since the file has to be touched now anyway, Task 1 goes straight to the final two-OTA-slot shape the OTA plan needs, instead of a temporary single-app table that gets redone later.
- Root `CMakeLists.txt` had a real bug independent of this plan: `project.cmake` was `include()`-d twice and in the wrong order relative to ESP-ADF's own `CMakeLists.txt`, which made `project()` recurse into itself and crash `cmake`. Already fixed (verified against ESP-ADF's own example projects) to:
  ```
  cmake_minimum_required(VERSION 3.5)

  include($ENV{ADF_PATH}/CMakeLists.txt)
  include($ENV{IDF_PATH}/tools/cmake/project.cmake)

  project(turntable_streamer)
  ```
  Don't touch this file in this plan unless something regresses it.

---

### Task 1: AAC-only pipeline, input gain, CPU headroom

**Files:**
- Modify: `main/app_config.h`
- Modify: `main/audio_streamer.c`
- Modify: `main/wifi_manager.c:100-132` (`root_get_handler`, `connect_post_handler`)
- Modify: `main/CMakeLists.txt`
- Modify: `sdkconfig`
- Create: `partitions.csv`

**Interfaces:**
- Produces: `app_config_t { char ssid[32]; char password[64]; int bitrate; int input_gain_db; }` — the shape every later task (and the OTA/MQTT plans) reads/writes via NVS key `CONFIG_NVS_KEY` in namespace `"storage"`.
- Produces: `audio_streamer_start(const app_config_t *config)` — signature unchanged, behavior now AAC-only, served via its own `esp_http_server` on port 80 (separate `httpd_handle_t` from `wifi_manager.c`'s setup-UI server — see Task 3, which puts that one on port 8080 in STA mode).
- Produces: `partitions.csv` with named partitions `nvs`/`otadata`/`phy_init`/`ota_0`/`ota_1` — the OTA plan's Task 2 (`esp_https_ota`) targets `ota_0`/`ota_1` by name; don't rename them later.

- [ ] **Step 1: Rewrite `app_config.h`**

```c
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define CONFIG_NVS_KEY "app_config"

typedef struct {
    char ssid[32];
    char password[64];
    int bitrate;        // AAC encoder bitrate in bps (128000/192000/256000/320000)
    int input_gain_db;  // ES8388 line-in gain: 0,3,6,9,12,15,18,21, or 24 (dB)
} app_config_t;

#endif // APP_CONFIG_H
```

- [ ] **Step 2: Rewrite `audio_streamer.c` to the single AAC branch, served over the device's own `esp_http_server`**

ESP-ADF has no MP3 encoder (see Global Constraints) — this uses `aac_encoder.h`
(`aac_encoder_cfg_t`/`aac_encoder_init()`, component `esp-adf-libs`), stereo
44.1kHz, whose valid bitrate range comfortably covers all four dropdown
options (128k/192k/256k/320k).

ESP-ADF's `http_stream` is an HTTP *client* (see Global Constraints) — it
cannot serve `/stream.aac` to anything. Instead the pipeline ends in a
`raw_stream` reader element, and a plain `esp_http_server` instance (ESP-IDF's
own component, not ESP-ADF's) exposes `GET /stream.aac`, looping
`raw_stream_read()` → `httpd_resp_send_chunk()` for whichever client is
connected:

```c
/**
 * @file audio_streamer.c
 * @brief AAC audio streamer: I2S line-in -> AAC encoder -> raw_stream,
 *        served at GET /stream.aac by this file's own esp_http_server.
 */

#include <esp_log.h>
#include <esp_http_server.h>
#include <mdns.h>

#include "audio_pipeline.h"
#include "audio_element.h"
#include "board.h"
#include "i2s_stream.h"
#include "aac_encoder.h"
#include "raw_stream.h"
#include "es8388.h"

#include "audio_streamer.h"

static const char *TAG = "AUDIO_STREAMER";

// ponytail: raw_stream's ring buffer is a single shared FIFO, so two
// simultaneous GET clients would split one stream's bytes rather than each
// getting the full thing. Fine for a single-listener hobby stream (Cast
// normally has one active receiver anyway) -- add per-client fan-out only if
// real multi-room playback is ever needed.
static audio_element_handle_t s_raw_reader;

static esp_err_t stream_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "audio/aac");
    char buf[1024];
    while (1) {
        int len = raw_stream_read(s_raw_reader, buf, sizeof(buf));
        if (len <= 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, buf, len) != ESP_OK) {
            break; // client disconnected
        }
    }
    return ESP_OK;
}

static void start_stream_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.server_port = 80;
    if (httpd_start(&server, &httpd_cfg) == ESP_OK) {
        httpd_uri_t stream_uri = {.uri = "/stream.aac", .method = HTTP_GET, .handler = stream_get_handler};
        httpd_register_uri_handler(server, &stream_uri);
    }
}

void audio_streamer_start(const app_config_t *config)
{
    ESP_LOGI(TAG, "Starting streamer. Bitrate: %d bps, input gain: %d dB", config->bitrate, config->input_gain_db);

    ESP_LOGI(TAG, "Initializing audio board and codec...");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);
    // ponytail: es_mic_gain_t's values are literally the dB step (0,3,...,24 -
    // verified against esp-adf's esxxx_common.h), so a direct cast replaces a lookup table.
    es8388_set_mic_gain((es_mic_gain_t)config->input_gain_db);

    ESP_LOGI(TAG, "Creating audio pipeline...");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    audio_pipeline_handle_t pipeline = audio_pipeline_init(&pipeline_cfg);

    ESP_LOGI(TAG, "Creating I2S stream reader (audio input)...");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_READER;
    audio_element_handle_t i2s_stream_reader = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "Configuring AAC encoder...");
    aac_encoder_cfg_t aac_cfg = DEFAULT_AAC_ENCODER_CONFIG();
    aac_cfg.bitrate = config->bitrate;
    audio_element_handle_t encoder = aac_encoder_init(&aac_cfg);

    ESP_LOGI(TAG, "Configuring raw_stream output tap...");
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;
    s_raw_reader = raw_stream_init(&raw_cfg);

    ESP_LOGI(TAG, "Linking elements: i2s -> aac -> raw");
    audio_pipeline_register(pipeline, i2s_stream_reader, "i2s");
    audio_pipeline_register(pipeline, encoder, "aac");
    audio_pipeline_register(pipeline, s_raw_reader, "raw");
    const char *link[3] = {"i2s", "aac", "raw"};
    audio_pipeline_link(pipeline, &link[0], 3);

    ESP_LOGI(TAG, "Starting HTTP stream server on port 80...");
    start_stream_server();

    ESP_LOGI(TAG, "Initializing mDNS service...");
    mdns_init();
    mdns_hostname_set("turntable");
    mdns_instance_name_set("Turntable Audio Streamer");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS initialized. Stream available at: http://turntable.local/stream.aac");

    ESP_LOGI(TAG, "Starting audio pipeline...");
    audio_pipeline_run(pipeline);
    ESP_LOGI(TAG, "Audio pipeline is now running.");
}
```

- [ ] **Step 3: Update the setup form and its parser in `wifi_manager.c`**

Replace the `root_get_handler` body's `resp_str` literal (drop the mode radios/JS, add a gain dropdown) — full replacement text:

```c
    const char* resp_str = (const char*) R"rawliteral(
    <!DOCTYPE html><html><head><title>Turntable Wi-Fi Setup</title><meta name="viewport" content="width=device-width, initial-scale=1"><style>body{font-family:sans-serif; background-color:#282c34; color:#fff; padding:20px;} h1,h2{color:#61afef;} input,select{padding:10px; width:calc(100% - 22px); border-radius:5px; border:1px solid #61afef; background-color:#3c4049; color:#fff;} input[type="submit"]{background-color:#98c379; color:#282c34; font-weight:bold; cursor:pointer; width:100%;}</style></head>
    <body><h1>Turntable Wi-Fi Setup</h1><form action="/connect" method="post"><h2>Wi-Fi Credentials</h2><input type="text" name="ssid" placeholder="WiFi SSID" required><br><br><input type="password" name="password" placeholder="Password"><br><br><h2>Stream</h2><label for="bitrate">AAC Bitrate:</label><br><select name="bitrate" id="bitrate"><option value="128000">128 kbps</option><option value="192000">192 kbps</option><option value="256000" selected>256 kbps</option><option value="320000">320 kbps</option></select><br><br><label for="gain">Line-in Gain:</label><br><select name="gain" id="gain"><option value="0">0 dB</option><option value="3">3 dB</option><option value="6">6 dB</option><option value="9">9 dB</option><option value="12" selected>12 dB</option><option value="15">15 dB</option><option value="18">18 dB</option><option value="21">21 dB</option><option value="24">24 dB</option></select><br><br><input type="submit" value="Save and Restart"></form></body></html>)rawliteral";
```

Replace `connect_post_handler` in full:

```c
static esp_err_t connect_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    app_config_t cfg = {0};
    char bitrate_str[16], gain_str[8];
    if (httpd_query_key_value(buf, "ssid", cfg.ssid, sizeof(cfg.ssid)) == ESP_OK &&
        httpd_query_key_value(buf, "password", cfg.password, sizeof(cfg.password)) == ESP_OK) {
        cfg.bitrate = (httpd_query_key_value(buf, "bitrate", bitrate_str, sizeof(bitrate_str)) == ESP_OK) ? atoi(bitrate_str) : 256000;
        int gain = (httpd_query_key_value(buf, "gain", gain_str, sizeof(gain_str)) == ESP_OK) ? atoi(gain_str) : 12;
        // Clamp untrusted form input to a valid ES8388 mic-gain step (0-24dB, multiples of 3).
        if (gain < 0) gain = 0;
        if (gain > 24) gain = 24;
        cfg.input_gain_db = (gain / 3) * 3;
        ESP_LOGI(TAG, "Saving config: SSID=%s, Bitrate=%d, Gain=%ddB", cfg.ssid, cfg.bitrate, cfg.input_gain_db);
        nvs_handle_t nvs;
        nvs_open("storage", NVS_READWRITE, &nvs);
        nvs_set_blob(nvs, CONFIG_NVS_KEY, &cfg, sizeof(app_config_t));
        nvs_commit(nvs);
        nvs_close(nvs);
        httpd_resp_send(req, "<h1>Config Saved! Restarting...</h1>", -1);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else { httpd_resp_send(req, "<h1>Error: Incomplete data.</h1>", -1); }
    return ESP_OK;
}
```

- [ ] **Step 4: Fix `main/CMakeLists.txt` REQUIRES**

The original REQUIRES list never actually covered the headers `audio_streamer.c`
includes (a pre-existing bug — `i2s_stream.h`/`raw_stream.h`/`aac_encoder.h`/
`es8388.h` were never resolvable). Per Global Constraints: `i2s_stream.h`/
`raw_stream.h` → component `audio_stream`; `aac_encoder.h` → component
`esp-adf-libs`; `es8388.h` → component `audio_hal`. `esp_http_server` is
already in this list. Update:

```
idf_component_register(SRCS "main.c" "wifi_manager.c" "audio_streamer.c"
                     INCLUDE_DIRS "."
                     REQUIRES nvs_flash esp_wifi esp_event log lwip esp_http_server esp_netif mdns
                              audio_pipeline esp_peripherals audio_hal audio_stream esp-adf-libs
)
```

- [ ] **Step 5: Bump CPU clock in `sdkconfig`**

Change these three lines:
```
# CONFIG_ESP32_DEFAULT_CPU_FREQ_160 is not set
CONFIG_ESP32_DEFAULT_CPU_FREQ_240=y
# CONFIG_ESP32_DEFAULT_CPU_FREQ_240 is not set   <- was this; becomes the line above
CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ=240
```
i.e. `CONFIG_ESP32_DEFAULT_CPU_FREQ_160=y` → unset, `# CONFIG_ESP32_DEFAULT_CPU_FREQ_240 is not set` → `CONFIG_ESP32_DEFAULT_CPU_FREQ_240=y`, `CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ=160` → `=240`.

- [ ] **Step 6: Custom two-OTA-slot partition table, corrected flash size**

The checked-in `CONFIG_ESPTOOLPY_FLASHSIZE=2MB` was already wrong (real hardware
is 4MB, confirmed), and IDF's default single-app partition sizing doesn't fit
this binary regardless of codec. Fix both now, in the final shape the OTA plan
needs, rather than a temporary table that gets redone later — two 1.75MB app
slots comfortably fit this binary with room to grow across the rest of this
plan plus the OTA/MQTT plans.

Create `partitions.csv` at the repo root:

```
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
otadata,  data, ota,     0xf000,   0x2000,
phy_init, data, phy,     0x11000,  0x1000,
ota_0,    app,  ota_0,   0x20000,  0x1C0000,
ota_1,    app,  ota_1,   0x1E0000, 0x1C0000,
```

In `sdkconfig`, change:
```
CONFIG_PARTITION_TABLE_SINGLE_APP=y
```
to:
```
# CONFIG_PARTITION_TABLE_SINGLE_APP is not set
```

Change:
```
# CONFIG_PARTITION_TABLE_CUSTOM is not set
```
to:
```
CONFIG_PARTITION_TABLE_CUSTOM=y
```

Change `CONFIG_PARTITION_TABLE_FILENAME="partitions_singleapp.csv"` to `CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"` (leave `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"` as-is — it already matches).

Change the flash size block from:
```
# CONFIG_ESPTOOLPY_FLASHSIZE_1MB is not set
CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y
# CONFIG_ESPTOOLPY_FLASHSIZE_4MB is not set
# CONFIG_ESPTOOLPY_FLASHSIZE_8MB is not set
# CONFIG_ESPTOOLPY_FLASHSIZE_16MB is not set
# CONFIG_ESPTOOLPY_FLASHSIZE_32MB is not set
# CONFIG_ESPTOOLPY_FLASHSIZE_64MB is not set
# CONFIG_ESPTOOLPY_FLASHSIZE_128MB is not set
CONFIG_ESPTOOLPY_FLASHSIZE="2MB"
```
to:
```
# CONFIG_ESPTOOLPY_FLASHSIZE_1MB is not set
# CONFIG_ESPTOOLPY_FLASHSIZE_2MB is not set
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
# CONFIG_ESPTOOLPY_FLASHSIZE_8MB is not set
# CONFIG_ESPTOOLPY_FLASHSIZE_16MB is not set
# CONFIG_ESPTOOLPY_FLASHSIZE_32MB is not set
# CONFIG_ESPTOOLPY_FLASHSIZE_64MB is not set
# CONFIG_ESPTOOLPY_FLASHSIZE_128MB is not set
CONFIG_ESPTOOLPY_FLASHSIZE="4MB"
```

- [ ] **Step 7: Build**

Run: `idf.py build`
Expected: clean build, no errors. Also run `idf.py partition-table` and confirm `ota_0`/`ota_1` both show, and the app binary size printed at the end of `idf.py build` is comfortably under `0x1C0000` (1,835,008) bytes. (Run this on your machine — see Global Constraints.)

- [ ] **Step 8: Manual verify**

Spec step 2: flash, submit the form with real Wi-Fi + a bitrate/gain choice, confirm `http://turntable.local/stream.aac` plays and the line-in signal isn't clipped/too quiet — adjust `gain` and resubmit if needed.

- [ ] **Step 9: Commit**

```bash
git add main/app_config.h main/audio_streamer.c main/wifi_manager.c main/CMakeLists.txt sdkconfig partitions.csv
git commit -m "Simplify to AAC-only pipeline served via raw_stream+esp_http_server, add input gain control, bump CPU to 240MHz, fix flash size and add two-OTA-slot partition table"
```

---

### Task 2: Wi-Fi auto-reconnect + DNS parser bounds-check fix

**Files:**
- Modify: `main/wifi_manager.c`

**Interfaces:**
- Consumes: `wifi_event_group`, `WIFI_CONNECTED_BIT` (existing file-scope statics, unchanged).
- Produces: no new symbols consumed elsewhere — internal reliability fix only.

- [ ] **Step 1: Replace the DNS query-parsing logic**

The bug: `sizeof(dns_header)` (`main/wifi_manager.c:69`) takes the size of the *pointer* `dns_header` (4 bytes), not `struct dns_hdr` (12 bytes), so parsing starts 8 bytes into the actual DNS header instead of past it — and the loop has no bound against `rx_buffer`'s actual received length, so a malformed/truncated packet on the open setup AP can walk off the end of the buffer.

Add this above `dns_server_task` (replacing the reliance on `sizeof(dns_header)`):

```c
#define DNS_HEADER_LEN 12  // struct dns_hdr's wire size: id+flags1+flags2+4x uint16 counts

// Finds the end of the first question in a DNS query packet: just past its
// QTYPE+QCLASS fields. Returns NULL if the packet is too short or the name
// field runs off the end of what was actually received (malformed/truncated).
static uint8_t *dns_question_end(uint8_t *rx_buffer, int rx_len) {
    if (rx_len < DNS_HEADER_LEN + 1) return NULL;
    uint8_t *p = rx_buffer + DNS_HEADER_LEN;
    uint8_t *limit = rx_buffer + rx_len;
    while (p < limit && *p != 0) {
        p += (*p + 1);
    }
    if (p >= limit) return NULL;
    p += 1 + 4; // skip the 0x00 name terminator, QTYPE(2), QCLASS(2)
    return (p <= limit) ? p : NULL;
}
```

Replace the body of `dns_server_task`'s `while (1)` loop with:

```c
    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &socklen);
        if (len <= 0) continue;

        uint8_t *query_end = dns_question_end(rx_buffer, len);
        // The answer record we append needs 16 more bytes (pointer+type+class+ttl+len+A);
        // bail rather than overflow rx_buffer.
        if (!query_end || query_end + 16 > rx_buffer + (int)sizeof(rx_buffer)) continue;

        esp_netif_ip_info_t ip_info;
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        esp_netif_get_ip_info(netif, &ip_info);

        struct dns_hdr *dns_header = (struct dns_hdr *)rx_buffer;
        if ((dns_header->flags1 & DNS_FLAG1_RESPONSE) == 0) {
            dns_header->flags1 |= DNS_FLAG1_RESPONSE | DNS_FLAG1_AUTHORATIVE;
            dns_header->numanswers = dns_header->numquestions;
            dns_header->numauthrr = htons(0);
            dns_header->numextrarr = htons(0);
        }

        *query_end++ = 0xC0;
        *query_end++ = 0x0C;
        *query_end++ = 0x00;
        *query_end++ = 0x01; // Type A
        *query_end++ = 0x00;
        *query_end++ = 0x01; // Class IN
        *query_end++ = 0x00;
        *query_end++ = 0x00;
        *query_end++ = 0x00;
        *query_end++ = 0x0A; // TTL 10s
        *query_end++ = 0x00;
        *query_end++ = 0x04; // RDATA length (4 bytes)
        memcpy(query_end, &ip_info.ip.addr, sizeof(ip_info.ip.addr));

        sendto(sock, rx_buffer, (query_end - rx_buffer) + 4, 0, (struct sockaddr *)&source_addr, socklen);
    }
```

(Leave the socket setup above the loop, and the `close(sock); vTaskDelete(NULL);` after it, unchanged.)

- [ ] **Step 2: Add Wi-Fi auto-reconnect**

Add near the top of the file, with the other statics:

```c
static esp_timer_handle_t wifi_reconnect_timer;
static int wifi_retry_count = 0;

// Exponential backoff capped at 30s, in ms. retry_count is 0-based.
static int wifi_backoff_ms(int retry_count) {
    if (retry_count > 8) retry_count = 8; // 200ms << 8 = 51200ms, already past the 30s cap
    int delay = 200 << retry_count;
    return delay > 30000 ? 30000 : delay;
}

static void wifi_reconnect_timer_cb(void *arg) {
    esp_wifi_connect();
}
```

Add `#include "esp_timer.h"` to the top includes.

In `start_sta_mode`, right after `wifi_event_group = xEventGroupCreate();`, add:

```c
    const esp_timer_create_args_t reconnect_timer_args = { .callback = &wifi_reconnect_timer_cb, .name = "wifi_reconnect" };
    esp_timer_create(&reconnect_timer_args, &wifi_reconnect_timer);
```

Update `wifi_event_handler` to add a disconnect case and reset the retry counter on reconnect:

```c
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        int delay_ms = wifi_backoff_ms(wifi_retry_count++);
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying in %d ms", delay_ms);
        esp_timer_start_once(wifi_reconnect_timer, (uint64_t)delay_ms * 1000);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
```

- [ ] **Step 3: Build**

Run: `idf.py build`
Expected: clean build.

- [ ] **Step 4: Manual verify**

Spec step 3: once streaming, turn your Wi-Fi AP off for ~10s and back on; confirm the stream recovers without a reboot (watch the log for the "retrying in..." lines and a fresh "Connected to AP" line).

- [ ] **Step 5: Commit**

```bash
git add main/wifi_manager.c
git commit -m "Add Wi-Fi auto-reconnect with backoff; fix DNS parser pointer-sizeof bug"
```

---

### Task 3: STA-mode web UI (status + reconfigure without a factory reset)

**Files:**
- Modify: `main/wifi_manager.c`

**Interfaces:**
- Produces: `static httpd_handle_t start_webserver(uint16_t port, bool captive)` — new signature. The OTA plan (Task adding the `/ota` endpoint) registers its handler on the server this returns, so its task must apply on top of this one.
- Consumes: `root_get_handler`, `connect_post_handler` (Task 1's versions).

`audio_streamer.c` already runs its own `esp_http_server` instance on port 80 to serve `/stream.aac` once streaming starts (Task 1). The setup/config server can't also bind port 80 in STA mode — it moves to 8080 there, and drops the AP-only captive-portal wildcard redirect.

- [ ] **Step 1: Parameterize `start_webserver` and add a status line**

Replace `start_webserver` in full:

```c
static httpd_handle_t start_webserver(uint16_t port, bool captive) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 4;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
        httpd_register_uri_handler(server, &root_uri);
        httpd_uri_t connect_uri = {.uri = "/connect", .method = HTTP_POST, .handler = connect_post_handler};
        httpd_register_uri_handler(server, &connect_uri);
        if (captive) {
            httpd_uri_t wildcard = {.uri = "/*", .method = HTTP_GET, .handler = root_get_handler};
            httpd_register_uri_handler(server, &wildcard);
        }
    }
    return server;
}
```

Replace `root_get_handler` in full, so it shows Wi-Fi/streaming status when connected (empty status string in AP mode, where `esp_wifi_sta_get_ap_info` fails since there's no STA connection):

```c
static esp_err_t root_get_handler(httpd_req_t *req) {
    char status[160] = "";
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        snprintf(status, sizeof(status),
                 "<p>Status: streaming, connected to %s (RSSI %d dBm).<br>Stream: "
                 "<a href=\"http://turntable.local/stream.aac\">http://turntable.local/stream.aac</a></p>",
                 (char *)ap_info.ssid, ap_info.rssi);
    }
    char resp[2048];
    int n = snprintf(resp, sizeof(resp),
        "<!DOCTYPE html><html><head><title>Turntable Setup</title>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<style>body{font-family:sans-serif;background-color:#282c34;color:#fff;padding:20px;} "
        "h1,h2{color:#61afef;} input,select{padding:10px;width:calc(100%% - 22px);border-radius:5px;"
        "border:1px solid #61afef;background-color:#3c4049;color:#fff;} "
        "input[type=\"submit\"]{background-color:#98c379;color:#282c34;font-weight:bold;cursor:pointer;width:100%%;}"
        "</style></head><body><h1>Turntable Setup</h1>%s"
        "<form action=\"/connect\" method=\"post\"><h2>Wi-Fi Credentials</h2>"
        "<input type=\"text\" name=\"ssid\" placeholder=\"WiFi SSID\" required><br><br>"
        "<input type=\"password\" name=\"password\" placeholder=\"Password\"><br><br>"
        "<h2>Stream</h2><label for=\"bitrate\">AAC Bitrate:</label><br>"
        "<select name=\"bitrate\" id=\"bitrate\"><option value=\"128000\">128 kbps</option>"
        "<option value=\"192000\">192 kbps</option><option value=\"256000\" selected>256 kbps</option>"
        "<option value=\"320000\">320 kbps</option></select><br><br>"
        "<label for=\"gain\">Line-in Gain:</label><br><select name=\"gain\" id=\"gain\">"
        "<option value=\"0\">0 dB</option><option value=\"3\">3 dB</option><option value=\"6\">6 dB</option>"
        "<option value=\"9\">9 dB</option><option value=\"12\" selected>12 dB</option>"
        "<option value=\"15\">15 dB</option><option value=\"18\">18 dB</option>"
        "<option value=\"21\">21 dB</option><option value=\"24\">24 dB</option></select><br><br>"
        "<input type=\"submit\" value=\"Save and Restart\"></form></body></html>",
        status);
    httpd_resp_send(req, resp, n);
    return ESP_OK;
}
```

Note the `%%` — `resp` is built with `snprintf`, so the literal `%` in `calc(100% - 22px)` must be escaped.

- [ ] **Step 2: Update call sites**

In `start_captive_portal`, change `start_webserver();` to `start_webserver(80, true);`.

In `start_sta_mode`, after the existing `audio_streamer_start(config);` line inside the `if (bits & WIFI_CONNECTED_BIT)` block, add:

```c
        start_webserver(8080, false);
```

- [ ] **Step 3: Build**

Run: `idf.py build`
Expected: clean build.

- [ ] **Step 4: Manual verify**

While streaming, visit `http://turntable.local:8080/` from a laptop on the same network; confirm it shows the connected SSID/RSSI and the stream link, and that resubmitting the form (e.g. a new gain value) saves and restarts without needing the physical button or a factory reset.

- [ ] **Step 5: Commit**

```bash
git add main/wifi_manager.c
git commit -m "Run the setup web UI in STA mode too, on port 8080, with status"
```

---

### Task 4: Physical recovery button + status LED

**Files:**
- Modify: `main/wifi_manager.c`
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Consumes: NVS namespace `"storage"` / `CONFIG_NVS_KEY` (Task 1).
- Produces: `led_state` global (`led_state_t`) — set at each mode transition; no other task reads it in this plan.

LyraT V4.3 GPIOs (verified against `esp-adf`'s `audio_board/lyrat_v4_3/board_def.h`): REC button = `GPIO_NUM_36`, general-purpose LED = `GPIO_NUM_22`.

- [ ] **Step 1: Add includes and state near the top of `wifi_manager.c`**

```c
#include "esp_peripherals.h"
#include "periph_button.h"
#include "driver/gpio.h"
#include "esp_timer.h"   // (already added in Task 2 if done first; harmless if duplicated)
```

```c
#define REC_BUTTON_GPIO GPIO_NUM_36  // LyraT V4.3 REC button
#define STATUS_LED_GPIO GPIO_NUM_22  // LyraT V4.3 general-purpose LED

typedef enum { LED_STATE_SETUP, LED_STATE_CONNECTING, LED_STATE_STREAMING } led_state_t;
static volatile led_state_t led_state = LED_STATE_CONNECTING;
```

- [ ] **Step 2: Add the LED task and board-control init**

```c
static void led_task(void *arg) {
    gpio_set_direction(STATUS_LED_GPIO, GPIO_MODE_OUTPUT);
    bool on = false;
    while (1) {
        int blink_ms = (led_state == LED_STATE_SETUP) ? 800
                      : (led_state == LED_STATE_CONNECTING) ? 200
                      : 0; // STREAMING: solid on
        if (blink_ms == 0) {
            gpio_set_level(STATUS_LED_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            on = !on;
            gpio_set_level(STATUS_LED_GPIO, on);
            vTaskDelay(pdMS_TO_TICKS(blink_ms));
        }
    }
}

static esp_err_t periph_callback(audio_event_iface_msg_t *event, void *context) {
    if (event->source_type == PERIPH_ID_BUTTON && (int)event->data == REC_BUTTON_GPIO
        && event->cmd == PERIPH_BUTTON_LONG_PRESSED) {
        ESP_LOGW(TAG, "REC held: erasing saved Wi-Fi config and rebooting to setup mode");
        nvs_handle_t nvs;
        if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_key(nvs, CONFIG_NVS_KEY);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        esp_restart();
    }
    return ESP_OK;
}

static void init_controls(void) {
    xTaskCreate(led_task, "status_led", 2048, NULL, 3, NULL);

    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t periph_set = esp_periph_set_init(&periph_cfg);

    periph_button_cfg_t btn_cfg = {
        .gpio_mask = (1ULL << REC_BUTTON_GPIO),
        .long_press_time_ms = 3000,
    };
    esp_periph_handle_t button_handle = periph_button_init(&btn_cfg);
    esp_periph_start(periph_set, button_handle);
    esp_periph_set_register_callback(periph_set, periph_callback, NULL);
}
```

- [ ] **Step 3: Wire it in and set `led_state` at each transition**

At the top of `wifi_manager_start`, before the NVS read, add:

```c
    init_controls();
```

In `start_captive_portal`, right before `start_dns_server();`, add:

```c
    led_state = LED_STATE_SETUP;
```

In `start_sta_mode`, right after `wifi_event_group = xEventGroupCreate();`, add:

```c
    led_state = LED_STATE_CONNECTING;
```

...and inside the `if (bits & WIFI_CONNECTED_BIT)` block, right before `audio_streamer_start(config);`, add:

```c
        led_state = LED_STATE_STREAMING;
```

- [ ] **Step 4: `driver` component is already linked transitively; confirm it's explicit**

Update `main/CMakeLists.txt`'s REQUIRES line to include `driver`:

```
idf_component_register(SRCS "main.c" "wifi_manager.c" "audio_streamer.c"
                     INCLUDE_DIRS "."
                     REQUIRES nvs_flash esp_wifi esp_event log lwip esp_http_server esp_netif mdns
                              audio_pipeline esp_peripherals audio_hal audio_stream esp-adf-libs driver
)
```

- [ ] **Step 5: Build**

Run: `idf.py build`
Expected: clean build.

- [ ] **Step 6: Manual verify**

Spec steps 5 and 7: watch the LED through setup (slow blink) → connecting (fast blink) → streaming (solid). Then hold REC for 3+ seconds; confirm the device erases its config and returns to the `Turntable-Setup` AP (LED back to slow blink).

- [ ] **Step 7: Commit**

```bash
git add main/wifi_manager.c main/CMakeLists.txt
git commit -m "Add physical factory-reset button (REC long-press) and status LED"
```
