#include <M5Unified.h>
#include <fcntl.h>
#include <unistd.h>
#include "app/FirmwareSwitch.h"
#include "app/MuteApp.h"
#include "app/PowerProbe.h"
#include "ble/TelephonyHid.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_pm.h"
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
    // Nothing is attached to Grove; the PMIC's 5 V boost only costs battery.
    cfg.output_power = false;
    M5.begin(cfg);
    M5.Display.setBrightness(90);
    ESP_LOGI("Boot", "BOOT %s %s IDF=%s", esp_app_get_description()->project_name,
             esp_app_get_description()->version, esp_app_get_description()->idf_ver);

    // Automatic light sleep while idle; the BT controller's modem sleep keeps the
    // connection alive on the main crystal. No DFS: the clock is already fixed at
    // CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, so min and max are the same.
    esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = true,
    };
    const auto pmErr = esp_pm_configure(&pm);
    if (pmErr != ESP_OK) {
        ESP_LOGW("Boot", "PM configure: %s", esp_err_to_name(pmErr));
    }
    // Take the lock before anything can sleep: light sleep kills the USB console.
    power::updateSleepLock(M5.Power.getVBUSVoltage());

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
