#include "wifi_manager.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_http_server.h>
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/prot/dns.h"
#include "audio_streamer.h"

static const char *TAG = "WIFI_MANAGER";

#define WIFI_AP_SSID "Turntable-Setup"
static EventGroupHandle_t wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

// --- Deklaracje funkcji wewnętrznych ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void start_sta_mode(const app_config_t *config);
static void start_captive_portal(void);
static void dns_server_task(void *pvParameters);
void start_dns_server(void) { xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL); }

// --- Implementacja serwera DNS (POPRAWIONA) ---
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

        if (len > 0) {
            // Zdobądź adres IP naszego punktu dostępowego
            esp_netif_ip_info_t ip_info;
            esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            esp_netif_get_ip_info(netif, &ip_info);
            
            // Odpowiedz tym adresem na każde zapytanie DNS
            struct dns_hdr *dns_header = (struct dns_hdr *)rx_buffer;

            if ((dns_header->flags1 & DNS_FLAG1_RESPONSE) == 0) {
                dns_header->flags1 |= DNS_FLAG1_RESPONSE | DNS_FLAG1_AUTHORATIVE;

                dns_header->numanswers = dns_header->numquestions;
                dns_header->numauthrr = htons(0);
                dns_header->numextrarr = htons(0);
            }

            uint8_t *query_end = rx_buffer + sizeof(dns_header);
            while (*query_end != 0) {
                query_end += (*query_end + 1);
            }
            query_end += 5; // Skip QTYPE and QCLASS

            // Stwórz odpowiedź
            *query_end++ = 0xC0;
            *query_end++ = 0x0C;
            *query_end++ = 0x00;
            *query_end++ = 0x01; // Type A
            *query_end++ = 0x00;
            *query_end++ = 0x01; // Class IN
            *query_end++ = 0x00;
            *query_end++ = 0x00;
            *query_end++ = 0x00;
            *query_end++ = 0x0A; // TTL 10 sekund
            *query_end++ = 0x00;
            *query_end++ = 0x04; // Długość danych (4 bajty)
            memcpy(query_end, &ip_info.ip.addr, sizeof(ip_info.ip.addr));
            
            sendto(sock, rx_buffer, (query_end - rx_buffer) + 4, 0, (struct sockaddr *)&source_addr, socklen);
        }
    }
    close(sock);
    vTaskDelete(NULL);
}


// --- Reszta kodu bez zmian (ale wklej ją dla pewności) ---

static esp_err_t root_get_handler(httpd_req_t *req) {
    const char* resp_str = (const char*) R"rawliteral(
    <!DOCTYPE html><html><head><title>Turntable Wi-Fi Setup</title><meta name="viewport" content="width=device-width, initial-scale=1"><style>body{font-family:sans-serif; background-color:#282c34; color:#fff; padding:20px;} h1,h2{color:#61afef;} input,select{padding:10px; width:calc(100% - 22px); border-radius:5px; border:1px solid #61afef; background-color:#3c4049; color:#fff;} input[type="submit"]{background-color:#98c379; color:#282c34; font-weight:bold; cursor:pointer; width:100%;} #bitrate_div{display:none;}</style></head>
    <body><h1>Turntable Wi-Fi Setup</h1><form action="/connect" method="post"><h2>Wi-Fi Credentials</h2><input type="text" name="ssid" placeholder="WiFi SSID" required><br><br><input type="password" name="password" placeholder="Password"><br><br><h2>Streaming Mode</h2><input type="radio" id="mode_mp3" name="mode" value="0" checked onchange="toggleBitrate(true)"><label for="mode_mp3"> MP3 Stream (HTTP)</label><br><input type="radio" id="mode_wav" name="mode" value="1" onchange="toggleBitrate(false)"><label for="mode_wav"> WAV Stream (Lossless)</label><br><input type="radio" id="mode_rtsp" name="mode" value="2" onchange="toggleBitrate(true)"><label for="mode_rtsp"> RTSP Stream (Low Latency)</label><br><br><div id="bitrate_div"><label for="bitrate">MP3/RTSP Bitrate:</label><br><select name="bitrate" id="bitrate"><option value="128000">128 kbps</option><option value="192000" selected>192 kbps</option><option value="256000">256 kbps</option><option value="320000">320 kbps</option></select></div><br><br><input type="submit" value="Save and Restart"></form>
    <script>function toggleBitrate(show){document.getElementById('bitrate_div').style.display = show ? 'block' : 'none';}toggleBitrate(true);</script></body></html>)rawliteral";
    httpd_resp_send(req, resp_str, strlen(resp_str));
    return ESP_OK;
}

static esp_err_t connect_post_handler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    app_config_t cfg = {0};
    char mode_str[8], bitrate_str[16];
    if (httpd_query_key_value(buf, "ssid", cfg.ssid, sizeof(cfg.ssid)) == ESP_OK && httpd_query_key_value(buf, "password", cfg.password, sizeof(cfg.password)) == ESP_OK && httpd_query_key_value(buf, "mode", mode_str, sizeof(mode_str)) == ESP_OK) {
        cfg.mode = (stream_mode_t)atoi(mode_str);
        if (cfg.mode != STREAM_MODE_WAV && httpd_query_key_value(buf, "bitrate", bitrate_str, sizeof(bitrate_str)) == ESP_OK) {
            cfg.bitrate = atoi(bitrate_str);
        } else { cfg.bitrate = 192000; }
        ESP_LOGI(TAG, "Saving config: SSID=%s, Mode=%d, Bitrate=%d", cfg.ssid, cfg.mode, cfg.bitrate);
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

static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
        httpd_register_uri_handler(server, &root_uri);
        httpd_uri_t connect_uri = {.uri = "/connect", .method = HTTP_POST, .handler = connect_post_handler};
        httpd_register_uri_handler(server, &connect_uri);
        httpd_uri_t wildcard = {.uri = "/*", .method = HTTP_GET, .handler = root_get_handler};
        httpd_register_uri_handler(server, &wildcard);
    }
    return server;
}

void wifi_manager_start(void) {
    app_config_t cfg;
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs));
    size_t required_size = sizeof(cfg);
    esp_err_t err = nvs_get_blob(nvs, CONFIG_NVS_KEY, &cfg, &required_size);
    nvs_close(nvs);
    if (err == ESP_OK) { start_sta_mode(&cfg); }
    else { start_captive_portal(); }
}

static void start_sta_mode(const app_config_t *config) {
    wifi_event_group = xEventGroupCreate();
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
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP. Starting audio streamer.");
        audio_streamer_start(config);
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
    start_dns_server();
    start_webserver();
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}