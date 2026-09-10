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
#include "nvs.h"

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

// Device identity. The shared PnP IDs require these strings to carry a contact
// the owner controls and a model name unique within it.
char manufacturerName[] = "wararyo(contact@wararyo.com)";
char modelNumber[] = "M5StopWatch MuteHid";
// The IDF's Device Information Service has no Model Number String; hosts that
// name a HID device from it would otherwise see no product name at all.
uint16_t characterDeclarationUuid = ESP_GATT_UUID_CHAR_DECLARE;
uint16_t modelNumberUuid = ESP_GATT_UUID_MODEL_NUMBER_STR;
uint8_t characteristicReadable = ESP_GATT_CHAR_PROP_BIT_READ;

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
bool bondedPeer(const uint8_t* address) {
    int count = esp_ble_get_bond_device_num();
    if (count <= 0) return false;
    std::vector<esp_ble_bond_dev_t> bonds(count);
    if (esp_ble_get_bond_device_list(&count, bonds.data()) != ESP_OK) return false;
    for (const auto& b : bonds) if (!std::memcmp(b.bd_addr, address, 6)) return true;
    return false;
}
bool allowedPeer(const uint8_t* address) {
    return esp_ble_get_bond_device_num() <= 0 || bondedPeer(address);
}

// A bonded host writes the Input CCCD once and then expects the server to keep
// it across reboots, so the subscription has to live in NVS next to the bond.
constexpr const char* Namespace = "mutehid";
constexpr const char* SubscriptionKey = "input_ccc";
struct Subscription { esp_bd_addr_t address; uint16_t value; };

