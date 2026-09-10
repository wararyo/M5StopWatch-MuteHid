#include "PowerProbe.h"
#include <M5Unified.h>
#include <cstdio>
#include "esp_private/esp_clk.h"
#include "esp_timer.h"

namespace {
constexpr uint32_t RecordIntervalMs = 60000;
constexpr int Capacity = 24 * 60;
// VBUS above this means USB power is present, so the battery is not the source.
constexpr int16_t UsbPresentMv = 4000;

// State bits, in the order tools/power_measure.py names them.
enum : uint8_t { Connected = 1, Advertising = 2, Lit = 4, Dimmed = 8, Charging = 16, Usb = 32 };

struct Entry {
    uint32_t seconds;  // Since the record started.
    int16_t vbat;
    uint8_t state;
};
Entry entries[Capacity];
int head = 0, count = 0;  // head is the next slot to write.
bool active = false;
uint32_t startedAt = 0, nextAt = 0;

uint8_t stateBits(const power::Sample& s, const power::Context& c) {
    uint8_t bits = 0;
    if (c.connected) bits |= Connected;
    if (c.advertising) bits |= Advertising;
    if (c.panelOn && c.brightness) bits |= Lit;
    if (c.dimmed) bits |= Dimmed;
    if (s.charging) bits |= Charging;
    if (s.vbus >= UsbPresentMv) bits |= Usb;
    return bits;
}

void append(uint32_t now, const power::Context& context) {
    const auto s = power::sample();
    entries[head] = {(now - startedAt) / 1000, s.vbat, stateBits(s, context)};
    head = (head + 1) % Capacity;
    if (count < Capacity) ++count;
}
}  // namespace

namespace power {

Sample sample() {
    Sample s{};
    s.ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    s.vbat = M5.Power.getBatteryVoltage();
    s.vbus = M5.Power.getVBUSVoltage();
    s.level = static_cast<int8_t>(M5.Power.getBatteryLevel());
    s.charging = M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
    s.cpuMhz = esp_clk_cpu_freq() / 1000000;
    return s;
}

void print(const Sample& s, const Context& c) {
    std::printf("PWR t=%lu vbat=%d vbus=%d lvl=%d chg=%d disp=%s bri=%u conn=%d adv=%d cpu=%d hold=%d rec=%d\n",
                (unsigned long)s.ms, s.vbat, s.vbus, s.level, s.charging, c.display, c.brightness,
                c.connected, c.advertising, s.cpuMhz, c.hold, active);
    std::fflush(stdout);
}

void startRecord(uint32_t now, const Context& context) {
    head = count = 0;
    startedAt = now;
    active = true;
    append(now, context);
    nextAt = now + RecordIntervalMs;
    std::printf("DRAIN start interval=%lus capacity=%d\n", (unsigned long)(RecordIntervalMs / 1000), Capacity);
    std::fflush(stdout);
}

void stopRecord() {
    active = false;
    std::printf("DRAIN stop n=%d\n", count);
    std::fflush(stdout);
}

bool recording() { return active; }

void tickRecord(uint32_t now, const Context& context) {
    if (!active || static_cast<int32_t>(now - nextAt) < 0) return;
    append(now, context);
    nextAt += RecordIntervalMs;
}

void dump() {
    const int first = (head - count + Capacity) % Capacity;
    for (int i = 0; i < count; ++i) {
        const auto& e = entries[(first + i) % Capacity];
        std::printf("DRAIN t=%lu vbat=%d st=%u\n", (unsigned long)e.seconds, e.vbat, e.state);
    }
    std::printf("DRAIN end n=%d\n", count);
    std::fflush(stdout);
}

}  // namespace power
