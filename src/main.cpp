#include <M5Unified.h>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "ble/TelephonyHid.h"
#include "domain/ProbeState.h"
#include "app/FirmwareSwitch.h"

namespace {
ProbeState state;
bool pairing = false;
uint32_t passkey = 0, pairDeadline = 0;
bool pulseRelease = false;
uint32_t pulseAt = 0, pulseDeadline = 0;
const char* message = "Waiting for PC";
uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
void status() {
    ESP_LOGI("Probe", "STATUS conn=%d auth=%d ccc=%d ready=%d host=%d pending=%d tx=%lu rx=%lu gen=%lu",
        state.connected, state.encrypted, state.subscribed, state.ready(), state.hostMute, state.pending,
        (unsigned long)state.txCount, (unsigned long)state.rxCount, (unsigned long)state.generation);
    nvs_stats_t stats{};
    if (nvs_get_stats(nullptr, &stats) == ESP_OK)
        ESP_LOGI("Probe", "NVS used=%u free=%u total=%u namespaces=%u", (unsigned)stats.used_entries,
                 (unsigned)stats.free_entries, (unsigned)stats.total_entries, (unsigned)stats.namespace_count);
}
bool transmit(uint8_t value, bool release = false) {
    if (!state.ready() || (!release && (state.pending || pulseRelease))) {
        message = "Not ready / awaiting host";
        ESP_LOGW("Probe", "INPUT rejected: ready=%d pending=%d", state.ready(), state.pending);
        return false;
    }
    const auto err = telephony::send(value);
    if (err != ESP_OK) { message = "Send failed"; return false; }
    state.sent(nowMs());
    message = value ? "Sent RAW 1 (not confirmed)" : "Sent RAW 0 (not confirmed)";
    return true;
}
void goBack() {
    if (pulseRelease && state.ready()) transmit(0, true);
    telephony::stop();
    M5.Power.setVibration(0);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!firmware::returnToUserDemo()) message = "UserDemo unavailable; reboot";
}
void command(char c) {
    if (pairing) {
        if (c == 'y' || c == 'n') {
            telephony::confirm(c == 'y'); pairing = false; message = "Pairing result pending";
        }
        return;
    }
    switch (c) {
    case '0': transmit(0); break;
    case '1': transmit(1); break;
    case 't':
        if (state.hostMute < 0) message = "Unknown: use explicit RAW 0/1";
        else transmit(state.hostMute ? 0 : 1);
        break;
    case 'p':
        if (transmit(1)) { pulseRelease = true; pulseAt = nowMs() + 80; pulseDeadline = nowMs() + 1000; }
        break;
    case 's': status(); break;
    case 'f': telephony::forget(); message = "Forget requested (offline only)"; break;
    case 'u': goBack(); break;
    case 'h':
        ESP_LOGI("Probe", "COMMANDS 0/1=raw input, t=invert known host, p=1 then 0, s=status, y/n=pair, f=forget offline, u=UserDemo"); break;
    default: break;
    }
}
void draw() {
    const int cx = M5.Display.width() / 2;
    const int cy = M5.Display.height() / 2;
    M5.Display.startWrite();
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.drawString("MuteHid PHASE 0", cx, cy - 152);
    char line[90];
    std::snprintf(line, sizeof(line), "BLE %d  ENC %d  CCC %d", state.connected, state.encrypted, state.subscribed);
    M5.Display.drawString(line, cx, cy - 104);
    M5.Display.setTextSize(4);
    const char* label = pairing ? "PAIR" : state.pending ? "WAIT" : state.hostMute < 0 ? "UNKNOWN" : state.hostMute ? "MUTED" : "MIC ON";
    M5.Display.setTextColor(state.hostMute < 0 ? TFT_YELLOW : state.hostMute ? TFT_RED : TFT_GREEN, TFT_BLACK);
    M5.Display.drawString(label, cx, cy - 35);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(2);
    if (pairing) {
        std::snprintf(line, sizeof(line), "%06lu", (unsigned long)passkey);
        M5.Display.drawString(line, cx, cy + 15);
        M5.Display.drawString("Compare with Windows", cx, cy + 50);
        M5.Display.drawString("A: YES    B: NO", cx, cy + 95);
    } else {
        M5.Display.drawString("Host-reported state", cx, cy + 15);
        std::snprintf(line, sizeof(line), "TX %lu    RX %lu", (unsigned long)state.txCount, (unsigned long)state.rxCount);
        M5.Display.drawString(line, cx, cy + 55);
        M5.Display.setTextSize(1);
        M5.Display.drawString(message, cx, cy + 87);
        M5.Display.setTextSize(2);
        M5.Display.drawString("A: RAW 1  B: RAW 0", cx, cy + 117);
    }
    M5.Display.setTextSize(1);
    M5.Display.drawString("A+B 3 sec: UserDemo", cx, cy + 157);
    M5.Display.endWrite();
}
}

