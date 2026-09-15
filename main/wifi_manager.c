#include "wifi_manager.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_http_server.h>
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/prot/dns.h"
#include "audio_streamer.h"
#include "esp_peripherals.h"
#include "periph_button.h"
#include "driver/gpio.h"
#include "ota_manager.h"
#include "mqtt_manager.h"

static const char *TAG = "WIFI_MANAGER";

#define WIFI_AP_SSID "Turntable-Setup"
#define REC_BUTTON_GPIO GPIO_NUM_36  // physically labeled KEY1/MODE on this board (ai-thinker-esp32-a1s), same GPIO as LyraT's REC
#define STATUS_LED_GPIO GPIO_NUM_22  // matches LyraT's general-purpose LED pin

typedef enum { LED_STATE_SETUP, LED_STATE_CONNECTING, LED_STATE_STREAMING } led_state_t;
static volatile led_state_t led_state = LED_STATE_CONNECTING;

static EventGroupHandle_t wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

static esp_timer_handle_t wifi_reconnect_timer;
static int wifi_retry_count = 0;

// --- Deklaracje funkcji wewnętrznych ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void start_sta_mode(const app_config_t *config);
static void start_captive_portal(void);
static void dns_server_task(void *pvParameters);
static bool load_config(app_config_t *cfg);
void start_dns_server(void) { xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL); }

// Exponential backoff capped at 30s, in ms. retry_count is 0-based.
static int wifi_backoff_ms(int retry_count) {
    if (retry_count > 8) retry_count = 8; // 200ms << 8 = 51200ms, already past the 30s cap
    int delay = 200 << retry_count;
    return delay > 30000 ? 30000 : delay;
}

static void wifi_reconnect_timer_cb(void *arg) {
    esp_wifi_connect();
}

// --- Implementacja serwera DNS (POPRAWIONA) ---
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

static void dns_server_task(void *pvParameters) {
    uint8_t rx_buffer[128];
    struct sockaddr_in dest_addr;

    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53); // Standardowy port DNS

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create DNS socket");
        vTaskDelete(NULL);
        return;
    }
    bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

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
    close(sock);
    vTaskDelete(NULL);
}


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

// Builds a <select> of the fixed AAC bitrate choices with the current one marked selected.
static void append_bitrate_options(char *buf, size_t buf_size, int current) {
    static const int choices[] = {128000, 192000, 256000, 320000};
    size_t off = strlen(buf);
    for (size_t i = 0; i < sizeof(choices) / sizeof(choices[0]); i++) {
        off += snprintf(buf + off, buf_size - off, "<option value=\"%d\"%s>%d kbps</option>",
                         choices[i], choices[i] == current ? " selected" : "", choices[i] / 1000);
    }
}

