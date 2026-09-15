# MQTT / Home Assistant Integration Implementation Plan

> **Status: implemented** (commit `acd1801`). Adapted from the plan text below in two ways:
> the web UI split into `/connect` (Wi-Fi), `/settings` (stream), and `/mqtt` (this plan's
> fields) instead of one merged form -- the merged form was itself a bug (any settings change
> required retyping Wi-Fi credentials), fixed in commit `1e0904f` before this plan landed. And
> `app_config_t` also carries `hum_filter_enabled` (unrelated feature, commit `1e0904f`) which
> the struct snippet below doesn't show. The `mqtt_manager.c` code below was used as written.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Make the device appear in Home Assistant automatically (MQTT Discovery), exposing its stream URL, streaming/Wi-Fi status, and restart/factory-reset/install-update buttons.

**Architecture:** New `mqtt_manager.c/h`, built on ESP-IDF's built-in `mqtt` component, entirely optional (no-ops if no broker is configured). Config gains three fields (broker URI, username, password) surfaced in the existing setup web form.

**Tech Stack:** ESP-IDF's `mqtt` component (`esp_mqtt_client`, built in, no new dependency).

**Spec:** `docs/superpowers/specs/2026-09-14-nest-streaming-hardening-design.md`

**Depends on:**
- `docs/superpowers/plans/2026-09-14-core-hardening.md` (the `app_config_t` shape from its Task 1, and the web UI from its Task 3).
- `docs/superpowers/plans/2026-09-14-ota-updates.md` (`ota_manager_install_last_async()`, called by the "Install update" MQTT button).

## Global Constraints

