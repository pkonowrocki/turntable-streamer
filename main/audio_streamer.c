/**
 * @file audio_streamer.c
 * @brief AAC-over-HTTP audio streamer: I2S line-in -> AAC encoder -> HTTP server.
 */

#include <esp_log.h>
#include <mdns.h>

#include "audio_pipeline.h"
#include "audio_element.h"
#include "board.h"
#include "i2s_stream.h"
#include "aac_encoder.h"
#include "http_stream.h"
#include "es8388.h"

#include "audio_streamer.h"

static const char *TAG = "AUDIO_STREAMER";

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

    ESP_LOGI(TAG, "Configuring HTTP stream writer...");
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.type = AUDIO_STREAM_WRITER;
    audio_element_handle_t stream_writer = http_stream_init(&http_cfg);
    audio_element_set_uri(stream_writer, "/stream.aac");

    ESP_LOGI(TAG, "Linking elements: i2s -> aac -> http");
    audio_pipeline_register(pipeline, i2s_stream_reader, "i2s");
    audio_pipeline_register(pipeline, encoder, "aac");
    audio_pipeline_register(pipeline, stream_writer, "http");
    const char *link[3] = {"i2s", "aac", "http"};
    audio_pipeline_link(pipeline, &link[0], 3);

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
