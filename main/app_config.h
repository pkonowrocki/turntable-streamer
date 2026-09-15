#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#define CONFIG_NVS_KEY "app_config"

typedef struct {
    char ssid[32];
    char password[64];
    int bitrate;            // AAC encoder bitrate in bps (128000/192000/256000/320000)
    int input_gain_db;      // ES8388 line-in gain: 0,3,6,9,12,15,18,21, or 24 (dB)
    int hum_filter_enabled; // 1 = cut low-band EQ to reduce 50/100Hz mains hum, 0 = flat
    char mqtt_broker_uri[128]; // e.g. "mqtt://192.168.1.10:1883"; empty = MQTT disabled
    char mqtt_username[32];
    char mqtt_password[64];
} app_config_t;

#endif // APP_CONFIG_H