bool loadSubscription(Subscription& record) {
    nvs_handle_t nvs;
    if (nvs_open(Namespace, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t size = sizeof(record);
    const auto err = nvs_get_blob(nvs, SubscriptionKey, &record, &size);
    nvs_close(nvs);
    return err == ESP_OK && size == sizeof(record);
}
void saveSubscription(const uint8_t* address, uint16_t value) {
    Subscription record{};
    std::memcpy(record.address, address, 6);
    record.value = value;
    nvs_handle_t nvs;
    auto err = nvs_open(Namespace, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, SubscriptionKey, &record, sizeof(record));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(Tag, "CCC store value=0x%04x result=%s", value, esp_err_to_name(err));
}
void clearSubscription() {
    nvs_handle_t nvs;
    if (nvs_open(Namespace, NVS_READWRITE, &nvs) != ESP_OK) return;
    const auto err = nvs_erase_key(nvs, SubscriptionKey);
    if (err == ESP_OK) nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(Tag, "CCC cleared result=%s", esp_err_to_name(err));
}
void restoreSubscription(const uint8_t* address) {
    Subscription record{};
    if (!loadSubscription(record)) { ESP_LOGI(Tag, "CCC none stored"); return; }
    // Accept the stored peer itself, or the record of the still-bonded host when
    // the connection reports a different (private) address for it.
    if (std::memcmp(record.address, address, 6) && !bondedPeer(record.address)) {
        ESP_LOGW(Tag, "CCC stored for another peer; not restored");
        return;
    }
    if (!(record.value & 0x0001)) { ESP_LOGI(Tag, "CCC stored as disabled"); return; }
    subscribed = true;
    if (cccHandle) {
        const uint8_t value[2] = {0x01, 0x00};
        const auto err = esp_ble_gatts_set_attr_value(cccHandle, 2, value);
        if (err != ESP_OK) ESP_LOGW(Tag, "CCC attribute update: %s", esp_err_to_name(err));
    }
    ESP_LOGI(Tag, "SUBSCRIBE restored=1 from bond");
    emit(telephony::Kind::Subscribe, 1);
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
        if (encrypted) restoreSubscription(p->ble_security.auth_cmpl.bd_addr);
        else esp_ble_gap_disconnect(peer);
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
            if (w.handle == cccHandle && w.len == 2) {
                // The descriptor already requires an encrypted link, so gating on
                // this task's view of encryption would only drop a host write that
                // arrives before the authentication event is dispatched.
                subscribed = w.value[0] == 1 && w.value[1] == 0;
                ESP_LOGI(Tag, "SUBSCRIBE input=%d", (int)subscribed.load());
                saveSubscription(peer, static_cast<uint16_t>(w.value[0] | (w.value[1] << 8)));
                emit(telephony::Kind::Subscribe, subscribed ? 1 : 0);
            } else if (w.handle == outputHandle) {
                // Report ID is metadata on GATT, but macOS prefixes it to the
                // payload anyway; Windows writes the payload alone. Accept both
                // shapes and pass only the payload byte on.
                const bool prefixed = w.len == 2 && w.value[0] == telephony::ReportId;
                const uint8_t value = prefixed ? w.value[1] : w.value[0];
                ESP_LOGI(Tag, "OUTPUT id=1 len=%u prefixed=%d value=0x%02x", w.len, (int)prefixed, value);
                if (encrypted && (w.len == 1 || prefixed)) emit(telephony::Kind::Output, value, 1, telephony::ReportId);
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

namespace {
bool declares(const esp_gatts_attr_db_t* db, uint16_t count, uint16_t service) {
    return count && db[0].att_desc.length == 2 && db[0].att_desc.value[0] == (service & 0xff) &&
           db[0].att_desc.value[1] == (service >> 8);
}
void describe(esp_gatts_attr_db_t& record, uint16_t& uuid, uint16_t length, uint8_t* value) {
    record.attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
    record.att_desc.uuid_length = ESP_UUID_LEN_16;
    record.att_desc.uuid_p = reinterpret_cast<uint8_t*>(&uuid);
    record.att_desc.perm = ESP_GATT_PERM_READ;
    record.att_desc.max_length = length;
    record.att_desc.length = length;
    record.att_desc.value = value;
}
}

extern "C" esp_err_t __real_esp_ble_gatts_create_attr_tab(const esp_gatts_attr_db_t*, esp_gatt_if_t, uint16_t, uint8_t);
extern "C" esp_err_t __wrap_esp_ble_gatts_create_attr_tab(const esp_gatts_attr_db_t* db, esp_gatt_if_t iface, uint16_t count, uint8_t instance) {
    // Append the Model Number String the IDF leaves out. The table is copied
    // into a static buffer because the stack keeps reading it after this call.
    if (declares(db, count, 0x180a)) {
        static esp_gatts_attr_db_t extended[12];
        constexpr uint16_t extra = 2;
        if (count + extra <= sizeof(extended) / sizeof(extended[0])) {
            std::memcpy(extended, db, count * sizeof(esp_gatts_attr_db_t));
            describe(extended[count], characterDeclarationUuid, 1, &characteristicReadable);
            describe(extended[count + 1], modelNumberUuid,
                     static_cast<uint16_t>(std::strlen(modelNumber)), reinterpret_cast<uint8_t*>(modelNumber));
            ESP_LOGI(Tag, "DIS model=\"%s\" appended", modelNumber);
            return __real_esp_ble_gatts_create_attr_tab(extended, iface, count + extra, instance);
        }
        ESP_LOGW(Tag, "Device information table too large; model number omitted");
    }
    // Inspect UUIDs rather than relying on private IDF database offsets.
    if (declares(db, count, 0x1812)) {
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
    cfg.manufacturer_name = manufacturerName;
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
    ESP_LOGI(Tag, "IDENTITY VID=%04x PID=%04x version=%04x model=\"%s\" bonds=%d", VendorId, ProductId,
             Version, modelNumber, esp_ble_get_bond_device_num());
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
    clearSubscription();
    subscribed = false;
}
void stop() { stopping = true; esp_ble_gap_stop_advertising(); if (connected) esp_ble_gap_disconnect(peer); }
// Drops the link but keeps advertising, so a new host can pair afterwards.
void disconnectPeer() { if (connected) esp_ble_gap_disconnect(peer); }
int bonds() { return esp_ble_get_bond_device_num(); }
bool peerText(char* out, unsigned size) {
    esp_bd_addr_t address{};
    if (connected) {
        std::memcpy(address, peer, 6);
    } else {
        int count = esp_ble_get_bond_device_num();
        if (count <= 0) return false;
        std::vector<esp_ble_bond_dev_t> list(count);
        if (esp_ble_get_bond_device_list(&count, list.data()) != ESP_OK || count <= 0) return false;
        std::memcpy(address, list[0].bd_addr, 6);
    }
    std::snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X", address[0], address[1], address[2],
                  address[3], address[4], address[5]);
    return true;
}
void battery(uint8_t level) { if (device) esp_hidd_dev_battery_set(device, level); }
}