// Builds a <select> of the ES8388's 0-24dB (3dB step) gain choices with the current one selected.
static void append_gain_options(char *buf, size_t buf_size, int current) {
    size_t off = strlen(buf);
    for (int db = 0; db <= 24; db += 3) {
        off += snprintf(buf + off, buf_size - off, "<option value=\"%d\"%s>%d dB</option>",
                         db, db == current ? " selected" : "", db);
    }
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    char status[240] = ""; // 240, not 160 -- 160 was too small for a realistic max SSID+RSSI (found in core-hardening's final review)
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        snprintf(status, sizeof(status),
                 "<p>Status: streaming, connected to %s (RSSI %d dBm).<br>Stream: "
                 "<a href=\"http://turntable.local/stream.aac\">http://turntable.local/stream.aac</a></p>",
                 (char *)ap_info.ssid, ap_info.rssi);
    }
    // Pre-fill Stream settings with what's actually saved, not hardcoded guesses -- and this
    // read is also what lets /settings change gain/bitrate/filter without touching Wi-Fi
    // credentials at all (see settings_post_handler).
    app_config_t current = {0}; // zeroed so mqtt_* stay empty ("disabled") if nothing's saved yet
    if (!load_config(&current)) { // nothing saved yet -- fall back to the same defaults as connect_post_handler
        current.bitrate = 256000;
        current.input_gain_db = 12;
        current.hum_filter_enabled = 0;
    }

    char bitrate_opts[300] = "";
    append_bitrate_options(bitrate_opts, sizeof(bitrate_opts), current.bitrate);
    char gain_opts[400] = "";
    append_gain_options(gain_opts, sizeof(gain_opts), current.input_gain_db);

    char resp[3584]; // was 3072 -- MQTT section adds ~400 bytes of HTML plus up to 128+32 for broker URI/username
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
        "<input type=\"submit\" value=\"Save and Restart\"></form>"
        "<form action=\"/settings\" method=\"post\"><h2>Stream</h2>"
        "<label for=\"bitrate\">AAC Bitrate:</label><br>"
        "<select name=\"bitrate\" id=\"bitrate\">%s</select><br><br>"
        "<label for=\"gain\">Line-in Gain:</label><br><select name=\"gain\" id=\"gain\">%s</select><br><br>"
        "<label><input type=\"checkbox\" name=\"hum_filter\" value=\"1\" style=\"width:auto;\"%s> "
        "Reduce mains hum (cuts low bass, keep off unless you hear a 50/100Hz hum)</label><br><br>"
        "<input type=\"submit\" value=\"Save and Restart\"></form>"
        "<form action=\"/mqtt\" method=\"post\"><h2>MQTT / Home Assistant (optional)</h2>"
        "<input type=\"text\" name=\"mqtt_broker\" value=\"%s\" placeholder=\"mqtt://192.168.1.10:1883 (blank = disabled)\"><br><br>"
        "<input type=\"text\" name=\"mqtt_user\" value=\"%s\" placeholder=\"MQTT username (optional)\"><br><br>"
        "<input type=\"password\" name=\"mqtt_pass\" placeholder=\"MQTT password (leave blank to keep current)\"><br><br>"
        "<input type=\"submit\" value=\"Save and Restart\"></form>"
        "<h2>Firmware Update</h2><form action=\"/ota\" method=\"post\">"
        "<input type=\"text\" name=\"url\" placeholder=\"https://.../firmware.bin\"><br><br>"
        "<input type=\"submit\" value=\"Install Update\"></form></body></html>",
        status, bitrate_opts, gain_opts, current.hum_filter_enabled ? " checked" : "",
        current.mqtt_broker_uri, current.mqtt_username);
    httpd_resp_send(req, resp, n);
    return ESP_OK;
}

// Decodes an application/x-www-form-urlencoded value in place: %XX -> byte, '+' -> space.
static void url_decode_inplace(char *s) {
    char *w = s;
    while (*s) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *w++ = ' ';
            s++;
        } else {
            *w++ = *s++;
        }
    }
    *w = '\0';
}

static esp_err_t ota_post_handler(httpd_req_t *req) {
    char buf[768];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    char url[256];
    if (httpd_query_key_value(buf, "url", url, sizeof(url)) == ESP_OK && strlen(url) > 0) {
        url_decode_inplace(url);
        if (strlen(url) == 0) {
            httpd_resp_send(req, "<h1>Error: missing firmware url.</h1>", -1);
            return ESP_OK;
        }
        ESP_LOGI(TAG, "OTA requested: %s", url);
        httpd_resp_send(req, "<h1>Installing update. Device will reboot if it succeeds.</h1>", -1);
        ota_manager_install_async(url);
    } else {
        httpd_resp_send(req, "<h1>Error: missing firmware url.</h1>", -1);
    }
    return ESP_OK;
}

