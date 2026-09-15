#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <stdbool.h>
#include "app_config.h"

// Connects to config->mqtt_broker_uri and publishes Home Assistant MQTT
// Discovery for this device (stream URL, streaming/RSSI sensors, and
// restart/factory-reset/install-update buttons). No-ops if
// config->mqtt_broker_uri is empty. Call once, after Wi-Fi is up. Never
// blocks the caller -- esp_mqtt_client manages its own connect/retry loop.
void mqtt_manager_start(const app_config_t *config);

// Publishes the "streaming" binary_sensor's state. No-op if MQTT isn't
// configured/connected yet.
void mqtt_manager_set_streaming(bool streaming);

#endif // MQTT_MANAGER_H
