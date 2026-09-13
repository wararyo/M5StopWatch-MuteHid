#pragma once
#include <cstdint>

// Power measurement support. The StopWatch's M5PM1 reports voltages only, so
// current comes from an external USB tester; this module prints what the
// device itself knows next to it. See docs/power-saving-ideas.md.
namespace power {

struct Sample {
    uint32_t ms;
    int16_t vbat, vbus;  // mV; the M5Unified getters return <= 0 on a failed read.
    int8_t level;        // Percent derived from vbat, -1 when unknown.
    bool charging;
    int cpuMhz;
};

// Device state that decides the draw, supplied by the app task.
struct Context {
    const char* display;  // "on", "dim", "dark" or "sleep".
    bool dimmed, panelOn;
    uint8_t brightness;   // Value last applied to the panel.
    bool connected, advertising, hold;
};

// Reads the PMIC over I2C; call from the task that owns M5Unified.
Sample sample();
// One "PWR key=value ..." line on stdout, independent of the log level.
void print(const Sample& sample, const Context& context);

// Battery-voltage record for runs on battery: one entry a minute in RAM,
// keeping the latest 24 hours, plus one every 5 minutes in NVS for the first
// 8 hours. Plugging USB back in does not reset the chip, so the RAM record
// survives until it is dumped as "DRAIN" lines; if the battery ran flat, the
// dump falls back to the NVS copy. Starting a record clears both.
void startRecord(uint32_t now, const Context& context);
void stopRecord();
bool recording();
void tickRecord(uint32_t now, const Context& context);
void dump();

}  // namespace power
