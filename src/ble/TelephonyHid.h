#pragma once
#include <cstdint>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace telephony {
enum class Kind : uint8_t { Started, Connected, Disconnected, Auth, Subscribe, Output, Numeric, Passkey, Control, Protocol, Error };
struct Event { Kind kind; uint32_t generation; uint32_t value; uint16_t length; uint16_t id; };
esp_err_t begin();
bool poll(Event& event);
// Task to notify whenever an event is queued, so it can block instead of polling.
void setWaiter(TaskHandle_t task);
bool overflowed();
esp_err_t send(uint8_t value);
void confirm(bool accept);
void forget();
void stop();
void disconnectPeer();
int bonds();
bool peerText(char* out, unsigned size);
void battery(uint8_t level);
// Power measurement: hold advertising off without dropping the link or bond.
void pauseAdvertising(bool pause);
bool advertisingActive();
}

