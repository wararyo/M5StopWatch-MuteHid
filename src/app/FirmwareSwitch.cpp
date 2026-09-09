#include "FirmwareSwitch.h"
#include <cstring>
#include "driver/gpio.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_log.h"

namespace firmware {
bool returnToUserDemo() {
#if MUTEHID_COEXIST
    const auto* target = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    esp_app_desc_t desc{};
    if (!target || target == esp_ota_get_running_partition() ||
        esp_ota_get_partition_description(target, &desc) != ESP_OK ||
        std::strcmp(desc.project_name, "StopWatch-UserDemo") != 0) {
        ESP_LOGE("Firmware", "Valid UserDemo not found; staying here");
        return false;
    }
    const auto err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) { ESP_LOGE("Firmware", "Boot selection: %s", esp_err_to_name(err)); return false; }
    esp_restart();
#endif
    return false;
}
void checkStartupEscape() {
#if MUTEHID_COEXIST
    gpio_config_t cfg{};
    cfg.pin_bit_mask = 1ULL << GPIO_NUM_1;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    if (gpio_config(&cfg) == ESP_OK && gpio_get_level(GPIO_NUM_1) == 0) returnToUserDemo();
#endif
}
}
