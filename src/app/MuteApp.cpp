#include "MuteApp.h"
#include <M5Unified.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include "app/FirmwareSwitch.h"
#include "ble/TelephonyHid.h"
#include "domain/MuteState.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "storage/Settings.h"
#include "ui/Renderer.h"

namespace {
constexpr const char* Tag = "MuteApp";

constexpr uint32_t IdleDimMs = 30000;
constexpr uint32_t NoteMs = 4000;
constexpr uint32_t ConfirmMs = 5000;
constexpr uint32_t PairingMs = 30000;
constexpr uint32_t VibrationMs = 20;
constexpr uint32_t BatteryMs = 30000;
constexpr uint32_t ChordMs = 3000;
// One physical press and the tap it may also produce must not toggle twice.
constexpr uint32_t ActionGuardMs = 250;
constexpr int TapSlack = 24;

domain::MuteState state;
storage::Settings settings;
ui::Screen screen = ui::Screen::Main;

bool pairing = false, dimmed = false, wakeGuard = false, forgetOnDisconnect = false;
bool touchValid = false;
int touchX = 0, touchY = 0;
uint8_t cursor = 0, confirm = 0;
uint32_t passkey = 0;
uint32_t pairDeadline = 0, noteDeadline = 0, confirmDeadline = 0;
uint32_t lastInput = 0, lastAction = 0, vibrationUntil = 0, batteryAt = 0, diagnosticsAt = 0;
const char* note = nullptr;
int8_t batteryLevel = -1;
bool charging = false;
char diagnostics[ui::DiagnosticLines][ui::DiagnosticWidth]{};

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void setNote(const char* text) {
    note = text;
    noteDeadline = nowMs() + NoteMs;
}
void vibrate() {
    if (!settings.vibration) return;
    M5.Power.setVibration(100);
    vibrationUntil = nowMs() + VibrationMs;
}
void applyBrightness() {
    M5.Display.setBrightness(dimmed ? (settings.level() / 4 > 15 ? settings.level() / 4 : 15)
                                    : settings.level());
}
void wake() {
    lastInput = nowMs();
    if (!dimmed) return;
    dimmed = false;
    applyBrightness();
}
void status() {
    ESP_LOGI(Tag, "STATUS conn=%d auth=%d ccc=%d ready=%d host=%d pending=%d tx=%lu rx=%lu gen=%lu",
             state.connected, state.encrypted, state.subscribed, state.ready(),
             static_cast<int>(state.host), state.pending, (unsigned long)state.txCount,
             (unsigned long)state.rxCount, (unsigned long)state.generation);
}

void leaveToUserDemo() {
    // Nothing is held down in the state-value scheme, so there is no release
    // report to send; drop the link and stop the motor before switching.
    telephony::stop();
    M5.Power.setVibration(0);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (!firmware::returnToUserDemo()) setNote("UserDemo unavailable");
}

bool sendValue(uint8_t value) {
    const auto err = telephony::send(value);
    if (err != ESP_OK) {
        ESP_LOGW(Tag, "SEND failed: %s", esp_err_to_name(err));
        setNote("Send failed");
        return false;
    }
    state.sent(value, nowMs());
    return true;
}

void muteAction() {
    const uint32_t now = nowMs();
    if (now - lastAction < ActionGuardMs) return;
    lastAction = now;
    switch (state.action()) {
    case domain::Action::Blocked:
        setNote(!state.connected ? "Not connected"
                                 : state.pending ? "Waiting for the PC" : "Not ready yet");
        return;
    case domain::Action::Mute:
        if (sendValue(1)) setNote("Mute requested");
        return;
    case domain::Action::Toggle:
        sendValue(state.valueFor(domain::Action::Toggle));
        return;
    }
}

// Diagnostic sends that bypass the toggle rule; used to exercise the
// "same value as the known state" path from the serial console.
void rawSend(uint8_t value) {
    if (state.action() == domain::Action::Blocked) {
        setNote("Cannot send now");
        ESP_LOGW(Tag, "RAW rejected: ready=%d pending=%d", state.ready(), state.pending);
        return;
    }
    sendValue(value);
}

void executeItem(ui::Item item) {
    switch (item) {
    case ui::Item::Pair:
        if (state.connected) {
            forgetOnDisconnect = true;
            telephony::disconnectPeer();
            setNote("Disconnecting to unpair");
        } else {
            telephony::forget();
            setNote("Bond removed");
        }
        screen = ui::Screen::Main;
        break;
    case ui::Item::Forget:
        if (state.connected) {
            setNote("Disconnect on the PC first");
        } else {
            telephony::forget();
            setNote("Bond removed");
        }
        break;
    case ui::Item::Reset:
        storage::reset();
        settings = storage::Settings{};
        applyBrightness();
        setNote("Settings reset");
        break;
    case ui::Item::UserDemo:
        leaveToUserDemo();
        break;
    default:
        break;
    }
}

void activateItem(uint8_t index) {
    const auto item = static_cast<ui::Item>(index);
    switch (item) {
    case ui::Item::Brightness:
        settings.brightness = (settings.brightness + 1) % storage::BrightnessCount;
        applyBrightness();
        storage::save(settings);
        confirm = 0;
        return;
    case ui::Item::Vibration:
        settings.vibration = !settings.vibration;
        storage::save(settings);
        vibrate();
        confirm = 0;
        return;
    case ui::Item::Diagnostics:
        screen = ui::Screen::Diagnostics;
        confirm = 0;
        return;
    default:
        break;
    }
    // Pairing, bond removal, settings reset and the UserDemo switch each need a
    // second deliberate tap.
    if (confirm != index + 1) {
        confirm = index + 1;
        confirmDeadline = nowMs() + ConfirmMs;
        return;
    }
    confirm = 0;
    executeItem(item);
}

void onButtonA() {
    switch (screen) {
    case ui::Screen::Pairing: telephony::confirm(true); pairing = false; screen = ui::Screen::Main; setNote("Pairing accepted"); break;
    case ui::Screen::Main: muteAction(); break;
    case ui::Screen::Settings: cursor = (cursor + 1) % ui::ItemCount; confirm = 0; break;
    case ui::Screen::Diagnostics: break;
    }
}
void onButtonB() {
    switch (screen) {
    case ui::Screen::Pairing: telephony::confirm(false); pairing = false; screen = ui::Screen::Main; setNote("Pairing rejected"); break;
    case ui::Screen::Main: screen = ui::Screen::Settings; cursor = 0; confirm = 0; break;
    case ui::Screen::Settings: screen = ui::Screen::Main; confirm = 0; break;
    case ui::Screen::Diagnostics: screen = ui::Screen::Settings; break;
    }
}
void onTap(int x, int y) {
    switch (screen) {
    case ui::Screen::Main:
        if (ui::hitMute(x, y)) muteAction();
        break;
    case ui::Screen::Settings: {
        const int index = ui::hitItem(x, y);
        if (index < 0) { confirm = 0; break; }
        if (confirm && confirm != index + 1) confirm = 0;
        cursor = static_cast<uint8_t>(index);
        activateItem(static_cast<uint8_t>(index));
        break;
    }
    default:
        break;
    }
}

void command(char c) {
    switch (c) {
    case 'a': onButtonA(); break;
    case 'b': onButtonB(); break;
    case '0': rawSend(0); break;
    case '1': rawSend(1); break;
    case 's': status(); break;
    case 'y': if (pairing) onButtonA(); break;
    case 'n': if (pairing) onButtonB(); break;
    case 'u': leaveToUserDemo(); break;
    case 'h':
        ESP_LOGI(Tag, "COMMANDS a/b=buttons, 0/1=raw input, s=status, y/n=pairing, u=UserDemo");
        break;
    default: break;
    }
}

void handleEvent(const telephony::Event& event) {
    const uint32_t now = nowMs();
    switch (event.kind) {
    case telephony::Kind::Connected:
        state.connect(event.generation);
        setNote("Connected");
        break;
    case telephony::Kind::Disconnected:
        state.disconnect();
        pairing = false;
        if (screen == ui::Screen::Pairing) screen = ui::Screen::Main;
        if (forgetOnDisconnect) {
            forgetOnDisconnect = false;
            telephony::forget();
            setNote("Bond removed");
        } else {
            setNote("Disconnected");
        }
        break;
    case telephony::Kind::Auth:
        state.encrypted = event.value != 0;
        pairing = false;
        if (screen == ui::Screen::Pairing) screen = ui::Screen::Main;
        if (!state.encrypted) setNote("Pairing failed");
        break;
    case telephony::Kind::Subscribe:
        state.subscribed = event.value != 0;
        break;
    case telephony::Kind::Output:
        switch (state.output(event.generation, event.id, event.length, static_cast<uint8_t>(event.value))) {
        case domain::Report::Changed:
            note = nullptr;
            wake();
            vibrate();
            break;
        case domain::Report::Unchanged:
            note = nullptr;
            break;
        case domain::Report::Rejected:
            ESP_LOGW(Tag, "OUTPUT rejected id=%u len=%u gen=%lu", event.id, event.length,
                     (unsigned long)event.generation);
            break;
        }
        break;
    case telephony::Kind::Numeric:
        pairing = true;
        passkey = event.value;
        screen = ui::Screen::Pairing;
        pairDeadline = now + PairingMs;
        wake();
        break;
    case telephony::Kind::Passkey:
        ESP_LOGI(Tag, "PASSKEY %06lu", (unsigned long)event.value);
        break;
    case telephony::Kind::Control:
        state.suspended = event.value == 0;
        break;
    case telephony::Kind::Protocol:
        state.reportMode = event.value == 1;
        break;
    case telephony::Kind::Error:
        state.host = domain::Host::Unknown;
        state.pending = false;
        setNote("GATT error");
        break;
    default:
        break;
    }
    status();
}

void refreshDiagnostics() {
    auto line = [](int index, const char* format, ...) {
        va_list args;
        va_start(args, format);
        vsnprintf(diagnostics[index], ui::DiagnosticWidth, format, args);
        va_end(args);
    };
    line(0, "conn=%d auth=%d ccc=%d ready=%d", state.connected, state.encrypted, state.subscribed,
         state.ready());
    line(1, "host=%s pending=%d",
         state.host == domain::Host::Unknown ? "unknown" : state.host == domain::Host::Muted ? "muted" : "unmuted",
         state.pending);
    line(2, "tx=%lu rx=%lu gen=%lu", (unsigned long)state.txCount, (unsigned long)state.rxCount,
         (unsigned long)state.generation);
    char peer[24];
    line(3, "bonds=%d %s", telephony::bonds(),
         telephony::peerText(peer, sizeof(peer)) ? peer : "-");
    nvs_stats_t stats{};
    if (nvs_get_stats(nullptr, &stats) == ESP_OK)
        line(4, "nvs used=%u free=%u ns=%u", (unsigned)stats.used_entries, (unsigned)stats.free_entries,
             (unsigned)stats.namespace_count);
    const auto* description = esp_app_get_description();
    line(5, "fw %s", description->version);
    line(6, "idf %s", description->idf_ver);
    line(7, "%s", MUTEHID_COEXIST ? "coexist build (ota_1)" : "standalone build");
}

void sampleBattery() {
    const int level = M5.Power.getBatteryLevel();
    batteryLevel = static_cast<int8_t>(level);
    charging = M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
    if (level >= 0) telephony::battery(static_cast<uint8_t>(level));
}
}  // namespace

