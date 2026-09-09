#pragma once
#include <cstdint>
#include "domain/MuteState.h"

namespace ui {

enum class Screen : uint8_t { Main, Pairing, Settings, Diagnostics };

// Settings items, in display order.
enum class Item : uint8_t { Brightness, Vibration, Pair, Forget, Diagnostics, Reset, UserDemo, Count };
constexpr uint8_t ItemCount = static_cast<uint8_t>(Item::Count);

constexpr uint8_t DiagnosticLines = 8;
constexpr uint8_t DiagnosticWidth = 44;

// Plain data so the renderer can skip redrawing an unchanged frame. Always
// build it with `View view{}` so the padding compares equal too.
struct View {
    Screen screen;
    bool connected, ready, pending, coexist, vibration, charging;
    domain::Host host;
    const char* note;      // String literal; compared by pointer.
    uint8_t phase;         // Animation step for the reflection wait.
    int8_t battery;
    uint32_t passkey;
    uint8_t cursor;        // Highlighted settings row.
    uint8_t confirm;       // Item index + 1 awaiting a second tap, else 0.
    uint8_t brightness;    // Percent for display.
    char diagnostics[DiagnosticLines][DiagnosticWidth];
};

void begin();
void render(const View& view, bool force = false);

// Hit tests, in display coordinates.
bool hitMute(int x, int y);
int hitItem(int x, int y);

}  // namespace ui
