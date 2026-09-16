#include "PowerProbe.h"
#include <M5Unified.h>
#include <cstdio>
#include "esp_err.h"
#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include "nvs.h"

namespace {
constexpr uint32_t RecordIntervalMs = 60000;
constexpr int Capacity = 24 * 60;
// VBUS above this means USB power is present, so the battery is not the source.
constexpr int16_t UsbPresentMv = 4000;

// A coarser copy of the record survives the battery running flat. It is one
// fixed-size blob in the app's namespace, which it shares with the settings
// and the CCC record, so it stays small: 8 hours at 5-minute steps.
constexpr uint32_t PersistIntervalMs = 5 * 60000;
constexpr int PersistCapacity = 8 * 12;
constexpr const char* Namespace = "mutehid";
constexpr const char* PersistKey = "pwr_drain";
constexpr uint16_t PersistVersion = 1;

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
uint32_t startedAt = 0, nextAt = 0, nextPersistAt = 0;

struct Persisted {
    uint16_t version, intervalS, count;
    int16_t vbat[PersistCapacity];
    uint8_t state[PersistCapacity];
};
Persisted persisted{};

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

void append(uint32_t now, const power::Sample& s, uint8_t state) {
    entries[head] = {(now - startedAt) / 1000, s.vbat, state};
    head = (head + 1) % Capacity;
    if (count < Capacity) ++count;
}

void savePersisted() {
    nvs_handle_t nvs;
    auto err = nvs_open(Namespace, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, PersistKey, &persisted, sizeof(persisted));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        std::printf("DRAIN nvs_error=%s\n", esp_err_to_name(err));
        std::fflush(stdout);
    }
}

bool loadPersisted(Persisted& out) {
    nvs_handle_t nvs;
    if (nvs_open(Namespace, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t size = sizeof(out);
    const auto err = nvs_get_blob(nvs, PersistKey, &out, &size);
    nvs_close(nvs);
    return err == ESP_OK && size == sizeof(out) && out.version == PersistVersion && out.count <= PersistCapacity;
}

void persist(const power::Sample& s, uint8_t state) {
    if (persisted.count >= PersistCapacity) return;  // Keep the first 8 hours.
    persisted.vbat[persisted.count] = s.vbat;
    persisted.state[persisted.count] = state;
    ++persisted.count;
    savePersisted();
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
    s.ext = M5.Power.getExtOutput();
    return s;
}

void print(const Sample& s, const Context& c) {
    std::printf("PWR t=%lu vbat=%d vbus=%d lvl=%d chg=%d disp=%s bri=%u conn=%d adv=%d cpu=%d ext=%d hold=%d rec=%d\n",
                (unsigned long)s.ms, s.vbat, s.vbus, s.level, s.charging, c.display, c.brightness,
                c.connected, c.advertising, s.cpuMhz, s.ext, c.hold, active);
    std::fflush(stdout);
}

void startRecord(uint32_t now, const Context& context) {
    head = count = 0;
    startedAt = now;
    active = true;
    persisted = {PersistVersion, static_cast<uint16_t>(PersistIntervalMs / 1000), 0, {}, {}};
    const auto s = sample();
    const auto state = stateBits(s, context);
    append(now, s, state);
    persist(s, state);
    nextAt = now + RecordIntervalMs;
    nextPersistAt = now + PersistIntervalMs;
    std::printf("DRAIN start interval=%lus capacity=%d nvs_interval=%lus nvs_capacity=%d\n",
                (unsigned long)(RecordIntervalMs / 1000), Capacity, (unsigned long)(PersistIntervalMs / 1000),
                PersistCapacity);
    std::fflush(stdout);
}

void stopRecord() {
    active = false;
    std::printf("DRAIN stop n=%d nvs_n=%u\n", count, persisted.count);
    std::fflush(stdout);
}

bool recording() { return active; }

void tickRecord(uint32_t now, const Context& context) {
    if (!active) return;
    const bool ram = static_cast<int32_t>(now - nextAt) >= 0;
    const bool nvs = static_cast<int32_t>(now - nextPersistAt) >= 0;
    if (!ram && !nvs) return;
    const auto s = sample();
    const auto state = stateBits(s, context);
    if (ram) {
        append(now, s, state);
        nextAt += RecordIntervalMs;
    }
    if (nvs) {
        persist(s, state);
        nextPersistAt += PersistIntervalMs;
    }
}

void dump() {
    if (count > 0) {
        std::printf("DRAIN source=ram interval=%lus\n", (unsigned long)(RecordIntervalMs / 1000));
        const int first = (head - count + Capacity) % Capacity;
        for (int i = 0; i < count; ++i) {
            const auto& e = entries[(first + i) % Capacity];
            std::printf("DRAIN t=%lu vbat=%d st=%u\n", (unsigned long)e.seconds, e.vbat, e.state);
        }
        std::printf("DRAIN end n=%d\n", count);
    } else {
        // Nothing in RAM: the chip restarted, e.g. after the battery ran flat.
        Persisted saved{};
        const bool found = loadPersisted(saved);
        const unsigned n = found ? saved.count : 0;
        std::printf("DRAIN source=nvs interval=%us\n", found ? saved.intervalS : 0);
        for (unsigned i = 0; i < n; ++i)
            std::printf("DRAIN t=%lu vbat=%d st=%u\n", (unsigned long)i * saved.intervalS, saved.vbat[i],
                        saved.state[i]);
        std::printf("DRAIN end n=%u\n", n);
    }
    std::fflush(stdout);
}

}  // namespace power
