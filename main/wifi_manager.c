#include <string.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include <esp_http_server.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/dns.h"

// Nasze pliki nagłówkowe
#include "app_config.h"
#include "wifi_manager.h"
#include "audio_streamer.h"

// --- Definicje i zmienne statyczne ---
static const char *TAG = "WIFI_MANAGER";
#define WIFI_AP_SSID "Turntable-Setup"
#define MAX_STA_CONN 1
#define MAX_RETRY 5

static EventGroupHandle_t wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;
static int s_retry_num = 0;

// --- Deklaracje funkcji wewnętrznych ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void start_captive_portal(void);
static void start_sta_mode(const app_config_t *config);
static esp_err_t root_get_handler(httpd_req_t *req);
static esp_err_t connect_post_handler(httpd_req_t *req);
static httpd_handle_t start_webserver(void);
static void dns_server_task(void *pvParameters);
void start_dns_server(void);

// --- Główna funkcja publiczna modułu ---

void wifi_manager_start(void) {
    app_config_t cfg;
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open("wifi_config", NVS_READONLY, &nvs);
    
    size_t required_size = sizeof(app_config_t);
    if (ret == ESP_OK && nvs_get_blob(nvs, CONFIG_NVS_KEY, &cfg, &required_size) == ESP_OK) {
        nvs_close(nvs);
        ESP_LOGI(TAG, "Found stored config. Starting in STA mode.");
        start_sta_mode(&cfg);
    } else {
        if (ret == ESP_OK) nvs_close(nvs);
        ESP_LOGI(TAG, "No config found or read error. Starting Captive Portal.");
        start_captive_portal();
    }
}

// --- Implementacje trybów pracy ---

static void start_sta_mode(const app_config_t *config) {
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg_wifi = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg_wifi));

    esp_event_handler_instance_t instance_any_id, instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = { .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK } };
    strcpy((char*)wifi_config.sta.ssid, config->ssid);
    strcpy((char*)wifi_config.sta.password, config->password);
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Successfully connected to AP.");
        audio_streamer_start(config);
    } else {
        ESP_LOGE(TAG, "Failed to connect. Restarting in Captive Portal mode.");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
}

static void start_captive_portal(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config_ap = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .password = "",
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_OPEN
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config_ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    start_dns_server();
    start_webserver();
    ESP_LOGI(TAG, "Captive Portal started. Connect to SSID: %s", WIFI_AP_SSID);
}


// --- Handlery zdarzeń i serwera HTTP ---

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry to connect to the AP");
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "Connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    const char* resp_str = (const char*) R"rawliteral(
    <!DOCTYPE html><html><head><title>Turntable Wi-Fi Setup</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>body{font-family:sans-serif, Arial; background-color: #282c34; color: #abb2bf; padding: 20px;} h1,h2{color:#61afef; border-bottom: 2px solid #61afef; padding-bottom: 10px;} input,select{width: calc(100% - 22px); padding:10px; border-radius:5px; border:1px solid #4a505c; background-color:#3c4049; color:#abb2bf; margin-bottom: 10px;} input[type="radio"]{width:auto;} input[type="submit"]{background-color:#98c379; color:#282c34; font-weight:bold; cursor:pointer; font-size: 16px;} #bitrate_div{display:none;}</style>
    </head><body>
    <h1>Turntable Audio Streamer Setup</h1>
    <form action="/connect" method="post">
        <h2>Wi-Fi Credentials</h2>
        <input type="text" name="ssid" placeholder="WiFi Network Name (SSID)" required><br>
        <input type="password" name="password" placeholder="Password"><br>
        <h2>Streaming Mode</h2>
        <input type="radio" id="mode_mp3" name="mode" value="0" checked onchange="toggleBitrate(true)">
        <label for="mode_mp3">MP3 Stream (HTTP, good balance)</label><br>
        <input type="radio" id="mode_wav" name="mode" value="1" onchange="toggleBitrate(false)">
        <label for="mode_wav">WAV Stream (Lossless, high bandwidth)</label><br>
        <input type="radio" id="mode_rtsp" name="mode" value="2" onchange="toggleBitrate(true)">
        <label for="mode_rtsp">RTSP Stream (MP3, low latency)</label><br><br>
        <div id="bitrate_div">
            <label for="bitrate">MP3 / RTSP Bitrate:</label>
            <select name="bitrate" id="bitrate">
                <option value="128000">128 kbps</option>
                <option value="192000" selected>192 kbps</option>
                <option value="256000">256 kbps</option>
                <option value="320000">320 kbps</option>
            </select>
        </div>
        <br><br><input type="submit" value="Save and Restart">
    </form>
    <script>
        function toggleBitrate(show) { document.getElementById('bitrate_div').style.display = show ? 'block' : 'none'; }
        toggleBitrate(true);
    </script>
    </body></html>)rawliteral";
    httpd_resp_send(req, resp_str, strlen(resp_str));
    return ESP_OK;
}

