#include "Settings.h"
#include <initializer_list>
#include "esp_log.h"
#include "nvs.h"

namespace {
constexpr const char* Tag = "Settings";
constexpr const char* Namespace = "mutehid";
constexpr const char* VersionKey = "cfgver";
constexpr const char* BrightnessKey = "bright";
constexpr const char* VibrationKey = "vibrate";
constexpr uint8_t Version = 1;
}

namespace storage {

void load(Settings& settings) {
    nvs_handle_t nvs;
    if (nvs_open(Namespace, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(Tag, "No stored settings; using defaults");
        return;
    }
    uint8_t version = 0, brightness = settings.brightness, vibration = settings.vibration;
    const bool known = nvs_get_u8(nvs, VersionKey, &version) == ESP_OK && version == Version;
    if (known) {
        if (nvs_get_u8(nvs, BrightnessKey, &brightness) == ESP_OK && brightness < BrightnessCount)
            settings.brightness = brightness;
        if (nvs_get_u8(nvs, VibrationKey, &vibration) == ESP_OK) settings.vibration = vibration != 0;
    } else if (version) {
        ESP_LOGW(Tag, "Unknown settings version %u; keeping defaults", version);
    }
    nvs_close(nvs);
    ESP_LOGI(Tag, "LOAD brightness=%u vibration=%d", settings.brightness, (int)settings.vibration);
}

esp_err_t save(const Settings& settings) {
    nvs_handle_t nvs;
    auto err = nvs_open(Namespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, VersionKey, Version);
    if (err == ESP_OK) err = nvs_set_u8(nvs, BrightnessKey, settings.brightness);
    if (err == ESP_OK) err = nvs_set_u8(nvs, VibrationKey, settings.vibration ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) ESP_LOGE(Tag, "SAVE failed: %s", esp_err_to_name(err));
    return err;
}

// Only the application's own keys. The bond and the CCCD record survive.
esp_err_t reset() {
    nvs_handle_t nvs;
    auto err = nvs_open(Namespace, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    for (const char* key : {VersionKey, BrightnessKey, VibrationKey}) {
        const auto erased = nvs_erase_key(nvs, key);
        if (erased != ESP_OK && erased != ESP_ERR_NVS_NOT_FOUND) err = erased;
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(Tag, "RESET application settings: %s", esp_err_to_name(err));
    return err;
}

}  // namespace storage
