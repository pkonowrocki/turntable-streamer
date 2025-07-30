#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define CONFIG_NVS_KEY "app_config"

typedef enum {
    STREAM_MODE_MP3,
    STREAM_MODE_WAV,
    STREAM_MODE_RTSP_MP3,
} stream_mode_t;

typedef struct {
    char ssid[32];
    char password[64];
    stream_mode_t mode;
    int bitrate;
} app_config_t;

#endif // APP_CONFIG_H