namespace app {

void run() {
    storage::load(settings);
    ui::begin();
    applyBrightness();
    lastInput = nowMs();
    sampleBattery();
    command('h');
    status();

    bool aSeen = false, bSeen = false, chord = false, holding = false;
    uint32_t heldSince = 0;

    for (;;) {
        M5.update();
        const uint32_t now = nowMs();

        telephony::Event event;
        while (telephony::poll(event)) {
            if (event.kind != telephony::Kind::Connected && event.kind != telephony::Kind::Started &&
                event.generation != state.generation)
                continue;
            handleEvent(event);
        }
        if (telephony::overflowed()) {
            state.host = domain::Host::Unknown;
            state.pending = false;
            state.subscribed = false;
            telephony::stop();
            setNote("Event overflow: reboot");
            ESP_LOGE(Tag, "Event queue overflow; stopped BLE");
        }

        switch (state.tick(now)) {
        case domain::Expiry::Unresolved:
            setNote("State unconfirmed");
            ESP_LOGW(Tag, "HOST_TIMEOUT: no automatic toggle retry");
            break;
        case domain::Expiry::NoChange:
            setNote("No change");
            ESP_LOGI(Tag, "HOST_SILENT: request matched the known state");
            break;
        default:
            break;
        }

        if (pairing && static_cast<int32_t>(now - pairDeadline) >= 0) {
            telephony::confirm(false);
            pairing = false;
            screen = ui::Screen::Main;
            setNote("Pairing timed out");
        }
        if (confirm && static_cast<int32_t>(now - confirmDeadline) >= 0) confirm = 0;
        if (note && static_cast<int32_t>(now - noteDeadline) >= 0) note = nullptr;
        if (vibrationUntil && static_cast<int32_t>(now - vibrationUntil) >= 0) {
            M5.Power.setVibration(0);
            vibrationUntil = 0;
        }
        if (!batteryAt || now - batteryAt >= BatteryMs) {
            batteryAt = now;
            sampleBattery();
        }
        if (screen == ui::Screen::Diagnostics && (!diagnosticsAt || now - diagnosticsAt >= 500)) {
            diagnosticsAt = now;
            refreshDiagnostics();
        }

        const bool a = M5.BtnA.isPressed(), b = M5.BtnB.isPressed();
        const auto touch = M5.Touch.getDetail();
        const bool touching = touch.isPressed();
        // Buttons work straight through a dimmed screen. A touch that only wakes
        // it is swallowed, including on the release after wakeGuard is cleared.
        bool guard = wakeGuard;
        if (a || b || touching) {
            lastInput = now;
            if (dimmed) {
                const bool byTouch = touching && !a && !b;
                wake();
                if (byTouch) wakeGuard = guard = true;
            }
        } else {
            wakeGuard = false;
        }
        if (!dimmed && now - lastInput >= IdleDimMs) {
            dimmed = true;
            applyBrightness();
        }

        if (a) aSeen = true;
        if (b) bSeen = true;
        if (a && b) chord = true;
        if (a && b && !touching) {
            if (!holding) {
                holding = true;
                heldSince = now;
            } else if (now - heldSince >= ChordMs) {
                holding = false;
                leaveToUserDemo();
            }
        } else {
            holding = false;
        }
        if (!a && !b) {
            if (!chord) {
                if (aSeen) onButtonA();
                else if (bSeen) onButtonB();
            }
            aSeen = bSeen = chord = false;
        }

        if (touch.wasPressed()) {
            touchX = touch.x;
            touchY = touch.y;
            touchValid = true;
        } else if (touching && touchValid &&
                   (std::abs(touch.x - touchX) > TapSlack || std::abs(touch.y - touchY) > TapSlack)) {
            touchValid = false;  // Dragged: not a tap.
        }
        if (touch.wasReleased()) {
            if (touchValid && !guard && !chord) onTap(touchX, touchY);
            touchValid = false;
        }

        char c;
        for (int i = 0; i < 16 && read(STDIN_FILENO, &c, 1) == 1; ++i) command(c);

        ui::View view{};
        view.screen = screen;
        view.connected = state.connected;
        view.ready = state.ready();
        view.pending = state.pending;
        view.coexist = MUTEHID_COEXIST;
        view.vibration = settings.vibration;
        view.host = state.host;
        view.note = note;
        view.phase = state.pending ? static_cast<uint8_t>((now / 250) % 3) : 0;
        view.battery = batteryLevel;
        view.charging = charging;
        view.passkey = passkey;
        view.cursor = cursor;
        view.confirm = confirm;
        view.brightness = settings.percent();
        if (screen == ui::Screen::Diagnostics) std::memcpy(view.diagnostics, diagnostics, sizeof(diagnostics));
        ui::render(view);

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace app
