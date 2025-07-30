/**
 * @file audio_streamer.c
 * @brief Implementacja logiki streamingu audio.
 * 
 * Ten plik jest odpowiedzialny za inicjalizację sprzętu audio,
 * a następnie dynamiczne tworzenie i uruchamianie odpowiedniego
 * pipeline'u audio (MP3, WAV, lub RTSP) na podstawie konfiguracji
 * przekazanej przez wifi_manager.
 */

#include <esp_log.h>
#include <mdns.h>

// Nagłówki z frameworka ESP-ADF
#include "audio_pipeline.h"
#include "audio_element.h"
#include "board.h"
#include "i2s_stream.h"
#include "mp3_encoder.h"
#include "wav_encoder.h"
#include "http_stream.h"
#include "rtsp_stream.h"

// Nasze pliki nagłówkowe
#include "audio_streamer.h"

static const char *TAG = "AUDIO_STREAMER";

/**
 * @brief Uruchamia streamer audio z podaną konfiguracją.
 */
void audio_streamer_start(const app_config_t *config)
{
    ESP_LOGI(TAG, "Starting streamer in Mode: %d, Bitrate: %d", config->mode, config->bitrate);

    audio_pipeline_handle_t pipeline;
    audio_element_handle_t i2s_stream_reader, encoder, stream_writer;

    // --- Krok 1: Inicjalizacja stałych elementów (wspólne dla wszystkich trybów) ---

    ESP_LOGI(TAG, "Initializing audio board and codec...");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_ENCODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "Creating audio pipeline...");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);

    ESP_LOGI(TAG, "Creating I2S stream reader (audio input)...");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_READER;
    i2s_stream_reader = i2s_stream_init(&i2s_cfg);

    // --- Krok 2: Dynamiczne budowanie pipeline'u na podstawie konfiguracji ---

    // Domyślny URI, zostanie nadpisany w każdym przypadku
    const char* stream_uri = "/stream.mp3"; 

    switch (config->mode) {

        case STREAM_MODE_MP3:
            ESP_LOGI(TAG, "Configuring pipeline for: MP3 over HTTP");
            
            // Konfiguracja enkodera MP3
            mp3_encoder_cfg_t mp3_cfg = DEFAULT_MP3_ENCODER_CONFIG();
            mp3_cfg.bitrate = config->bitrate;
            encoder = mp3_encoder_init(&mp3_cfg);
            
            // Konfiguracja serwera/writera HTTP
            http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
            http_cfg.type = AUDIO_STREAM_WRITER;
            stream_writer = http_stream_init(&http_cfg);
            http_stream_set_uri(stream_writer, "/stream.mp3");
            stream_uri = "http://turntable.local/stream.mp3";

            // Rejestracja i łączenie elementów
            ESP_LOGI(TAG, "Linking elements: i2s -> mp3 -> http");
            audio_pipeline_register(pipeline, i2s_stream_reader, "i2s");
            audio_pipeline_register(pipeline, encoder, "mp3");
            audio_pipeline_register(pipeline, stream_writer, "http");
            const char *link_mp3[3] = {"i2s", "mp3", "http"};
            audio_pipeline_link(pipeline, &link_mp3[0], 3);
            break;

        case STREAM_MODE_WAV:
            ESP_LOGI(TAG, "Configuring pipeline for: WAV over HTTP (Lossless)");
            
            // Konfiguracja enkodera WAV
            wav_encoder_cfg_t wav_cfg = DEFAULT_WAV_ENCODER_CONFIG();
            encoder = wav_encoder_init(&wav_cfg);

            // Konfiguracja serwera/writera HTTP
            http_stream_cfg_t http_wav_cfg = HTTP_STREAM_CFG_DEFAULT();
            http_wav_cfg.type = AUDIO_STREAM_WRITER;
            stream_writer = http_stream_init(&http_wav_cfg);
            http_stream_set_uri(stream_writer, "/stream.wav");
            stream_uri = "http://turntable.local/stream.wav";

            // Rejestracja i łączenie elementów
            ESP_LOGI(TAG, "Linking elements: i2s -> wav -> http");
            audio_pipeline_register(pipeline, i2s_stream_reader, "i2s");
            audio_pipeline_register(pipeline, encoder, "wav");
            audio_pipeline_register(pipeline, stream_writer, "http");
            const char *link_wav[3] = {"i2s", "wav", "http"};
            audio_pipeline_link(pipeline, &link_wav[0], 3);
            break;

        case STREAM_MODE_RTSP_MP3:
            ESP_LOGI(TAG, "Configuring pipeline for: MP3 over RTSP (Low Latency)");
            
            // Konfiguracja enkodera MP3
            mp3_encoder_cfg_t rtsp_mp3_cfg = DEFAULT_MP3_ENCODER_CONFIG();
            rtsp_mp3_cfg.bitrate = config->bitrate;
            encoder = mp3_encoder_init(&rtsp_mp3_cfg);
            
            // Konfiguracja serwera/writera RTSP
            rtsp_stream_cfg_t rtsp_cfg = DEFAULT_RTSP_STREAM_CFG();
            rtsp_cfg.port = 554;
            stream_writer = rtsp_stream_init(&rtsp_cfg);
            // Domyślna ścieżka dla RTSP w ESP-ADF to /live.mp3
            stream_uri = "rtsp://turntable.local:554/live.mp3";
            
            // Rejestracja i łączenie elementów
            ESP_LOGI(TAG, "Linking elements: i2s -> mp3 -> rtsp");
            audio_pipeline_register(pipeline, i2s_stream_reader, "i2s");
            audio_pipeline_register(pipeline, encoder, "mp3");
            audio_pipeline_register(pipeline, stream_writer, "rtsp");
            const char *link_rtsp[3] = {"i2s", "mp3", "rtsp"};
            audio_pipeline_link(pipeline, &link_rtsp[0], 3);
            break;

        default:
            ESP_LOGE(TAG, "Unknown streaming mode selected: %d", config->mode);
            return; // Zakończ, jeśli tryb jest nieznany
    }
    
    // --- Krok 3: Inicjalizacja mDNS i uruchomienie pipeline'u ---

    ESP_LOGI(TAG, "Initializing mDNS service...");
    mdns_init();
    mdns_hostname_set("turntable");
    mdns_instance_name_set("Turntable Audio Streamer");
    
    // Zarejestruj odpowiednią usługę w zależności od protokołu
    if (config->mode == STREAM_MODE_RTSP_MP3) {
        mdns_service_add(NULL, "_rtsp", "_tcp", 554, NULL, 0);
    } else {
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    
    ESP_LOGI(TAG, "mDNS initialized. Stream available at: %s", stream_uri);

    ESP_LOGI(TAG, "Starting audio pipeline...");
    audio_pipeline_run(pipeline);
    ESP_LOGI(TAG, "Audio pipeline is now running.");
}