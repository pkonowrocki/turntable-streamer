/**
 * @file ota_manager.c
 * @brief Fetches and flashes firmware over HTTP(S) via esp_https_ota.
 */

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <esp_log.h>
#include <esp_https_ota.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_system.h>
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

static atomic_flag ota_busy = ATOMIC_FLAG_INIT;

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
    ESP_LOGI(TAG, "Starting OTA from %s (free heap: %u bytes)", url, (unsigned)esp_get_free_heap_size());
    esp_http_client_config_t http_cfg = {
        .url = url,
        // TLS cert validation for https:// URLs; CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP
        // (enabled in sdkconfig) also permits plain http:// for local testing.
        .crt_bundle_attach = esp_crt_bundle_attach,
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
    atomic_flag_clear(&ota_busy);
    vTaskDelete(NULL);
}

void ota_manager_install_async(const char *url)
{
    if (atomic_flag_test_and_set(&ota_busy)) {
        ESP_LOGW(TAG, "OTA already in progress, ignoring new request");
        return;
    }
    save_url(url);
    ota_task_arg_t *a = malloc(sizeof(ota_task_arg_t));
    if (!a) {
        ESP_LOGE(TAG, "OOM starting OTA task");
        atomic_flag_clear(&ota_busy);
        return;
    }
    strncpy(a->url, url, OTA_URL_MAX_LEN);
    a->url[OTA_URL_MAX_LEN] = '\0';
    if (xTaskCreate(install_task, "ota_install", 8192, a, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        free(a);
        atomic_flag_clear(&ota_busy);
    }
}

static void install_last_task(void *arg)
{
    nvs_handle_t nvs;
    char url[OTA_URL_MAX_LEN + 1] = {0};
    size_t len = sizeof(url);
    esp_err_t open_err = nvs_open("storage", NVS_READONLY, &nvs);
    if (open_err == ESP_OK) {
        esp_err_t err = nvs_get_str(nvs, OTA_URL_NVS_KEY, url, &len);
        nvs_close(nvs);
        if (err == ESP_OK && url[0] != '\0') {
            install(url);
        } else {
            ESP_LOGW(TAG, "No saved OTA URL to install");
        }
    } else {
        ESP_LOGW(TAG, "Could not open NVS to read saved OTA URL: %s", esp_err_to_name(open_err));
    }
    atomic_flag_clear(&ota_busy);
    vTaskDelete(NULL);
}

void ota_manager_install_last_async(void)
{
    if (atomic_flag_test_and_set(&ota_busy)) {
        ESP_LOGW(TAG, "OTA already in progress, ignoring new request");
        return;
    }
    if (xTaskCreate(install_last_task, "ota_install_last", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
        atomic_flag_clear(&ota_busy);
    }
}