// Only Wi-Fi credentials -- ssid/password. Preserves whatever stream settings (bitrate,
// gain, hum filter) are already saved, so reconnecting to Wi-Fi never silently resets them.
static esp_err_t connect_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    app_config_t cfg;
    if (!load_config(&cfg)) { // first-time setup (captive portal): no saved settings yet
        cfg.bitrate = 256000;
        cfg.input_gain_db = 12;
        cfg.hum_filter_enabled = 0;
    }
    if (httpd_query_key_value(buf, "ssid", cfg.ssid, sizeof(cfg.ssid)) == ESP_OK &&
        httpd_query_key_value(buf, "password", cfg.password, sizeof(cfg.password)) == ESP_OK) {
        ESP_LOGI(TAG, "Saving Wi-Fi credentials: SSID=%s", cfg.ssid);
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

// Stream settings only -- bitrate/gain/hum filter. Preserves the saved Wi-Fi credentials,
// so changing gain never requires retyping the network password (the bug this fixes).
static esp_err_t settings_post_handler(httpd_req_t *req) {
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    app_config_t cfg;
    if (!load_config(&cfg)) {
        httpd_resp_send(req, "<h1>Error: no Wi-Fi configured yet.</h1>", -1);
        return ESP_OK;
    }
    char bitrate_str[16], gain_str[8];
    int bitrate = (httpd_query_key_value(buf, "bitrate", bitrate_str, sizeof(bitrate_str)) == ESP_OK) ? atoi(bitrate_str) : cfg.bitrate;
    // Clamp untrusted form input to a valid AAC bitrate; anything else could make
    // aac_encoder_init() fail on the next boot after this gets persisted to NVS.
    if (bitrate != 128000 && bitrate != 192000 && bitrate != 256000 && bitrate != 320000) {
        bitrate = 256000;
    }
    cfg.bitrate = bitrate;
    int gain = (httpd_query_key_value(buf, "gain", gain_str, sizeof(gain_str)) == ESP_OK) ? atoi(gain_str) : cfg.input_gain_db;
    // Clamp untrusted form input to a valid ES8388 mic-gain step (0-24dB, multiples of 3).
    if (gain < 0) gain = 0;
    if (gain > 24) gain = 24;
    cfg.input_gain_db = (gain / 3) * 3;
    // A checkbox only appears in the POST body when checked, so its absence means "off".
    cfg.hum_filter_enabled = (httpd_query_key_value(buf, "hum_filter", bitrate_str, sizeof(bitrate_str)) == ESP_OK) ? 1 : 0;
    ESP_LOGI(TAG, "Saving stream settings: Bitrate=%d, Gain=%ddB, HumFilter=%d", cfg.bitrate, cfg.input_gain_db, cfg.hum_filter_enabled);
    nvs_handle_t nvs;
    nvs_open("storage", NVS_READWRITE, &nvs);
    nvs_set_blob(nvs, CONFIG_NVS_KEY, &cfg, sizeof(app_config_t));
    nvs_commit(nvs);
    nvs_close(nvs);
    httpd_resp_send(req, "<h1>Settings Saved! Restarting...</h1>", -1);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

// MQTT/Home Assistant fields only. Preserves the saved Wi-Fi credentials and stream
// settings, same as settings_post_handler. An empty broker field disables MQTT; an empty
// password field means "keep the currently saved password" (it's never echoed back into
// the form, so submitting the form without retyping it would otherwise silently erase it).
static esp_err_t mqtt_post_handler(httpd_req_t *req) {
    char buf[300];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    app_config_t cfg;
    if (!load_config(&cfg)) {
        httpd_resp_send(req, "<h1>Error: no Wi-Fi configured yet.</h1>", -1);
        return ESP_OK;
    }
    if (httpd_query_key_value(buf, "mqtt_broker", cfg.mqtt_broker_uri, sizeof(cfg.mqtt_broker_uri)) == ESP_OK) {
        url_decode_inplace(cfg.mqtt_broker_uri);
    } else {
        cfg.mqtt_broker_uri[0] = '\0';
    }
    if (httpd_query_key_value(buf, "mqtt_user", cfg.mqtt_username, sizeof(cfg.mqtt_username)) == ESP_OK) {
        url_decode_inplace(cfg.mqtt_username);
    } else {
        cfg.mqtt_username[0] = '\0';
    }
    char new_pass[64];
    if (httpd_query_key_value(buf, "mqtt_pass", new_pass, sizeof(new_pass)) == ESP_OK) {
        url_decode_inplace(new_pass);
        if (new_pass[0] != '\0') {
            strncpy(cfg.mqtt_password, new_pass, sizeof(cfg.mqtt_password) - 1);
            cfg.mqtt_password[sizeof(cfg.mqtt_password) - 1] = '\0';
        } // else: field left blank on purpose -- keep the existing saved password
    }
    ESP_LOGI(TAG, "Saving MQTT settings: broker=%s", cfg.mqtt_broker_uri[0] ? cfg.mqtt_broker_uri : "(disabled)");
    nvs_handle_t nvs;
    nvs_open("storage", NVS_READWRITE, &nvs);
    nvs_set_blob(nvs, CONFIG_NVS_KEY, &cfg, sizeof(app_config_t));
    nvs_commit(nvs);
    nvs_close(nvs);
    httpd_resp_send(req, "<h1>MQTT Settings Saved! Restarting...</h1>", -1);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

static httpd_handle_t start_webserver(uint16_t port, bool captive) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1; // avoid colliding with audio_streamer.c's own httpd instance
    config.stack_size = 8192; // root_get_handler's resp[3584]+status[240] locals overflow the 4096-byte default
    config.max_uri_handlers = 8;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed on port %u", port);
        return NULL;
    }
    httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    httpd_register_uri_handler(server, &root_uri);
    httpd_uri_t connect_uri = {.uri = "/connect", .method = HTTP_POST, .handler = connect_post_handler};
    httpd_register_uri_handler(server, &connect_uri);
    httpd_uri_t settings_uri = {.uri = "/settings", .method = HTTP_POST, .handler = settings_post_handler};
    httpd_register_uri_handler(server, &settings_uri);
    httpd_uri_t mqtt_uri = {.uri = "/mqtt", .method = HTTP_POST, .handler = mqtt_post_handler};
    httpd_register_uri_handler(server, &mqtt_uri);
    httpd_uri_t ota_uri = {.uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler};
    httpd_register_uri_handler(server, &ota_uri);
    if (captive) {
        httpd_uri_t wildcard = {.uri = "/*", .method = HTTP_GET, .handler = root_get_handler};
        httpd_register_uri_handler(server, &wildcard);
    }
    return server;
}

// Reads the saved config from NVS. Returns false (cfg left zeroed) if none is saved yet.
static bool load_config(app_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READWRITE, &nvs) != ESP_OK) return false;
    size_t required_size = sizeof(*cfg);
    esp_err_t err = nvs_get_blob(nvs, CONFIG_NVS_KEY, cfg, &required_size);
    nvs_close(nvs);
    return err == ESP_OK;
}

