#include "Renderer.h"
#include <M5Unified.h>
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "generated/Icons.h"

namespace {
using domain::Host;
using m5gfx::LovyanGFX;

// TFT_BLACK is an int literal; the drawing helpers need a single colour type.
constexpr uint16_t Background = TFT_BLACK;

// Drawing goes through a PSRAM canvas so a full repaint reaches the panel in
// one transfer instead of flashing through fillScreen. Without the canvas the
// same code draws straight to the panel.
M5Canvas canvas(&M5.Display);
bool buffered = false;
ui::View previous{};
bool valid = false;

LovyanGFX& target() {
    if (buffered) return canvas;
    return M5.Display;
}
void flush() {
    if (buffered) canvas.pushSprite(0, 0);
}

// Positions are derived from the panel size so the 466/468 px variants and the
// round cut-off both stay inside the drawn area.
int centreX() { return M5.Display.width() / 2; }
int centreY() { return M5.Display.height() / 2; }
int radius() { return (M5.Display.width() < M5.Display.height() ? M5.Display.width() : M5.Display.height()) / 2; }

constexpr int IconOffsetY = -34;   // Centre of the large icon, relative to cy.
constexpr int DotsOffsetY = 164;   // Reflection-wait animation, relative to cy.
constexpr int RowHeight = 46;
constexpr int RowsTop = -152;      // First settings row, relative to cy.

uint16_t stateColour(LovyanGFX& g, const ui::View& view) {
    if (!view.connected) return g.color565(120, 120, 130);
    if (!view.ready) return g.color565(150, 150, 160);
    switch (view.host) {
    case Host::Muted: return g.color565(240, 80, 70);
    case Host::Unmuted: return g.color565(60, 200, 110);
    default: return g.color565(240, 190, 60);
    }
}
uint16_t dim(uint16_t colour, int numerator, int denominator) {
    const int r = ((colour >> 11) & 0x1f) * numerator / denominator;
    const int g = ((colour >> 5) & 0x3f) * numerator / denominator;
    const int b = (colour & 0x1f) * numerator / denominator;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

const icons::Icon& stateIcon(const ui::View& view) {
    if (!view.connected || !view.ready) return icons::microphoneQuestion;
    switch (view.host) {
    case Host::Muted: return icons::microphoneOff;
    case Host::Unmuted: return icons::microphone;
    default: return icons::microphoneQuestion;
    }
}
const char* stateLabel(const ui::View& view) {
    if (!view.connected) return "OFFLINE";
    if (!view.ready) return "LINKING";
    switch (view.host) {
    case Host::Muted: return "MUTED";
    case Host::Unmuted: return "MIC ON";
    default: return "UNKNOWN";
    }
}
// The reported states and the reflection wait speak for themselves; only the
// states that would otherwise be cryptic carry a second line.
const char* stateDetail(const ui::View& view) {
    if (!view.connected) return "CONNECT FROM PC";
    if (!view.ready) return "SECURING LINK";
    if (view.pending) return nullptr;
    return view.host == Host::Unknown ? "NO REPORT YET" : nullptr;
}

void drawIcon(LovyanGFX& g, const icons::Icon& icon, int x, int y, uint16_t colour) {
    g.pushGrayscaleImage(x - icon.width / 2, y - icon.height / 2, icon.width, icon.height,
                         icon.alpha, m5gfx::grayscale_8bit, colour, Background);
}

void drawDots(LovyanGFX& g, const ui::View& view) {
    const int cx = centreX(), cy = centreY();
    const uint16_t on = g.color565(220, 220, 230), off = g.color565(70, 70, 80);
    for (int i = 0; i < 3; ++i)
        g.fillCircle(cx - 18 + i * 18, cy + DotsOffsetY, 5, i == view.phase % 3 ? on : off);
}

void drawStatusRow(LovyanGFX& g, const ui::View& view) {
    const int cx = centreX(), cy = centreY();
    const int y = cy - radius() + 62;
    const uint16_t colour = view.connected ? g.color565(120, 190, 255) : g.color565(110, 110, 120);
    // A filled dot for a usable link, a ring while it is not; no vendor logo.
    if (view.connected && view.ready) {
        g.fillCircle(cx - 92, y, 9, colour);
    } else {
        g.drawCircle(cx - 92, y, 9, colour);
        g.drawCircle(cx - 92, y, 8, colour);
    }
    g.setFont(&fonts::FreeSans12pt7b);
    g.setTextSize(1);
    g.setTextColor(colour, Background);
    g.setTextDatum(middle_left);
    g.drawString(view.connected ? (view.ready ? "READY" : "LINKING") : "OFFLINE", cx - 70, y);
    if (view.battery >= 0) {
        char text[16];
        std::snprintf(text, sizeof(text), "%s%d%%", view.charging ? "充電 " : "", view.battery);
        g.setTextDatum(middle_right);
        g.setTextColor(g.color565(200, 200, 210), Background);
        g.drawString(text, cx + 96, y);
    }
}

void drawMain(LovyanGFX& g, const ui::View& view) {
    const int cx = centreX(), cy = centreY();
    drawStatusRow(g, view);

    const uint16_t colour = stateColour(g, view);
    const uint16_t front = view.pending ? dim(colour, 2, 5) : colour;
    drawIcon(g, stateIcon(view), cx, cy + IconOffsetY, front);

    g.setFont(&fonts::FreeSansBold24pt7b);
    g.setTextSize(1);
    g.setTextDatum(middle_center);
    g.setTextColor(front, Background);
    g.drawString(stateLabel(view), cx, cy + 92);

    if (const char* detail = stateDetail(view)) {
        g.setFont(&fonts::FreeSans12pt7b);
        g.setTextColor(g.color565(220, 220, 230), Background);
        g.drawString(detail, cx, cy + 134);
    }

    if (view.pending) {
        drawDots(g, view);
    } else if (view.note) {
        g.setFont(&fonts::FreeSans12pt7b);
        g.setTextColor(g.color565(170, 170, 180), Background);
        g.drawString(view.note, cx, cy + DotsOffsetY);
    }

    g.setFont(&fonts::FreeSans9pt7b);
    g.setTextColor(g.color565(120, 120, 130), Background);
    g.drawString("A: TOGGLE  B: SETTINGS", cx, cy + 196);
}

void drawPairing(LovyanGFX& g, const ui::View& view) {
    const int cx = centreX(), cy = centreY();
    char text[16];
    std::snprintf(text, sizeof(text), "%06lu", static_cast<unsigned long>(view.passkey));
    g.setTextDatum(middle_center);
    g.setFont(&fonts::FreeSans12pt7b);
    g.setTextSize(1);
    g.setTextColor(g.color565(220, 220, 230), Background);
    g.drawString("PAIRING", cx, cy - 120);
    g.setFont(&fonts::FreeSansBold24pt7b);
    g.setTextColor(g.color565(120, 190, 255), Background);
    g.setTextSize(2);
    g.drawString(text, cx, cy - 30);
    g.setTextSize(1);
    g.setFont(&fonts::FreeSans12pt7b);
    g.setTextColor(g.color565(220, 220, 230), Background);
    g.drawString("SAME NUMBER ON THE PC?", cx, cy + 60);
    g.drawString("A: YES        B: NO", cx, cy + 130);
}

void itemText(const ui::View& view, uint8_t index, char* out, size_t size) {
    switch (static_cast<ui::Item>(index)) {
    case ui::Item::Brightness: std::snprintf(out, size, "Brightness  %u%%", view.brightness); break;
    case ui::Item::Vibration: std::snprintf(out, size, "Vibration  %s", view.vibration ? "ON" : "OFF"); break;
    case ui::Item::Pair: std::snprintf(out, size, "Pair a new PC"); break;
    case ui::Item::Forget: std::snprintf(out, size, "Remove the bond"); break;
    case ui::Item::Diagnostics: std::snprintf(out, size, "Diagnostics"); break;
    case ui::Item::Reset: std::snprintf(out, size, "Reset settings"); break;
    case ui::Item::UserDemo:
        std::snprintf(out, size, "%s", view.coexist ? "Return to UserDemo" : "UserDemo (standalone: n/a)");
        break;
    default: out[0] = '\0'; break;
    }
}

void drawSettings(LovyanGFX& g, const ui::View& view) {
    const int cx = centreX(), cy = centreY();
    g.setTextDatum(middle_center);
    g.setTextSize(1);
    g.setFont(&fonts::FreeSans12pt7b);
    g.setTextColor(g.color565(220, 220, 230), Background);
    g.drawString("SETTINGS", cx, cy + RowsTop - 44);

    for (uint8_t index = 0; index < ui::ItemCount; ++index) {
        const int y = cy + RowsTop + index * RowHeight;
        const bool selected = index == view.cursor;
        const bool confirming = view.confirm == index + 1;
        const uint16_t background = selected ? g.color565(30, 40, 60) : Background;
        if (selected) g.fillRoundRect(cx - 178, y - 20, 356, 40, 10, background);
        char text[48];
        itemText(view, index, text, sizeof(text));
        g.setFont(&fonts::FreeSans12pt7b);
        g.setTextColor(confirming ? g.color565(240, 190, 60) : g.color565(220, 220, 230), background);
        g.drawString(confirming ? "TAP AGAIN TO CONFIRM" : text, cx, y);
    }
    g.setFont(&fonts::FreeSans12pt7b);
    g.setTextColor(g.color565(120, 120, 130), Background);
    g.drawString("B: CLOSE", cx, cy + RowsTop + ui::ItemCount * RowHeight + 14);
}

void drawDiagnostics(LovyanGFX& g, const ui::View& view) {
    const int cx = centreX(), cy = centreY();
    g.setTextDatum(middle_center);
    g.setTextSize(1);
    g.setFont(&fonts::FreeSans12pt7b);
    g.setTextColor(g.color565(220, 220, 230), Background);
    g.drawString("DIAGNOSTICS", cx, cy - 150);
    g.setFont(&fonts::FreeMono9pt7b);
    g.setTextDatum(middle_left);
    g.setTextColor(g.color565(200, 200, 210), Background);
    for (uint8_t line = 0; line < ui::DiagnosticLines; ++line)
        if (view.diagnostics[line][0])
            g.drawString(view.diagnostics[line], cx - 132, cy - 100 + line * 26);
    g.setTextDatum(middle_center);
    g.setTextColor(g.color565(120, 120, 130), Background);
    g.drawString("B: BACK", cx, cy + 150);
}

// True when the frames differ only in the reflection-wait animation.
bool onlyPhaseChanged(const ui::View& view) {
    ui::View comparable = view;
    comparable.phase = previous.phase;
    return view.phase != previous.phase && std::memcmp(&comparable, &previous, sizeof(ui::View)) == 0;
}
}  // namespace

namespace ui {

void begin() {
    valid = false;
    canvas.setPsram(true);
    canvas.setColorDepth(16);
    buffered = canvas.createSprite(M5.Display.width(), M5.Display.height()) != nullptr;
    if (!buffered) ESP_LOGW("Renderer", "No canvas memory; drawing directly");
    M5.Display.setTextWrap(false);
    M5.Display.fillScreen(Background);
}

void render(const View& view, bool force) {
    if (!force && valid) {
        if (std::memcmp(&view, &previous, sizeof(View)) == 0) return;
        if (view.screen == Screen::Main && view.pending && onlyPhaseChanged(view)) {
            // Only the dots move: touch the panel where they are, not the whole
            // screen, and keep the canvas in step for the next full repaint.
            if (buffered) drawDots(canvas, view);
            drawDots(M5.Display, view);
            previous = view;
            return;
        }
    }
    previous = view;
    valid = true;
    LovyanGFX& g = target();
    g.startWrite();
    g.fillScreen(Background);
    switch (view.screen) {
    case Screen::Main: drawMain(g, view); break;
    case Screen::Pairing: drawPairing(g, view); break;
    case Screen::Settings: drawSettings(g, view); break;
    case Screen::Diagnostics: drawDiagnostics(g, view); break;
    }
    g.endWrite();
    flush();
}

bool hitMute(int x, int y) {
    const int dx = x - centreX();
    const int dy = y - (centreY() + IconOffsetY);
    return dx * dx + dy * dy <= 118 * 118;
}

int hitItem(int x, int y) {
    if (x < centreX() - 178 || x > centreX() + 178) return -1;
    const int offset = y - (centreY() + RowsTop) + RowHeight / 2;
    if (offset < 0) return -1;
    const int index = offset / RowHeight;
    return index < ItemCount ? index : -1;
}

}  // namespace ui