static esp_err_t connect_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret, remaining = req->content_len;
    if (remaining >= sizeof(buf)) { return ESP_FAIL; }
    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) { return ESP_FAIL; }
    buf[ret] = '\0';
    app_config_t cfg = {0};
    char mode_str[8], bitrate_str[16];
    if (httpd_query_key_value(buf, "ssid", cfg.ssid, sizeof(cfg.ssid)) == ESP_OK &&
        httpd_query_key_value(buf, "mode", mode_str, sizeof(mode_str)) == ESP_OK) {
        
        // Hasło jest opcjonalne (dla otwartych sieci)
        httpd_query_key_value(buf, "password", cfg.password, sizeof(cfg.password));
        
        cfg.mode = (stream_mode_t)atoi(mode_str);
        if (cfg.mode == STREAM_MODE_MP3 || cfg.mode == STREAM_MODE_RTSP_MP3) {
            if (httpd_query_key_value(buf, "bitrate", bitrate_str, sizeof(bitrate_str)) == ESP_OK) {
                cfg.bitrate = atoi(bitrate_str);
            } else { cfg.bitrate = 192000; }
        }
        ESP_LOGI(TAG, "Saving config: SSID=%s, Mode=%d, Bitrate=%d", cfg.ssid, cfg.mode, cfg.bitrate);
        nvs_handle_t nvs;
        nvs_open("wifi_config", NVS_READWRITE, &nvs);
        nvs_set_blob(nvs, CONFIG_NVS_KEY, &cfg, sizeof(app_config_t));
        nvs_commit(nvs);
        nvs_close(nvs);
        httpd_resp_send(req, "<h1>Configuration Saved!</h1><p>The device will now restart and connect to your Wi-Fi.</p>", -1);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        httpd_resp_send(req, "<h1>Error</h1><p>Incomplete data received. SSID and Mode are required.</p>", -1);
    }
    return ESP_OK;
}

static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
        httpd_register_uri_handler(server, &root_uri);
        httpd_uri_t connect_uri = {.uri = "/connect", .method = HTTP_POST, .handler = connect_post_handler};
        httpd_register_uri_handler(server, &connect_uri);
        // Handler dla dowolnego innego adresu (dla Captive Portal)
        httpd_uri_t wildcard_uri = {.uri = "/*", .method = HTTP_GET, .handler = root_get_handler};
        httpd_register_uri_handler(server, &wildcard_uri);
    }
    return server;
}

// --- Serwer DNS (Magia Captive Portal) ---

#define DNS_PORT 53
typedef struct __attribute__((__packed__)) {
    uint16_t id; uint16_t flags; uint16_t qdcount;
    uint16_t ancount; uint16_t nscount; uint16_t arcount;
} DnsHeader;

void start_dns_server() {
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
}

void dns_server_task(void *pvParameters) {
    char rx_buffer[128];
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(DNS_PORT);
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) { ESP_LOGE(TAG, "DNS: Unable to create socket"); vTaskDelete(NULL); }
    bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
        if (len > 0) {
            DnsHeader *dns_header = (DnsHeader *)rx_buffer;
            if ((dns_header->flags & 0x8000) == 0 && dns_header->qdcount > 0) {
                // To jest zapytanie, odpowiadamy
                tcpip_adapter_ip_info_t ip_info;
                tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip_info);
                
                // Przygotuj odpowiedź
                char *p = rx_buffer + sizeof(DnsHeader);
                while (*p != 0) { p += (*p + 1); }
                p += 5; // Przeskocz za typ i klasę zapytania

                // Nagłówek odpowiedzi
                dns_header->flags |= 0x8000; // Flaga odpowiedzi
                dns_header->ancount = htons(1);

                // Rekord odpowiedzi A
                *p++ = 0xC0; *p++ = 0x0C; // Wskaźnik do nazwy
                *p++ = 0x00; *p++ = 0x01; // Typ A
                *p++ = 0x00; *p++ = 0x01; // Klasa IN
                *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x3C; // TTL 60s
                *p++ = 0x00; *p++ = 0x04; // Długość danych (4 bajty)
                
                // Adres IP
                uint32_t ip_addr = ip_info.ip.addr;
                *p++ = (ip_addr >> 0) & 0xFF;
                *p++ = (ip_addr >> 8) & 0xFF;
                *p++ = (ip_addr >> 16) & 0xFF;
                *p++ = (ip_addr >> 24) & 0xFF;
                
                sendto(sock, rx_buffer, (p - rx_buffer), 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
            }
        }
    }
    close(sock);
    vTaskDelete(NULL);
}