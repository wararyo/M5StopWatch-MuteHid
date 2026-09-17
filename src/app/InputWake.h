#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Wakes the app task on a button press or a touch-controller interrupt, from a
// blocking wait or from automatic light sleep, so the main loop can wait long
// while idle without missing short taps. See docs/power-saving-ideas.md (17).
namespace input_wake {

void begin(TaskHandle_t task);
// Interrupts are level-triggered and disable themselves on firing; call before
// each wait to re-enable the pins that are back at their idle (high) level.
void rearm();

}  // namespace input_wake
