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
