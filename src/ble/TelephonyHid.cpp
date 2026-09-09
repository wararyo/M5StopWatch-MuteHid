#include "TelephonyHid.h"
#include "ReportMap.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "esp_mac.h"

namespace {
constexpr const char* Tag = "Telephony";
QueueHandle_t events;
std::atomic<bool> lost{false};
std::atomic<bool> connected{false}, encrypted{false}, subscribed{false};
std::atomic<bool> numericPending{false}, stopping{false};
std::atomic<uint32_t> generation{0};
esp_hidd_dev_t* device = nullptr;
esp_gatt_if_t hidIf = ESP_GATT_IF_NONE;
uint16_t inputHandle = 0, cccHandle = 0, outputHandle = 0, controlHandle = 0, protocolHandle = 0;
uint16_t connectionId = 0;
int inputIndex = -1, cccIndex = -1, outputIndex = -1, controlIndex = -1, protocolIndex = -1;
esp_bd_addr_t peer{}, confirmationPeer{};
bool advDataReady = false, scanReady = false, serviceReady = false;
// Bluedroid's config_adv_data API requires UUIDs as 128-bit little endian,
// even when it emits the Bluetooth-base UUID as a 16-bit advertising field.
uint8_t serviceUuid[] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                         0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00};
esp_ble_adv_params_t advertising{};

void emit(telephony::Kind kind, uint32_t value = 0, uint16_t len = 0, uint16_t id = 0) {
    telephony::Event e{kind, generation.load(), value, len, id};
    if (xQueueSend(events, &e, 0) != pdTRUE) lost.store(true);
}
void advertise() {
    if (advDataReady && scanReady && serviceReady && !connected && !stopping) {
        const auto err = esp_ble_gap_start_advertising(&advertising);
        ESP_LOGI(Tag, "ADV request=%s", esp_err_to_name(err));
    }
}
bool allowedPeer(const uint8_t* address) {
    int count = esp_ble_get_bond_device_num();
    if (!count) return true;
    std::vector<esp_ble_bond_dev_t> bonds(count);
    if (esp_ble_get_bond_device_list(&count, bonds.data()) != ESP_OK) return false;
    for (const auto& b : bonds) if (!std::memcmp(b.bd_addr, address, 6)) return true;
    return false;
}
void gap(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* p) {
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        advDataReady = p->adv_data_cmpl.status == ESP_BT_STATUS_SUCCESS; advertise(); break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        scanReady = p->scan_rsp_data_cmpl.status == ESP_BT_STATUS_SUCCESS; advertise(); break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        ESP_LOGI(Tag, "ADV started status=%d", p->adv_start_cmpl.status); break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr, allowedPeer(p->ble_security.ble_req.bd_addr)); break;
    case ESP_GAP_BLE_NC_REQ_EVT:
        std::memcpy(confirmationPeer, p->ble_security.key_notif.bd_addr, 6);
        numericPending = true;
        ESP_LOGI(Tag, "PAIR compare=%06lu; A/serial y accepts, B/serial n rejects", (unsigned long)p->ble_security.key_notif.passkey);
        emit(telephony::Kind::Numeric, p->ble_security.key_notif.passkey); break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        emit(telephony::Kind::Passkey, p->ble_security.key_notif.passkey); break;
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        esp_ble_passkey_reply(p->ble_security.ble_req.bd_addr, false, 0); break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (!connected || std::memcmp(peer, p->ble_security.auth_cmpl.bd_addr, 6)) break;
        encrypted = p->ble_security.auth_cmpl.success;
        numericPending = false;
        ESP_LOGI(Tag, "AUTH success=%d reason=0x%02x mode=0x%02x bonds=%d", (int)encrypted.load(),
                 p->ble_security.auth_cmpl.fail_reason, p->ble_security.auth_cmpl.auth_mode, esp_ble_get_bond_device_num());
        emit(telephony::Kind::Auth, encrypted ? 1 : 0);
        if (!encrypted) esp_ble_gap_disconnect(peer);
        break;
    case ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT:
        ESP_LOGI(Tag, "BOND remove status=%d remaining=%d", p->remove_bond_dev_cmpl.status, esp_ble_get_bond_device_num()); break;
    default: break;
    }
}

