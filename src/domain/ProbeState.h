#pragma once
#include <cstdint>

struct ProbeState {
    bool connected = false;
    bool encrypted = false;
    bool subscribed = false;
    bool suspended = false;
    bool reportMode = true;
    bool pending = false;
    int hostMute = -1;
    uint32_t txCount = 0, rxCount = 0, deadline = 0, generation = 0;
    bool ready() const { return connected && encrypted && subscribed && !suspended && reportMode; }
    void connect(uint32_t gen) {
        connected = true;
        encrypted = subscribed = suspended = pending = false;
        reportMode = true;
        hostMute = -1;
        generation = gen;
    }
    void disconnect() {
        connected = encrypted = subscribed = pending = false;
        hostMute = -1;
    }
    bool output(uint32_t gen, unsigned id, unsigned length, uint8_t value) {
        if (!connected || !encrypted || gen != generation || id != 1 || length != 1)
            return false;
        hostMute = value & 1;
        pending = false;
        ++rxCount;
        return true;
    }
    void sent(uint32_t now) { ++txCount; pending = true; deadline = now + 1000; }
    bool tick(uint32_t now) {
        if (!pending || static_cast<int32_t>(now - deadline) < 0) return false;
        pending = false;
        hostMute = -1;
        return true;
    }
};

