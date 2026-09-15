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
#include <esp_netif.h>
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
             "\"manufacturer\":\"DIY\",\"model\":\"ESP32 Audio Kit V2.2 (ESP32-A1S)\"}%s}",
             name, device_id, object_id, state_or_cmd_key, state_or_cmd_topic,
             availability_topic, device_id, extra_json ? extra_json : "");

    esp_mqtt_client_publish(client, discovery_topic, payload, 0, 1, true);
}

// Builds "http://<ip>/stream.aac" from the STA interface's current IP. Google Cast
// devices (and this project's earlier Windows testing) don't reliably resolve
// turntable.local, so Home Assistant needs a real IP to actually cast to a Nest speaker.
static void get_stream_url(char *out, size_t out_len) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(out, out_len, "http://" IPSTR "/stream.aac", IP2STR(&ip_info.ip));
    } else {
        snprintf(out, out_len, "http://turntable.local/stream.aac"); // fallback if IP isn't available yet
    }
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

    char stream_url[48];
    get_stream_url(stream_url, sizeof(stream_url));
    esp_mqtt_client_publish(client, stream_url_t, stream_url, 0, 1, true);
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