void gatts(esp_gatts_cb_event_t event, esp_gatt_if_t iface, esp_ble_gatts_cb_param_t* p) {
    // Map indices captured from the public attr-table API to assigned handles.
    if (event == ESP_GATTS_CREAT_ATTR_TAB_EVT && p->add_attr_tab.svc_uuid.len == ESP_UUID_LEN_16 &&
        p->add_attr_tab.svc_uuid.uuid.uuid16 == 0x1812 && p->add_attr_tab.status == ESP_GATT_OK) {
        hidIf = iface;
        auto handle = [&](int index) -> uint16_t { return index >= 0 && index < p->add_attr_tab.num_handle ? p->add_attr_tab.handles[index] : 0; };
        inputHandle = handle(inputIndex); cccHandle = handle(cccIndex); outputHandle = handle(outputIndex);
        controlHandle = handle(controlIndex); protocolHandle = handle(protocolIndex);
        ESP_LOGI(Tag, "HANDLES input=0x%04x ccc=0x%04x output=0x%04x", inputHandle, cccHandle, outputHandle);
        const uint8_t zero = 0;
        if (inputHandle) esp_ble_gatts_set_attr_value(inputHandle, 1, &zero);
        if (outputHandle) esp_ble_gatts_set_attr_value(outputHandle, 1, &zero);
    }
    if (iface == hidIf) {
        if (event == ESP_GATTS_START_EVT) { serviceReady = p->start.status == ESP_GATT_OK; advertise(); }
        if (event == ESP_GATTS_CONNECT_EVT) {
            ++generation;
            connectionId = p->connect.conn_id;
            std::memcpy(peer, p->connect.remote_bda, 6);
            connected = true; encrypted = false; subscribed = false;
            ESP_LOGI(Tag, "CONNECT generation=%lu", (unsigned long)generation.load());
            emit(telephony::Kind::Connected);
        }
        if (event == ESP_GATTS_DISCONNECT_EVT) {
            connected = false; encrypted = false; subscribed = false; numericPending = false;
            ESP_LOGI(Tag, "DISCONNECT reason=0x%02x", p->disconnect.reason);
            emit(telephony::Kind::Disconnected, p->disconnect.reason);
            advertise();
        }
        if (event == ESP_GATTS_READ_EVT) ESP_LOGI(Tag, "READ handle=0x%04x", p->read.handle);
        if (event == ESP_GATTS_WRITE_EVT) {
            auto& w = p->write;
            ESP_LOGI(Tag, "WRITE handle=0x%04x len=%u encrypted=%d prep=%d offset=%u", w.handle, w.len, (int)encrypted.load(), w.is_prep, w.offset);
            if (w.is_prep || w.offset || !w.len) {
                ESP_LOGW(Tag, "Ignoring malformed/long write");
                return; // Prevent the SDK's unchecked value[0] access.
            }
            if (w.handle == cccHandle && w.len == 2 && encrypted) {
                subscribed = w.value[0] == 1 && w.value[1] == 0;
                ESP_LOGI(Tag, "SUBSCRIBE input=%d", (int)subscribed.load());
                emit(telephony::Kind::Subscribe, subscribed ? 1 : 0);
            } else if (w.handle == outputHandle) {
                ESP_LOGI(Tag, "OUTPUT id=1 len=%u value=0x%02x", w.len, w.value[0]);
                if (encrypted && w.len == 1) emit(telephony::Kind::Output, w.value[0], w.len, telephony::ReportId);
            } else if (w.handle == controlHandle && w.len == 1) emit(telephony::Kind::Control, w.value[0]);
            else if (w.handle == protocolHandle && w.len == 1) emit(telephony::Kind::Protocol, w.value[0]);
        }
        if (event == ESP_GATTS_CONF_EVT && p->conf.status != ESP_GATT_OK) emit(telephony::Kind::Error, p->conf.status);
    }
    esp_hidd_gatts_event_handler(event, iface, p);
}
void hidEvent(void*, esp_event_base_t, int32_t id, void*) {
    // Output is consumed synchronously in gatts(), before disconnect/generation
    // can change; the IDF's separate event task is used for lifecycle only.
    if (id == ESP_HIDD_START_EVENT) emit(telephony::Kind::Started);
}
}

extern "C" esp_err_t __real_esp_ble_gatts_create_attr_tab(const esp_gatts_attr_db_t*, esp_gatt_if_t, uint16_t, uint8_t);
extern "C" esp_err_t __wrap_esp_ble_gatts_create_attr_tab(const esp_gatts_attr_db_t* db, esp_gatt_if_t iface, uint16_t count, uint8_t instance) {
    // Inspect UUIDs rather than relying on private IDF database offsets.
    if (count && db[0].att_desc.length == 2 && db[0].att_desc.value[0] == 0x12 && db[0].att_desc.value[1] == 0x18) {
        int lastReport = -1, lastCcc = -1;
        for (int i = 0; i < count; ++i) {
            const auto& a = db[i].att_desc;
            if (a.uuid_length != 2) continue;
            const unsigned uuid = a.uuid_p[0] | (a.uuid_p[1] << 8);
            if (uuid == 0x2a4d) { lastReport = i; lastCcc = -1; }
            if (uuid == 0x2902) lastCcc = i;
            if (uuid == 0x2a4c) controlIndex = i;
            if (uuid == 0x2a4e) protocolIndex = i;
            if (uuid == 0x2908 && a.length == 2 && a.value[0] == telephony::ReportId) {
                if (a.value[1] == 1) { inputIndex = lastReport; cccIndex = lastCcc; }
                if (a.value[1] == 2) outputIndex = lastReport;
            }
        }
    }
    return __real_esp_ble_gatts_create_attr_tab(db, iface, count, instance);
}

