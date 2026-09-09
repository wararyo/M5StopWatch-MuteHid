#pragma once
#include <cstdint>

// Host-reported mute state and the reflection window described in the
// specification section 5. Kept free of BLE and UI dependencies so the
// transitions can be exercised by tools/test_mute.cpp on a PC.
namespace domain {

enum class Host : int8_t { Unknown = -1, Unmuted = 0, Muted = 1 };
enum class Action : uint8_t { Blocked, Toggle, Mute };
enum class Report : uint8_t { Rejected, Unchanged, Changed };
enum class Expiry : uint8_t { None, Unresolved, NoChange };

// Adjustable initial value, not a protocol constant.
constexpr uint32_t ReflectionMs = 1000;

struct MuteState {
    bool connected = false;
    bool encrypted = false;
    bool subscribed = false;
    bool suspended = false;
    bool reportMode = true;
    Host host = Host::Unknown;
    bool pending = false;
    uint8_t requested = 0;
    Host believed = Host::Unknown;
    uint32_t deadline = 0, generation = 0, txCount = 0, rxCount = 0;

    bool ready() const { return connected && encrypted && subscribed && !suspended && reportMode; }

    // Phase 0 confirmed the host treats Input as an absolute state value, so an
    // unknown state cannot be inverted. Only the absolute "mute" request is left.
    Action action() const {
        if (!ready() || pending) return Action::Blocked;
        return host == Host::Unknown ? Action::Mute : Action::Toggle;
    }
    uint8_t valueFor(Action action) const {
        if (action == Action::Mute) return 1;
        return host == Host::Muted ? 0 : 1;
    }

    void connect(uint32_t connectionGeneration) {
        connected = true;
        encrypted = subscribed = suspended = pending = false;
        reportMode = true;
        host = believed = Host::Unknown;
        generation = connectionGeneration;
    }
    void disconnect() {
        connected = encrypted = subscribed = pending = false;
        host = believed = Host::Unknown;
    }
    void sent(uint8_t value, uint32_t now) {
        ++txCount;
        pending = true;
        requested = value;
        believed = host;
        deadline = now + ReflectionMs;
    }
    Report output(uint32_t reportGeneration, unsigned id, unsigned length, uint8_t value) {
        if (!connected || !encrypted || reportGeneration != generation || id != 1 || length != 1)
            return Report::Rejected;
        const Host reported = (value & 1) ? Host::Muted : Host::Unmuted;
        const bool changed = reported != host;
        host = reported;
        pending = false;
        ++rxCount;
        return changed ? Report::Changed : Report::Unchanged;
    }
    // Meet only reports a change, so a request that matched the known state is
    // expected to stay silent. Only a request that should have changed the host
    // makes the state unknown.
    Expiry tick(uint32_t now) {
        if (!pending || static_cast<int32_t>(now - deadline) < 0) return Expiry::None;
        pending = false;
        if (believed != Host::Unknown && requested == static_cast<uint8_t>(believed))
            return Expiry::NoChange;
        host = Host::Unknown;
        return Expiry::Unresolved;
    }
};

}  // namespace domain