- Toolchain is installed at `~/esp/esp-idf` (v5.3.3) and `~/esp/esp-adf` (v2.7); `IDF_PATH`/`ADF_PATH` are persistent env vars, but the toolchain's own PATH additions are not — dot-source `~/esp/esp-idf/export.ps1` in the same command as every `idf.py` call (see the core-hardening plan's Global Constraints for the exact one-liner). On-device flash/verify steps still need a human at the board.
- ESP-ADF has no MP3 encoder — this project uses AAC throughout (see the spec's "Codec: AAC, not MP3" section and the core-hardening plan). The stream URL this plan publishes to MQTT is `/stream.aac`, not `.mp3`.
- MQTT must never block the audio pipeline: `mqtt_manager_start` returns as soon as the client is created; `esp_mqtt_client` manages its own connect/retry loop in a background task.
- Spec: `docs/superpowers/specs/2026-09-14-nest-streaming-hardening-design.md`

---

### Task 1: MQTT config fields

**Files:**
- Modify: `main/app_config.h`
- Modify: `main/wifi_manager.c`

**Interfaces:**
- Produces: `app_config_t` gains `mqtt_broker_uri[128]`, `mqtt_username[32]`, `mqtt_password[64]` — consumed by `mqtt_manager_start` (Task 2). Empty `mqtt_broker_uri` means "MQTT disabled".

- [x] **Step 1: Extend `app_config_t` in `main/app_config.h`**

```c
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define CONFIG_NVS_KEY "app_config"

typedef struct {
    char ssid[32];
    char password[64];
    int bitrate;              // AAC encoder bitrate in bps (128000/192000/256000/320000)
    int input_gain_db;        // ES8388 line-in gain: 0,3,6,9,12,15,18,21, or 24 (dB)
    char mqtt_broker_uri[128]; // e.g. "mqtt://192.168.1.10:1883"; empty = MQTT disabled
    char mqtt_username[32];
    char mqtt_password[64];
} app_config_t;

#endif // APP_CONFIG_H
```

- [x] **Step 2: Add the MQTT fields to the setup form and its parser**

Replace `root_get_handler` in full (adds an "MQTT / Home Assistant" section before the Firmware Update section from the OTA plan; if that plan hasn't been implemented yet, drop the Firmware Update block and keep the rest):

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
    char resp[3072];
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
        "<h2>MQTT / Home Assistant (optional)</h2>"
        "<input type=\"text\" name=\"mqtt_broker\" placeholder=\"mqtt://192.168.1.10:1883 (blank = disabled)\"><br><br>"
        "<input type=\"text\" name=\"mqtt_user\" placeholder=\"MQTT username (optional)\"><br><br>"
        "<input type=\"password\" name=\"mqtt_pass\" placeholder=\"MQTT password (optional)\"><br><br>"
        "<input type=\"submit\" value=\"Save and Restart\"></form>"
        "<h2>Firmware Update</h2><form action=\"/ota\" method=\"post\">"
        "<input type=\"text\" name=\"url\" placeholder=\"https://.../firmware.bin\"><br><br>"
        "<input type=\"submit\" value=\"Install Update\"></form></body></html>",
        status);
    httpd_resp_send(req, resp, n);
    return ESP_OK;
}
```

Replace `connect_post_handler` in full:

```c
static esp_err_t connect_post_handler(httpd_req_t *req) {
    char buf[512];
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
        // MQTT fields are optional; an empty mqtt_broker_uri means MQTT stays disabled.
        httpd_query_key_value(buf, "mqtt_broker", cfg.mqtt_broker_uri, sizeof(cfg.mqtt_broker_uri));
        httpd_query_key_value(buf, "mqtt_user", cfg.mqtt_username, sizeof(cfg.mqtt_username));
        httpd_query_key_value(buf, "mqtt_pass", cfg.mqtt_password, sizeof(cfg.mqtt_password));
        ESP_LOGI(TAG, "Saving config: SSID=%s, Bitrate=%d, Gain=%ddB, MQTT=%s", cfg.ssid, cfg.bitrate, cfg.input_gain_db,
                 cfg.mqtt_broker_uri[0] ? cfg.mqtt_broker_uri : "(disabled)");
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

(`httpd_query_key_value` returning `ESP_ERR_NOT_FOUND` for the MQTT fields just leaves `cfg.mqtt_*` at its `{0}`-initialized empty string, which is the "disabled" state — no extra handling needed.)

- [x] **Step 3: Build**

Run: `idf.py build`
Expected: clean build.

- [x] **Step 4: Commit**

```bash
git add main/app_config.h main/wifi_manager.c
git commit -m "Add optional MQTT broker fields to app_config_t and the setup form"
```

---

### Task 2: `mqtt_manager` component + Home Assistant discovery

**Files:**
- Create: `main/mqtt_manager.h`
- Create: `main/mqtt_manager.c`
- Modify: `main/CMakeLists.txt`
- Modify: `main/wifi_manager.c`

**Interfaces:**
- Consumes: `app_config_t` (Task 1), `ota_manager_install_last_async()` (OTA plan Task 2).
- Produces: `mqtt_manager_start(const app_config_t *config)`, `mqtt_manager_set_streaming(bool streaming)` — called from `wifi_manager.c`'s `start_sta_mode`.

- [x] **Step 1: Write `main/mqtt_manager.h`**

```c
#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <stdbool.h>
#include "app_config.h"

// Connects to config->mqtt_broker_uri and publishes Home Assistant MQTT
// Discovery for this device (stream URL, streaming/RSSI sensors, and
// restart/factory-reset/install-update buttons). No-ops if
// config->mqtt_broker_uri is empty. Call once, after Wi-Fi is up. Never
// blocks the caller -- esp_mqtt_client manages its own connect/retry loop.
void mqtt_manager_start(const app_config_t *config);

// Publishes the "streaming" binary_sensor's state. No-op if MQTT isn't
// configured/connected yet.
void mqtt_manager_set_streaming(bool streaming);

#endif // MQTT_MANAGER_H
```

- [x] **Step 2: Write `main/mqtt_manager.c`**

```c
/**
 * @file mqtt_manager.c
 * @brief Optional MQTT connection + Home Assistant MQTT Discovery.
 *
 * Entirely optional: no-ops if app_config_t.mqtt_broker_uri is empty.
 * Never blocks the audio pipeline -- esp_mqtt_client owns its own
 * connect/retry loop in a background task.
 */

#include <string.h>
#include <stdio.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <mqtt_client.h>
#include <nvs_flash.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mqtt_manager.h"
#include "ota_manager.h"

static const char *TAG = "MQTT_MANAGER";

static esp_mqtt_client_handle_t client;
static char device_id[24];        // "turntable-a1b2c3"
static char topic_prefix[24];     // "turntable/a1b2c3"
static char availability_topic[40];

static void topic(char *out, size_t out_len, const char *suffix) {
    snprintf(out, out_len, "%s/%s", topic_prefix, suffix);
}

static void publish_discovery(const char *component, const char *object_id, const char *name,
                               const char *state_or_cmd_key, const char *state_or_cmd_topic,
                               const char *extra_json) {
    char discovery_topic[128];
    snprintf(discovery_topic, sizeof(discovery_topic), "homeassistant/%s/%s_%s/config", component, device_id, object_id);

    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"unique_id\":\"%s_%s\",\"%s\":\"%s\","
             "\"availability_topic\":\"%s\","
             "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"Turntable Streamer\","
             "\"manufacturer\":\"DIY\",\"model\":\"ESP32-LyraT\"}%s}",
             name, device_id, object_id, state_or_cmd_key, state_or_cmd_topic,
             availability_topic, device_id, extra_json ? extra_json : "");

    esp_mqtt_client_publish(client, discovery_topic, payload, 0, 1, true);
}

static void publish_all_discovery(void) {
    char stream_url_t[40], streaming_t[40], rssi_t[40];
    char restart_cmd[40], reset_cmd[40], update_cmd[40];
    topic(stream_url_t, sizeof(stream_url_t), "stream_url");
    topic(streaming_t, sizeof(streaming_t), "streaming");
    topic(rssi_t, sizeof(rssi_t), "rssi");
    topic(restart_cmd, sizeof(restart_cmd), "restart/set");
    topic(reset_cmd, sizeof(reset_cmd), "factory_reset/set");
    topic(update_cmd, sizeof(update_cmd), "update/set");

    publish_discovery("sensor", "stream_url", "Stream URL", "state_topic", stream_url_t, NULL);
    publish_discovery("binary_sensor", "streaming", "Streaming", "state_topic", streaming_t,
                       ",\"payload_on\":\"ON\",\"payload_off\":\"OFF\"");
    publish_discovery("sensor", "rssi", "Wi-Fi Signal", "state_topic", rssi_t,
                       ",\"unit_of_measurement\":\"dBm\",\"device_class\":\"signal_strength\"");
    publish_discovery("button", "restart", "Restart", "command_topic", restart_cmd, NULL);
    publish_discovery("button", "factory_reset", "Factory Reset", "command_topic", reset_cmd, NULL);
    publish_discovery("button", "update", "Install Update", "command_topic", update_cmd, NULL);

    esp_mqtt_client_publish(client, stream_url_t, "http://turntable.local/stream.aac", 0, 1, true);
    esp_mqtt_client_publish(client, availability_topic, "online", 0, 1, true);
}

static void factory_reset_and_reboot(void) {
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, CONFIG_NVS_KEY);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    esp_restart();
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    char restart_cmd[40], reset_cmd[40], update_cmd[40];
    topic(restart_cmd, sizeof(restart_cmd), "restart/set");
    topic(reset_cmd, sizeof(reset_cmd), "factory_reset/set");
    topic(update_cmd, sizeof(update_cmd), "update/set");

    if (event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "MQTT connected");
        esp_mqtt_client_subscribe(client, restart_cmd, 1);
        esp_mqtt_client_subscribe(client, reset_cmd, 1);
        esp_mqtt_client_subscribe(client, update_cmd, 1);
        publish_all_discovery();
    } else if (event_id == MQTT_EVENT_DATA) {
        char received_topic[64] = {0};
        int tlen = event->topic_len < (int)sizeof(received_topic) - 1 ? event->topic_len : (int)sizeof(received_topic) - 1;
        memcpy(received_topic, event->topic, tlen);

        if (strcmp(received_topic, restart_cmd) == 0) {
            esp_restart();
        } else if (strcmp(received_topic, reset_cmd) == 0) {
            factory_reset_and_reboot();
        } else if (strcmp(received_topic, update_cmd) == 0) {
            ota_manager_install_last_async();
        }
    }
}

static void rssi_publish_task(void *arg) {
    char rssi_t[40];
    topic(rssi_t, sizeof(rssi_t), "rssi");
    while (1) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", ap_info.rssi);
            esp_mqtt_client_publish(client, rssi_t, buf, 0, 0, true);
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

void mqtt_manager_start(const app_config_t *config) {
    if (config->mqtt_broker_uri[0] == '\0') {
        ESP_LOGI(TAG, "No MQTT broker configured, skipping");
        return;
    }

    uint8_t mac[6];
    char id_hex[8];
    esp_efuse_mac_get_default(mac);
    snprintf(id_hex, sizeof(id_hex), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(device_id, sizeof(device_id), "turntable-%s", id_hex);
    snprintf(topic_prefix, sizeof(topic_prefix), "turntable/%s", id_hex);
    snprintf(availability_topic, sizeof(availability_topic), "%s/availability", topic_prefix);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = config->mqtt_broker_uri,
        .session.last_will.topic = availability_topic,
        .session.last_will.msg = "offline",
        .session.last_will.msg_len = 0,
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
    };
    if (config->mqtt_username[0] != '\0') {
        mqtt_cfg.credentials.username = config->mqtt_username;
        mqtt_cfg.credentials.authentication.password = config->mqtt_password;
    }

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);

    xTaskCreate(rssi_publish_task, "mqtt_rssi", 3072, NULL, 3, NULL);
}

void mqtt_manager_set_streaming(bool streaming) {
    if (!client) return;
    char streaming_t[40];
    topic(streaming_t, sizeof(streaming_t), "streaming");
    esp_mqtt_client_publish(client, streaming_t, streaming ? "ON" : "OFF", 0, 1, true);
}
```

- [x] **Step 3: Add the new source file and `mqtt`/`esp_hw_support` to `main/CMakeLists.txt`**

```
idf_component_register(SRCS "main.c" "wifi_manager.c" "audio_streamer.c" "ota_manager.c" "mqtt_manager.c"
                     INCLUDE_DIRS "."
                     REQUIRES nvs_flash esp_wifi esp_event log lwip esp_http_server esp_netif mdns
                              audio_pipeline esp_peripherals audio_hal audio_stream esp-adf-libs driver
                              esp_https_ota esp_http_client mqtt esp_hw_support
)
```

- [x] **Step 4: Call it from `wifi_manager.c`**

Add `#include "mqtt_manager.h"` near the top.

In `start_sta_mode`, inside the `if (bits & WIFI_CONNECTED_BIT)` block, after the existing `audio_streamer_start(config);` and `start_webserver(8080, false);` lines (add them in this order if the OTA/webserver lines aren't there yet from the other plans), add:

```c
        mqtt_manager_start(config);
        mqtt_manager_set_streaming(true);
```

- [x] **Step 5: Build**

Run: `idf.py build`
Expected: clean build.

- [x] **Step 6: Manual verify**

Spec step 4: point `mqtt_broker` at a real broker your Home Assistant is already connected to, save, and confirm the device shows up in HA (Settings → Devices → MQTT) as "Turntable Streamer" with all six entities. Press each button entity and confirm: Restart reboots the device, Factory Reset returns it to the `Turntable-Setup` AP, Install Update re-runs the last firmware URL saved via the web form. Then stop the device (power off) and confirm HA shows it unavailable within a few seconds (the LWT `offline` message).

- [x] **Step 7: Commit**

```bash
git add main/mqtt_manager.h main/mqtt_manager.c main/CMakeLists.txt main/wifi_manager.c
git commit -m "Add mqtt_manager: Home Assistant MQTT Discovery + control buttons"
```
