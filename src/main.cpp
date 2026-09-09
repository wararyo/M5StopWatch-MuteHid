#include <M5Unified.h>
#include <fcntl.h>
#include <unistd.h>
#include "app/FirmwareSwitch.h"
#include "app/MuteApp.h"
#include "ble/TelephonyHid.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "nvs_flash.h"

namespace {
void fail(const char* message) {
    M5.Display.setTextDatum(middle_center);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.drawString(message, M5.Display.width() / 2, M5.Display.height() / 2);
}
}

extern "C" void app_main() {
    firmware::checkStartupEscape();
    auto cfg = M5.config();
    cfg.internal_imu = cfg.internal_rtc = cfg.internal_mic = cfg.internal_spk = false;
    M5.begin(cfg);
    M5.Display.setBrightness(90);
    ESP_LOGI("Boot", "BOOT %s %s IDF=%s", esp_app_get_description()->project_name,
             esp_app_get_description()->version, esp_app_get_description()->idf_ver);

    const auto nvsErr = nvs_flash_init();
    if (nvsErr != ESP_OK) {
        // Erasing here would take the shared UserDemo settings and the BLE bond
        // with it; recovery stays a deliberate, documented step.
        ESP_LOGE("Boot", "NVS init failed: %s; NEVER erasing shared NVS", esp_err_to_name(nvsErr));
        fail("NVS error: shared data kept");
        return;
    }
    const auto err = telephony::begin();
    if (err != ESP_OK) {
        ESP_LOGE("Boot", "BLE init: %s", esp_err_to_name(err));
        fail("BLE init failed");
        return;
    }
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    app::run();
}
