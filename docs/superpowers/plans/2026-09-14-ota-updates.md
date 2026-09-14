# OTA Updates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the device fetch and flash new firmware from a URL pasted into its web UI, without bricking itself on a bad download.

**Architecture:** Switch from the current single-app partition table to a custom two-slot OTA table sized for this board's 4MB flash. Add a new `ota_manager.c/h` wrapping ESP-IDF's built-in `esp_https_ota`, triggered from a new `/ota` endpoint on the web server the core-hardening plan added.

**Tech Stack:** ESP-IDF's `esp_https_ota` component (built in, no new dependency).

**Spec:** `docs/superpowers/specs/2026-09-14-nest-streaming-hardening-design.md`

**Depends on:** `docs/superpowers/plans/2026-09-14-core-hardening.md` (Task 3's `start_webserver(uint16_t port, bool captive)` and the STA-mode web UI must exist first).

## Global Constraints

- Same as the core-hardening plan: no `idf.py` in this sandbox — every build/flash verification runs on your machine.
- Flash size confirmed as 4MB (`CONFIG_ESPTOOLPY_FLASHSIZE=4MB` after this plan's Task 1; it's currently `2MB` in `sdkconfig`, which Task 1 also fixes since it was never updated to match the real hardware).
- Spec: `docs/superpowers/specs/2026-09-14-nest-streaming-hardening-design.md`

---

### Task 1: Custom two-slot OTA partition table

**Files:**
- Create: `partitions.csv`
- Modify: `sdkconfig`

**Interfaces:**
- Produces: partitions named `ota_0`/`ota_1` plus `otadata`/`nvs`/`phy_init` — consumed implicitly by `esp_https_ota` (Task 2) via the standard IDF OTA partition APIs it calls internally.

- [ ] **Step 1: Create `partitions.csv` at the repo root**

Sized for 4MB flash: two 1.75MB app slots (plenty for an ESP-ADF + Wi-Fi + MQTT + HTTPS + OTA image), leaving headroom below the 4MB ceiling.

```
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
otadata,  data, ota,     0xf000,   0x2000,
phy_init, data, phy,     0x11000,  0x1000,
ota_0,    app,  ota_0,   0x20000,  0x1C0000,
ota_1,    app,  ota_1,   0x1E0000, 0x1C0000,
```

- [ ] **Step 2: Switch `sdkconfig` to the custom table and the real flash size**

Change:
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

- [ ] **Step 3: Build**

Run: `idf.py build`
Expected: clean build. Also run `idf.py partition-table` and confirm both `ota_0`/`ota_1` show and the app binary size printed at the end of `idf.py build` is comfortably under `0x1C0000` (1,835,008) bytes — if it isn't, stop and enlarge the partitions before continuing to Task 2.

- [ ] **Step 4: Commit**

```bash
git add partitions.csv sdkconfig
git commit -m "Switch to a custom two-slot OTA partition table sized for 4MB flash"
```

---

### Task 2: `ota_manager` component

**Files:**
- Create: `main/ota_manager.h`
- Create: `main/ota_manager.c`
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `void ota_manager_install_async(const char *url);` — saves `url` as the last-used OTA URL and installs it; safe to call from an HTTP handler (Task 3 uses this).
  - `void ota_manager_install_last_async(void);` — re-installs whatever URL was last saved; the MQTT plan's "Install update" button calls this.
- Consumes: NVS namespace `"storage"` (shared with `app_config_t`, under its own key `"ota_url"` so it doesn't touch the config blob's layout).

- [ ] **Step 1: Write `main/ota_manager.h`**

```c
#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

// Spawns a task that saves `url` as the last-used OTA URL (NVS namespace
// "storage", key "ota_url") and then downloads and flashes it. Safe to call
// from an HTTP handler or event callback -- it never blocks the caller.
// On success the device reboots into the new firmware; on failure it logs
// the error and keeps running the current firmware.
void ota_manager_install_async(const char *url);

// Spawns a task that re-installs the last URL saved via
// ota_manager_install_async(). No-ops (logs a warning) if none was saved yet.
void ota_manager_install_last_async(void);

#endif // OTA_MANAGER_H
```

- [ ] **Step 2: Write `main/ota_manager.c`**

```c
/**
 * @file ota_manager.c
 * @brief Fetches and flashes firmware over HTTP(S) via esp_https_ota.
 */

#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <esp_https_ota.h>
#include <esp_http_client.h>
#include <nvs_flash.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ota_manager.h"

static const char *TAG = "OTA_MANAGER";
#define OTA_URL_NVS_KEY "ota_url"
#define OTA_URL_MAX_LEN 255

typedef struct {
    char url[OTA_URL_MAX_LEN + 1];
} ota_task_arg_t;

static void save_url(const char *url)
{
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_str(nvs, OTA_URL_NVS_KEY, url);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void install(const char *url)
{
    ESP_LOGI(TAG, "Starting OTA from %s", url);
    esp_http_client_config_t http_cfg = {
        .url = url,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    // esp_https_ota() only switches the boot partition on a fully verified
    // success; a bad URL or interrupted download returns an error here and
    // leaves the currently-running firmware untouched.
    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA succeeded, rebooting");
        esp_restart();
    }
    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
}

static void install_task(void *arg)
{
    ota_task_arg_t *a = (ota_task_arg_t *)arg;
    install(a->url);
    free(a);
    vTaskDelete(NULL);
}

void ota_manager_install_async(const char *url)
{
    save_url(url);
    ota_task_arg_t *a = malloc(sizeof(ota_task_arg_t));
    if (!a) {
        ESP_LOGE(TAG, "OOM starting OTA task");
        return;
    }
    strncpy(a->url, url, OTA_URL_MAX_LEN);
    a->url[OTA_URL_MAX_LEN] = '\0';
    xTaskCreate(install_task, "ota_install", 8192, a, 5, NULL);
}

static void install_last_task(void *arg)
{
    nvs_handle_t nvs;
    char url[OTA_URL_MAX_LEN + 1] = {0};
    size_t len = sizeof(url);
    if (nvs_open("storage", NVS_READONLY, &nvs) == ESP_OK) {
        esp_err_t err = nvs_get_str(nvs, OTA_URL_NVS_KEY, url, &len);
        nvs_close(nvs);
        if (err == ESP_OK && url[0] != '\0') {
            install(url);
        } else {
            ESP_LOGW(TAG, "No saved OTA URL to install");
        }
    }
    vTaskDelete(NULL);
}

void ota_manager_install_last_async(void)
{
    xTaskCreate(install_last_task, "ota_install_last", 8192, NULL, 5, NULL);
}
```

- [ ] **Step 3: Add the new source file and its components to `main/CMakeLists.txt`**

```
idf_component_register(SRCS "main.c" "wifi_manager.c" "audio_streamer.c" "ota_manager.c"
                     INCLUDE_DIRS "."
                     REQUIRES nvs_flash esp_wifi esp_event log lwip esp_http_server esp_netif mdns
                              audio_pipeline esp_peripherals audio_hal driver
                              esp_https_ota esp_http_client
)
```

- [ ] **Step 4: Build**

Run: `idf.py build`
Expected: clean build.

- [ ] **Step 5: Commit**

```bash
git add main/ota_manager.h main/ota_manager.c main/CMakeLists.txt
git commit -m "Add ota_manager: async firmware install via esp_https_ota"
```

---

### Task 3: Wire OTA into the web UI

**Files:**
- Modify: `main/wifi_manager.c`

**Interfaces:**
- Consumes: `ota_manager_install_async(const char *url)` (Task 2).
- Consumes/modifies: `root_get_handler`, `start_webserver` (from the core-hardening plan's Task 3).

- [ ] **Step 1: Add `#include "ota_manager.h"`** near the other local includes.

- [ ] **Step 2: Add the firmware-URL field to the setup page**

Replace `root_get_handler`'s `snprintf` call in full (same as the core-hardening plan's Task 3 version, with a Firmware Update section added before `</body></html>`):

```c
static esp_err_t root_get_handler(httpd_req_t *req) {
    char status[160] = "";
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        snprintf(status, sizeof(status),
                 "<p>Status: streaming, connected to %s (RSSI %d dBm).<br>Stream: "
                 "<a href=\"http://turntable.local/stream.mp3\">http://turntable.local/stream.mp3</a></p>",
                 (char *)ap_info.ssid, ap_info.rssi);
    }
    char resp[2560];
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
        "<h2>Stream</h2><label for=\"bitrate\">MP3 Bitrate:</label><br>"
        "<select name=\"bitrate\" id=\"bitrate\"><option value=\"128000\">128 kbps</option>"
        "<option value=\"192000\">192 kbps</option><option value=\"256000\" selected>256 kbps</option>"
        "<option value=\"320000\">320 kbps</option></select><br><br>"
        "<label for=\"gain\">Line-in Gain:</label><br><select name=\"gain\" id=\"gain\">"
        "<option value=\"0\">0 dB</option><option value=\"3\">3 dB</option><option value=\"6\">6 dB</option>"
        "<option value=\"9\">9 dB</option><option value=\"12\" selected>12 dB</option>"
        "<option value=\"15\">15 dB</option><option value=\"18\">18 dB</option>"
        "<option value=\"21\">21 dB</option><option value=\"24\">24 dB</option></select><br><br>"
        "<input type=\"submit\" value=\"Save and Restart\"></form>"
        "<h2>Firmware Update</h2><form action=\"/ota\" method=\"post\">"
        "<input type=\"text\" name=\"url\" placeholder=\"https://.../firmware.bin\"><br><br>"
        "<input type=\"submit\" value=\"Install Update\"></form></body></html>",
        status);
    httpd_resp_send(req, resp, n);
    return ESP_OK;
}
```

- [ ] **Step 3: Add the `/ota` POST handler**

```c
static esp_err_t ota_post_handler(httpd_req_t *req) {
    char buf[300];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    char url[256];
    if (httpd_query_key_value(buf, "url", url, sizeof(url)) == ESP_OK && strlen(url) > 0) {
        ESP_LOGI(TAG, "OTA requested: %s", url);
        httpd_resp_send(req, "<h1>Installing update. Device will reboot if it succeeds.</h1>", -1);
        ota_manager_install_async(url);
    } else {
        httpd_resp_send(req, "<h1>Error: missing firmware url.</h1>", -1);
    }
    return ESP_OK;
}
```

- [ ] **Step 4: Register it and raise `max_uri_handlers`**

In `start_webserver`, change `config.max_uri_handlers = 4;` to `config.max_uri_handlers = 8;` (root + connect + ota + the AP-only wildcard = 4 already at the old ceiling; 8 leaves headroom), and add, alongside the existing `root_uri`/`connect_uri` registrations:

```c
        httpd_uri_t ota_uri = {.uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler};
        httpd_register_uri_handler(server, &ota_uri);
```

- [ ] **Step 5: Build**

Run: `idf.py build`
Expected: clean build.

- [ ] **Step 6: Manual verify**

Spec step 6: host a `.bin` locally (e.g. `python3 -m http.server` in your `build/` directory) and submit its URL via the web form; confirm the device downloads, flashes, and reboots on the new version (check the log banner or a version string you bump for the test). Then submit an unreachable URL and confirm the device logs an OTA failure and keeps running instead of rebooting.

- [ ] **Step 7: Commit**

```bash
git add main/wifi_manager.c
git commit -m "Add /ota endpoint and firmware-URL field to the setup web UI"
```