namespace telephony {
esp_err_t begin() {
    events = xQueueCreate(32, sizeof(Event));
    if (!events) return ESP_ERR_NO_MEM;
    esp_bt_controller_config_t controller = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err;
#define CHECK(call) do { err = (call); if (err != ESP_OK) { ESP_LOGE(Tag, "%s: %s", #call, esp_err_to_name(err)); return err; } } while (0)
    CHECK(esp_bt_controller_init(&controller));
    CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    CHECK(esp_bluedroid_init());
    CHECK(esp_bluedroid_enable());
    CHECK(esp_ble_gap_register_callback(gap));
    CHECK(esp_ble_gatts_register_callback(gatts));
    uint8_t auth = ESP_LE_AUTH_REQ_SC_MITM_BOND, io = ESP_IO_CAP_IO, keySize = 16;
    uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, 1));
    CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io, 1));
    CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &keySize, 1));
    CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, 1));
    CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, 1));
    CHECK(esp_ble_gap_set_device_name("M5StopWatch MuteHid"));
    advertising.adv_int_min = 0x30; advertising.adv_int_max = 0x60;
    advertising.adv_type = ADV_TYPE_IND;
    advertising.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    advertising.channel_map = ADV_CHNL_ALL;
    advertising.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    esp_ble_adv_data_t adv{};
    adv.appearance = ESP_HID_APPEARANCE_GENERIC;
    adv.flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
    adv.service_uuid_len = sizeof(serviceUuid); adv.p_service_uuid = serviceUuid;
    CHECK(esp_ble_gap_config_adv_data(&adv));
    esp_ble_adv_data_t scan{};
    scan.set_scan_rsp = true; scan.include_name = true;
    CHECK(esp_ble_gap_config_adv_data(&scan));
    static esp_hid_raw_report_map_t maps[] = {{ReportMap, sizeof(ReportMap)}};
    static char serial[20];
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
    std::snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_hid_device_config_t cfg{};
    cfg.vendor_id = VendorId; cfg.product_id = ProductId; cfg.version = Version;
    cfg.device_name = "M5StopWatch MuteHid";
    cfg.manufacturer_name = "MuteHid Phase0 (unassigned IDs)";
    cfg.serial_number = serial;
    cfg.report_maps = maps; cfg.report_maps_len = 1;
    auto* parsed = esp_hid_parse_report_map(ReportMap, sizeof(ReportMap));
    if (!parsed) return ESP_FAIL;
    bool input = false, output = false;
    for (unsigned i = 0; i < parsed->reports_len; ++i) {
        const auto& r = parsed->reports[i];
        ESP_LOGI(Tag, "MAP id=%u type=%u len=%u protocol=%u", r.report_id, r.report_type, r.value_len, r.protocol_mode);
        if (r.report_id == ReportId && r.value_len == 1 && r.protocol_mode == ESP_HID_PROTOCOL_MODE_REPORT) {
            input |= r.report_type == ESP_HID_REPORT_TYPE_INPUT;
            output |= r.report_type == ESP_HID_REPORT_TYPE_OUTPUT;
        }
    }
    esp_hid_free_report_map(parsed);
    if (!input || !output) return ESP_ERR_INVALID_SIZE;
    ESP_LOGI(Tag, "IDENTITY experimental VID=%04x PID=%04x version=%04x bonds=%d", VendorId, ProductId, Version, esp_ble_get_bond_device_num());
    CHECK(esp_hidd_dev_init(&cfg, ESP_HID_TRANSPORT_BLE, hidEvent, &device));
#undef CHECK
    return ESP_OK;
}
bool poll(Event& e) { return events && xQueueReceive(events, &e, 0) == pdTRUE; }
bool overflowed() { return lost.exchange(false); }
esp_err_t send(uint8_t value) {
    if (!connected || !encrypted || !subscribed || !inputHandle || stopping) return ESP_ERR_INVALID_STATE;
    // The IDF 5.5 esp_hidd_dev_input_set waits forever on an internal semaphore.
    // Use its generated characteristic with the nonblocking public GATTS API,
    // keeping the UI/recovery gesture alive even when a host stops responding.
    esp_ble_gatts_set_attr_value(inputHandle, 1, &value);
    const auto err = esp_ble_gatts_send_indicate(hidIf, connectionId, inputHandle, 1, &value, false);
    ESP_LOGI(Tag, "INPUT id=1 len=1 value=%u result=%s", value, esp_err_to_name(err));
    return err;
}
void confirm(bool accept) { if (numericPending.exchange(false)) esp_ble_confirm_reply(confirmationPeer, accept); }
void forget() {
    if (connected) { ESP_LOGW(Tag, "Disconnect in Windows before forgetting bonds"); return; }
    int count = esp_ble_get_bond_device_num();
    std::vector<esp_ble_bond_dev_t> bonds(count);
    if (count && esp_ble_get_bond_device_list(&count, bonds.data()) == ESP_OK)
        for (const auto& b : bonds) { esp_bd_addr_t address; std::memcpy(address, b.bd_addr, 6); esp_ble_remove_bond_device(address); }
}
void stop() { stopping = true; esp_ble_gap_stop_advertising(); if (connected) esp_ble_gap_disconnect(peer); }
void battery(uint8_t level) { if (device) esp_hidd_dev_battery_set(device, level); }
}
