#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

// Spawns a task that saves `url` as the last-used OTA URL (NVS namespace
// "storage", key "ota_url") and then downloads and flashes it. Safe to call
// from an HTTP handler or event callback -- it never blocks the caller.
// On success the device reboots into the new firmware; on failure it logs
// the error and keeps running the current firmware.
void ota_manager_install_async(const char *url);

// Spawns a task that re-installs the last URL saved via
// ota_manager_install_async(). No-ops (logs a warning) if none was saved yet.
void ota_manager_install_last_async(void);

#endif // OTA_MANAGER_H
