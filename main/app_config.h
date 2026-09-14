#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define CONFIG_NVS_KEY "app_config"

typedef struct {
    char ssid[32];
    char password[64];
    int bitrate;        // AAC encoder bitrate in bps (128000/192000/256000/320000)
    int input_gain_db;  // ES8388 line-in gain: 0,3,6,9,12,15,18,21, or 24 (dB)
} app_config_t;

#endif // APP_CONFIG_H