extern "C" void app_main() {
    firmware::checkStartupEscape();
    auto cfg = M5.config();
    cfg.internal_imu = cfg.internal_rtc = cfg.internal_mic = cfg.internal_spk = false;
    M5.begin(cfg);
    M5.Display.setBrightness(90);
    ESP_LOGI("Probe", "BOOT %s %s IDF=%s", esp_app_get_description()->project_name,
             esp_app_get_description()->version, esp_app_get_description()->idf_ver);
    const auto nvsErr = nvs_flash_init();
    if (nvsErr != ESP_OK) {
        ESP_LOGE("Probe", "NVS init failed: %s; NEVER erasing shared NVS", esp_err_to_name(nvsErr));
        message = "NVS error: shared data preserved";
        draw();
        return;
    }
    const auto err = telephony::begin();
    if (err != ESP_OK) { ESP_LOGE("Probe", "BLE init: %s", esp_err_to_name(err)); message = "BLE initialization failed"; draw(); return; }
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    command('h'); status();
    bool aSeen = false, bSeen = false, chord = false, holding = false;
    uint32_t heldSince = 0, lastDraw = 0;
    for (;;) {
        M5.update();
        const uint32_t now = nowMs();
        telephony::Event event;
        while (telephony::poll(event)) {
            if (event.kind != telephony::Kind::Connected && event.kind != telephony::Kind::Started && event.generation != state.generation) continue;
            switch (event.kind) {
            case telephony::Kind::Connected: state.connect(event.generation); message = "Connected; waiting for security"; break;
            case telephony::Kind::Disconnected: state.disconnect(); pairing = pulseRelease = false; message = "Disconnected"; break;
            case telephony::Kind::Auth: state.encrypted = event.value != 0; pairing = false; message = "Security result received"; break;
            case telephony::Kind::Subscribe: state.subscribed = event.value != 0; message = "Input subscription changed"; break;
            case telephony::Kind::Output:
                if (state.output(event.generation, event.id, event.length, event.value)) message = "Host OUTPUT received";
                break;
            case telephony::Kind::Numeric: pairing = true; passkey = event.value; pairDeadline = now + 30000; break;
            case telephony::Kind::Passkey: ESP_LOGI("Probe", "PASSKEY %06lu", (unsigned long)event.value); break;
            case telephony::Kind::Control: state.suspended = event.value == 0; break;
            case telephony::Kind::Protocol: state.reportMode = event.value == 1; break;
            case telephony::Kind::Error: state.hostMute = -1; state.pending = false; message = "GATT error"; break;
            default: break;
            }
            status();
        }
        if (telephony::overflowed()) {
            state.hostMute = -1; state.pending = false; state.subscribed = false;
            telephony::stop(); message = "Event overflow: reboot required";
            ESP_LOGE("Probe", "Event queue overflow; stopped BLE");
        }
        if (pairing && static_cast<int32_t>(now - pairDeadline) >= 0) { telephony::confirm(false); pairing = false; }
        if (state.tick(now)) { message = "No host OUTPUT; state unknown"; ESP_LOGW("Probe", "HOST_TIMEOUT: no automatic toggle retry"); }
        if (pulseRelease && static_cast<int32_t>(now - pulseAt) >= 0) {
            if (!state.ready()) { pulseRelease = false; telephony::stop(); message = "Release unavailable; reconnect"; }
            else if (transmit(0, true)) pulseRelease = false;
            else if (static_cast<int32_t>(now - pulseDeadline) >= 0) { pulseRelease = false; telephony::stop(); message = "Release failed; reconnect"; }
            else pulseAt = now + 50;
        }
        const bool a = M5.BtnA.isPressed(), b = M5.BtnB.isPressed();
        const bool touch = M5.Touch.getDetail().isPressed();
        if (a) aSeen = true;
        if (b) bSeen = true;
        if (a && b) chord = true;
        if (a && b && !touch) {
            if (!holding) { holding = true; heldSince = now; }
            else if (now - heldSince >= 3000) { goBack(); holding = false; }
        } else holding = false;
        if (!a && !b) {
            if (!chord) {
                if (aSeen) command(pairing ? 'y' : '1');
                else if (bSeen) command(pairing ? 'n' : '0');
            }
            aSeen = bSeen = chord = false;
        }
        char c;
        for (int i = 0; i < 16 && read(STDIN_FILENO, &c, 1) == 1; ++i) command(c);
        if (now - lastDraw >= 200) { draw(); lastDraw = now; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