void wifi_manager_start(void) {
    init_controls();
    app_config_t cfg;
    if (load_config(&cfg)) { start_sta_mode(&cfg); }
    else { start_captive_portal(); }
}

static void start_sta_mode(const app_config_t *config) {
    wifi_event_group = xEventGroupCreate();
    led_state = LED_STATE_CONNECTING;
    const esp_timer_create_args_t reconnect_timer_args = { .callback = &wifi_reconnect_timer_cb, .name = "wifi_reconnect" };
    esp_timer_create(&reconnect_timer_args, &wifi_reconnect_timer);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg_wifi = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg_wifi));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    wifi_config_t wifi_config = {0};
    strcpy((char*)wifi_config.sta.ssid, config->ssid);
    strcpy((char*)wifi_config.sta.password, config->password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    // ponytail: default modem-sleep power save lets the radio nap between beacons, which can
    // make the device miss incoming TCP SYNs (connects fine, but new connections time out
    // intermittently) -- this is a streaming device on external power, so there's no battery
    // budget to protect; trade the power savings for reliable reachability.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP. Starting audio streamer.");
        led_state = LED_STATE_STREAMING;
        audio_streamer_start(config);
        start_webserver(8080, false);
        mqtt_manager_start(config);
        mqtt_manager_set_streaming(true);
    }
}

static void start_captive_portal(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &wifi_event_handler, NULL, NULL);
    wifi_config_t wifi_config = { .ap = { .ssid = WIFI_AP_SSID, .ssid_len = strlen(WIFI_AP_SSID), .max_connection = 4, .authmode = WIFI_AUTH_OPEN } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    led_state = LED_STATE_SETUP;
    start_dns_server();
    start_webserver(80, true);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        led_state = LED_STATE_CONNECTING;
        int delay_ms = wifi_backoff_ms(wifi_retry_count++);
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying in %d ms", delay_ms);
        esp_timer_stop(wifi_reconnect_timer); // no-op if not currently active
        esp_err_t timer_err = esp_timer_start_once(wifi_reconnect_timer, (uint64_t)delay_ms * 1000);
        if (timer_err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to schedule Wi-Fi reconnect: %s", esp_err_to_name(timer_err));
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}