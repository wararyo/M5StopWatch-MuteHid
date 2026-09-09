#pragma once
#include <cstdint>
#include "esp_err.h"

// Application settings in the shared NVS. The BLE bond and the stored Input
// CCCD live in the same namespace but are managed by ble/TelephonyHid; erasing
// settings must never touch them (specification section 6.2).
namespace storage {

inline constexpr uint8_t BrightnessSteps[] = {40, 90, 140, 190, 245};
inline constexpr uint8_t BrightnessCount = sizeof(BrightnessSteps) / sizeof(BrightnessSteps[0]);

struct Settings {
    uint8_t brightness = 2;  // Index into BrightnessSteps.
    bool vibration = true;
    uint8_t level() const { return BrightnessSteps[brightness < BrightnessCount ? brightness : 2]; }
    uint8_t percent() const { return static_cast<uint8_t>((brightness + 1) * 100 / BrightnessCount); }
};

void load(Settings& settings);
esp_err_t save(const Settings& settings);
esp_err_t reset();

}  // namespace storage
