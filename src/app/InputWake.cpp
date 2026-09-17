#include "InputWake.h"
#include <cstdint>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"

namespace {
constexpr const char* Tag = "InputWake";
// KEY.A, KEY.B and the CST820 INT line; all idle high and go low when active.
constexpr gpio_num_t Pins[] = {GPIO_NUM_2, GPIO_NUM_1, GPIO_NUM_13};

TaskHandle_t waiter = nullptr;

void IRAM_ATTR onLow(void* arg) {
    // A level interrupt would refire for as long as the pin stays low.
    gpio_intr_disable(static_cast<gpio_num_t>(reinterpret_cast<intptr_t>(arg)));
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(waiter, &woken);
    if (woken) portYIELD_FROM_ISR();
}
}  // namespace

namespace input_wake {

void begin(TaskHandle_t task) {
    waiter = task;
    auto err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(Tag, "ISR service: %s", esp_err_to_name(err));
        return;
    }
    for (const auto pin : Pins) {
        // Light-sleep GPIO wake-up only supports levels, and it shares the pin's
        // interrupt type, so the run-time interrupt is a low level as well.
        gpio_set_intr_type(pin, GPIO_INTR_LOW_LEVEL);
        gpio_isr_handler_add(pin, onLow, reinterpret_cast<void*>(static_cast<intptr_t>(pin)));
        gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);
    }
    err = esp_sleep_enable_gpio_wakeup();
    if (err != ESP_OK) ESP_LOGE(Tag, "GPIO wake-up: %s", esp_err_to_name(err));
    rearm();
}

void rearm() {
    for (const auto pin : Pins) {
        if (gpio_get_level(pin)) gpio_intr_enable(pin);
    }
}

}  // namespace input_wake
