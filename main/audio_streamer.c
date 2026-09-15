/**
 * @file audio_streamer.c
 * @brief AAC audio streamer: I2S line-in -> AAC encoder -> raw_stream,
 *        served at GET /stream.aac by this file's own esp_http_server.
 */

#include <esp_log.h>
#include <esp_http_server.h>
#include <mdns.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_pipeline.h"
#include "audio_element.h"
#include "board.h"
#include "i2s_stream.h"
#include "aac_encoder.h"
#include "raw_stream.h"
#include "es8388.h"
#include "equalizer.h"

#include "audio_streamer.h"

static const char *TAG = "AUDIO_STREAMER";

// ponytail: measured via FFT on a captured stream -- 50Hz mains hum plus harmonics at
// 100/150/200/250/300/400/500Hz, ~38dB below signal peak. ESP-ADF's equalizer is a 10-band
// graphic EQ (band centers below), not a surgical notch, so this cuts the lowest 3 bands
// (31/62/125Hz -- mostly rumble territory, below where most turntable program content lives)
// instead of trying to notch every harmonic individually. Revisit with narrower cuts if this
// turns out to eat too much real bass.
// Band centers at 44100/48000Hz: 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 Hz.
// Stereo needs 20 entries (10 bands x L+R, per equalizer.h); both channels get the same cut.
static int s_hum_filter_gain[20] = {-13, -10, -6, 0, 0, 0, 0, 0, 0, 0, -13, -10, -6, 0, 0, 0, 0, 0, 0, 0};
static int s_flat_gain[20]       = {0};

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
            httpd_resp_send_chunk(req, NULL, 0); // end the chunked response cleanly
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
    httpd_cfg.stack_size = 8192; // headroom for stream_get_handler's 1KB stack buffer + httpd overhead
    esp_err_t err = httpd_start(&server, &httpd_cfg);
    if (err == ESP_OK) {
        httpd_uri_t stream_uri = {.uri = "/stream.aac", .method = HTTP_GET, .handler = stream_get_handler};
        httpd_register_uri_handler(server, &stream_uri);
        ESP_LOGI(TAG, "HTTP stream server started on port %d", httpd_cfg.server_port);
    } else {
        ESP_LOGE(TAG, "httpd_start failed on port %d: %s", httpd_cfg.server_port, esp_err_to_name(err));
    }
}

void audio_streamer_start(const app_config_t *config)
{
    ESP_LOGI(TAG, "Starting streamer. Bitrate: %d bps, input gain: %d dB", config->bitrate, config->input_gain_db);

    // ponytail: settle delay before the first I2C write to the ES8388 -- cheap insurance
    // against a boot-time power-rail race, harmless if unneeded. (Earlier I2C NACKs this
    // session were actually caused by using the wrong board definition -- LyraT V4.3's
    // pinout instead of this board's ai-thinker-esp32-a1s one -- now fixed via board.h.)
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "Initializing audio board and codec...");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);
    // ponytail: es_mic_gain_t's values are literally the dB step (0,3,...,24 -
    // verified against esp-adf's esxxx_common.h), so a direct cast replaces a lookup table.
    esp_err_t gain_err = ESP_FAIL;
    for (int attempt = 0; attempt < 5 && gain_err != ESP_OK; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        gain_err = es8388_set_mic_gain((es_mic_gain_t)config->input_gain_db);
        if (gain_err != ESP_OK) {
            ESP_LOGW(TAG, "es8388_set_mic_gain failed (attempt %d/5): %s", attempt + 1, esp_err_to_name(gain_err));
        }
    }
    if (gain_err != ESP_OK) {
        ESP_LOGE(TAG, "Could not set input gain after 5 attempts -- codec I2C is unresponsive; "
                       "audio will likely be silent");
    }

    ESP_LOGI(TAG, "Creating audio pipeline...");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    audio_pipeline_handle_t pipeline = audio_pipeline_init(&pipeline_cfg);

    ESP_LOGI(TAG, "Creating I2S stream reader (audio input)...");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_READER;
    audio_element_handle_t i2s_stream_reader = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "Configuring hum filter (equalizer, %s)...", config->hum_filter_enabled ? "on" : "off");
    equalizer_cfg_t eq_cfg = DEFAULT_EQUALIZER_CONFIG();
    eq_cfg.samplerate = 44100; // must match I2S_STREAM_CFG_DEFAULT()'s rate
    eq_cfg.channel = 2;
    eq_cfg.set_gain = config->hum_filter_enabled ? s_hum_filter_gain : s_flat_gain;
    audio_element_handle_t equalizer = equalizer_init(&eq_cfg);

    ESP_LOGI(TAG, "Configuring AAC encoder...");
    aac_encoder_cfg_t aac_cfg = DEFAULT_AAC_ENCODER_CONFIG();
    aac_cfg.bitrate = config->bitrate;
    audio_element_handle_t encoder = aac_encoder_init(&aac_cfg);

    ESP_LOGI(TAG, "Configuring raw_stream output tap...");
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;
    s_raw_reader = raw_stream_init(&raw_cfg);

    ESP_LOGI(TAG, "Linking elements: i2s -> equalizer -> aac -> raw");
    audio_pipeline_register(pipeline, i2s_stream_reader, "i2s");
    audio_pipeline_register(pipeline, equalizer, "equalizer");
    audio_pipeline_register(pipeline, encoder, "aac");
    audio_pipeline_register(pipeline, s_raw_reader, "raw");
    const char *link[4] = {"i2s", "equalizer", "aac", "raw"};
    audio_pipeline_link(pipeline, &link[0], 4);

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
