#include "ancs_client.h"

#include <esp_app_desc.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_random.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <host/ble_store.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <os/os_mbuf.h>
#include <services/dis/ble_svc_dis.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstring>
#include <utility>

extern "C" void ble_store_config_init(void);

namespace {

constexpr char kTag[] = "AncsClient";
constexpr char kDeviceName[] = "Xiaozhi ANCS";
constexpr uint8_t kCommandGetNotificationAttributes = 0;
constexpr uint8_t kAttributeAppIdentifier = 0;
constexpr uint8_t kAttributeTitle = 1;
constexpr uint8_t kAttributeSubtitle = 2;
constexpr uint8_t kAttributeMessage = 3;
constexpr uint16_t kTitleMaxLength = 256;
constexpr uint16_t kSubtitleMaxLength = 256;
constexpr uint16_t kMessageMaxLength = 1024;
constexpr size_t kNotificationRecordSize = 8;
constexpr size_t kMaxPendingRequests = 16;
constexpr size_t kMaxCacheEntries = 32;
constexpr uint32_t kDataTimeoutMs = 5000;
constexpr uint32_t kDiscoveryRetryMs = 5000;

const ble_uuid16_t kGattServiceUuid = BLE_UUID16_INIT(0x1801);
const ble_uuid16_t kServiceChangedUuid = BLE_UUID16_INIT(0x2a05);
const ble_uuid16_t kClientConfigurationUuid = BLE_UUID16_INIT(BLE_GATT_DSC_CLT_CFG_UUID16);

const ble_uuid128_t kAncsServiceUuid = BLE_UUID128_INIT(
    0xd0, 0x00, 0x2d, 0x12, 0x1e, 0x4b, 0x0f, 0xa4, 0x99, 0x4e, 0xce, 0xb5, 0x31, 0xf4, 0x05, 0x79);
const ble_uuid128_t kNotificationSourceUuid = BLE_UUID128_INIT(
    0xbd, 0x1d, 0xa2, 0x99, 0xe6, 0x25, 0x58, 0x8c, 0xd9, 0x42, 0x01, 0x63, 0x0d, 0x12, 0xbf, 0x9f);
const ble_uuid128_t kControlPointUuid = BLE_UUID128_INIT(
    0xd9, 0xd9, 0xaa, 0xfd, 0xbd, 0x9b, 0x21, 0x98, 0xa8, 0x49, 0xe1, 0x45, 0xf3, 0xd8, 0xd1, 0x69);
const ble_uuid128_t kDataSourceUuid = BLE_UUID128_INIT(
    0xfb, 0x7b, 0x7c, 0xce, 0x6a, 0xb3, 0x44, 0xbe, 0xb5, 0x4b, 0xd6, 0x24, 0xe9, 0xc6, 0xea, 0x22);

std::string NewSessionId() {
    std::array<uint8_t, 8> bytes = {};
    esp_fill_random(bytes.data(), bytes.size());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string id(bytes.size() * 2, '0');
    for (size_t index = 0; index < bytes.size(); ++index) {
        id[index * 2] = kHex[bytes[index] >> 4];
        id[index * 2 + 1] = kHex[bytes[index] & 0x0f];
    }
    return id;
}

bool CopyMbuf(struct os_mbuf* mbuf, std::vector<uint8_t>& output) {
    const uint16_t size = OS_MBUF_PKTLEN(mbuf);
    output.resize(size);
    return size == 0 || os_mbuf_copydata(mbuf, 0, size, output.data()) == 0;
}

}  // namespace

AncsClient* AncsClient::instance_ = nullptr;

AncsClient::AncsClient(NotificationCallback callback) : on_notification_(std::move(callback)) {}

AncsClient::~AncsClient() { Stop(); }

bool AncsClient::Start() {
    if (started_) {
        return true;
    }
    if (instance_ != nullptr) {
        ESP_LOGE(kTag, "Only one ANCS client can run");
        return false;
    }
    const esp_err_t result = nimble_port_init();
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "nimble_port_init failed: %s", esp_err_to_name(result));
        return false;
    }

    instance_ = this;
    ble_hs_cfg.reset_cb = OnReset;
    ble_hs_cfg.sync_cb = OnSync;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_store_config_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_dis_init();
    ble_svc_gap_device_name_set(kDeviceName);
    ble_svc_dis_manufacturer_name_set("XiaoZhi");
    ble_svc_dis_firmware_revision_set(esp_app_get_description()->version);

    if (ble_npl_callout_init(&data_timeout_, nimble_port_get_dflt_eventq(), DataTimeout, this) !=
            0 ||
        ble_npl_callout_init(&discovery_timeout_, nimble_port_get_dflt_eventq(), DiscoveryTimeout,
                             this) != 0) {
        ESP_LOGE(kTag, "Unable to initialize NimBLE timers");
        nimble_port_deinit();
        instance_ = nullptr;
        return false;
    }

    started_ = true;
    nimble_port_freertos_init(HostTask);
    ESP_LOGI(kTag, "ANCS client initialized with persistent single-phone bonding");
    return true;
}

void AncsClient::Stop() {
    if (!started_) {
        return;
    }
    started_ = false;
    ble_npl_callout_stop(&data_timeout_);
    ble_npl_callout_stop(&discovery_timeout_);
    ble_gap_adv_stop();
    if (connected_) {
        ble_gap_terminate(connection_handle_, BLE_ERR_REM_USER_CONN_TERM);
    }
    ResetConnection();
    if (nimble_port_stop() == 0) {
        nimble_port_deinit();
    }
    instance_ = nullptr;
}

void AncsClient::StartAdvertising() {
    if (!started_ || connected_) {
        return;
    }
    ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.sol_uuids128 = &kAncsServiceUuid;
    fields.sol_num_uuids128 = 1;
    int result = ble_gap_adv_set_fields(&fields);
    if (result != 0) {
        ESP_LOGE(kTag, "Unable to set ANCS solicitation: %d", result);
        return;
    }

    ble_hs_adv_fields scan_response = {};
    scan_response.name = reinterpret_cast<const uint8_t*>(kDeviceName);
    scan_response.name_len = sizeof(kDeviceName) - 1;
    scan_response.name_is_complete = 1;
    result = ble_gap_adv_rsp_set_fields(&scan_response);
    if (result != 0) {
        ESP_LOGE(kTag, "Unable to set scan response: %d", result);
        return;
    }

    ble_gap_adv_params parameters = {};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    result =
        ble_gap_adv_start(own_address_type_, nullptr, BLE_HS_FOREVER, &parameters, GapEvent, this);
    if (result == 0) {
        ESP_LOGI(kTag, "Advertising as %s with ANCS service solicitation", kDeviceName);
    } else {
        ESP_LOGE(kTag, "Unable to start advertising: %d", result);
    }
}

void AncsClient::BeginSecurity(uint16_t connection_handle) {
    const int result = ble_gap_security_initiate(connection_handle);
    if (result != 0) {
        ESP_LOGE(kTag, "Unable to initiate encryption: %d", result);
        ble_gap_terminate(connection_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

bool AncsClient::VerifyBondedPeer(uint16_t connection_handle) const {
    ble_addr_t peers[1] = {};
    int peer_count = 0;
    if (ble_store_util_bonded_peers(peers, &peer_count, 1) != 0 || peer_count == 0) {
        return true;
    }
    ble_gap_conn_desc descriptor = {};
    if (ble_gap_conn_find(connection_handle, &descriptor) != 0) {
        return false;
    }
    return ble_addr_cmp(&peers[0], &descriptor.peer_id_addr) == 0;
}

void AncsClient::DiscoverServiceChanged() {
    if (!connected_ || !encrypted_) {
        return;
    }
    discovery_target_ = DiscoveryTarget::kGattService;
    service_start_handle_ = 0;
    service_end_handle_ = 0;
    service_changed_handle_ = 0;
    const int result = ble_gattc_disc_svc_by_uuid(connection_handle_, &kGattServiceUuid.u,
                                                  ServiceDiscovered, this);
    if (result != 0) {
        ESP_LOGW(kTag, "GATT service discovery deferred: %d", result);
        ScheduleAncsDiscovery(250);
    }
}

void AncsClient::DiscoverAncs() {
    if (!connected_ || !encrypted_ || ancs_ready_) {
        return;
    }
    ble_npl_callout_stop(&discovery_timeout_);
    discovery_target_ = DiscoveryTarget::kAncsService;
    service_start_handle_ = 0;
    service_end_handle_ = 0;
    notification_source_handle_ = 0;
    data_source_handle_ = 0;
    control_point_handle_ = 0;
    characteristics_.clear();
    const int result = ble_gattc_disc_svc_by_uuid(connection_handle_, &kAncsServiceUuid.u,
                                                  ServiceDiscovered, this);
    if (result != 0) {
        ESP_LOGW(kTag, "ANCS service discovery deferred: %d", result);
        ScheduleAncsDiscovery(kDiscoveryRetryMs);
    }
}

void AncsClient::DiscoverCharacteristics() {
    const int result = ble_gattc_disc_all_chrs(connection_handle_, service_start_handle_,
                                               service_end_handle_, CharacteristicDiscovered, this);
    if (result != 0) {
        ESP_LOGW(kTag, "Characteristic discovery failed to start: %d", result);
        if (discovery_target_ == DiscoveryTarget::kAncsService) {
            ScheduleAncsDiscovery(kDiscoveryRetryMs);
        } else {
            ScheduleAncsDiscovery(250);
        }
    }
}

void AncsClient::DiscoverDescriptors(DescriptorTarget target, uint16_t value_handle,
                                     uint16_t end_handle) {
    descriptor_target_ = target;
    descriptor_cccd_handle_ = 0;
    if (value_handle == 0 || value_handle >= end_handle) {
        FinishSubscription(target, false);
        return;
    }
    const int result = ble_gattc_disc_all_dscs(connection_handle_, value_handle, end_handle,
                                               DescriptorDiscovered, this);
    if (result != 0) {
        ESP_LOGW(kTag, "Descriptor discovery failed to start: %d", result);
        FinishSubscription(target, false);
    }
}

void AncsClient::Subscribe(DescriptorTarget target, uint16_t cccd_handle) {
    subscription_target_ = target;
    const uint16_t value = target == DescriptorTarget::kServiceChanged ? 0x0002 : 0x0001;
    const int result = ble_gattc_write_flat(connection_handle_, cccd_handle, &value, sizeof(value),
                                            SubscriptionComplete, this);
    if (result != 0) {
        ESP_LOGW(kTag, "CCCD subscription failed to start: %d", result);
        FinishSubscription(target, false);
    }
}

void AncsClient::FinishSubscription(DescriptorTarget target, bool success) {
    if (target == DescriptorTarget::kServiceChanged) {
        ESP_LOGI(kTag, "Service Changed subscription %s", success ? "ready" : "unavailable");
        DiscoverAncs();
        return;
    }
    if (target == DescriptorTarget::kDataSource) {
        if (!success) {
            ScheduleAncsDiscovery(kDiscoveryRetryMs);
            return;
        }
        auto found = std::find_if(
            characteristics_.begin(), characteristics_.end(),
            [](const RemoteCharacteristic& characteristic) { return characteristic.kind == 2; });
        if (found == characteristics_.end()) {
            ScheduleAncsDiscovery(kDiscoveryRetryMs);
            return;
        }
        DiscoverDescriptors(DescriptorTarget::kNotificationSource, found->value_handle,
                            found->descriptor_end_handle);
        return;
    }
    if (target == DescriptorTarget::kNotificationSource) {
        if (success) {
            ancs_ready_ = true;
            ble_npl_callout_stop(&discovery_timeout_);
            ESP_LOGI(kTag, "ANCS ready; Data Source subscribed before Notification Source");
        } else {
            ScheduleAncsDiscovery(kDiscoveryRetryMs);
        }
    }
}

void AncsClient::ScheduleAncsDiscovery(uint32_t delay_ms) {
    if (connected_ && encrypted_ && !ancs_ready_) {
        ble_npl_callout_reset(&discovery_timeout_, ble_npl_time_ms_to_ticks32(delay_ms));
    }
}

void AncsClient::InvalidateAncs() {
    ancs_ready_ = false;
    notification_source_handle_ = 0;
    data_source_handle_ = 0;
    control_point_handle_ = 0;
    characteristics_.clear();
    notification_source_buffer_.clear();
    pending_requests_.clear();
    request_active_ = false;
    attribute_parser_.Reset();
    notification_cache_.clear();
    pre_existing_filter_.Clear();
    ble_npl_callout_stop(&data_timeout_);
}

void AncsClient::HandleNotificationSource(struct os_mbuf* mbuf) {
    std::vector<uint8_t> data;
    if (!CopyMbuf(mbuf, data)) {
        ESP_LOGW(kTag, "Unable to flatten Notification Source mbuf");
        return;
    }
    notification_source_buffer_.insert(notification_source_buffer_.end(), data.begin(), data.end());
    while (notification_source_buffer_.size() >= kNotificationRecordSize) {
        HandleNotificationRecord(notification_source_buffer_.data());
        notification_source_buffer_.erase(
            notification_source_buffer_.begin(),
            notification_source_buffer_.begin() + kNotificationRecordSize);
    }
}

void AncsClient::HandleDataSource(struct os_mbuf* mbuf) {
    if (!request_active_) {
        return;
    }
    std::vector<uint8_t> data;
    if (!CopyMbuf(mbuf, data)) {
        FinishRequest(false);
        return;
    }
    const auto status = attribute_parser_.Append(data.data(), data.size());
    if (status == AncsAttributeParser::Status::kNeedMore) {
        ble_npl_callout_reset(&data_timeout_, ble_npl_time_ms_to_ticks32(kDataTimeoutMs));
        return;
    }
    if (status != AncsAttributeParser::Status::kComplete) {
        ESP_LOGW(kTag, "Invalid ANCS attribute response");
        FinishRequest(false);
        return;
    }
    if (attribute_parser_.result().uid != active_request_.notification.uid) {
        ESP_LOGW(kTag, "Ignoring late ANCS attribute response uid=%" PRIu32,
                 attribute_parser_.result().uid);
        attribute_parser_.Reset();
        ble_npl_callout_reset(&data_timeout_, ble_npl_time_ms_to_ticks32(kDataTimeoutMs));
        return;
    }

    const auto& result = attribute_parser_.result();
    active_request_.notification.app_identifier = result.app_identifier;
    active_request_.notification.title = result.title;
    active_request_.notification.subtitle = result.subtitle;
    active_request_.notification.message = result.message;
    active_request_.notification.truncated = result.truncated;
    FinishRequest(true);
}

void AncsClient::HandleNotificationRecord(const uint8_t* record) {
    AncsNotification notification;
    if (!ParseAncsNotificationRecord(record, kNotificationRecordSize, notification)) {
        ESP_LOGW(kTag, "Unknown ANCS event id %u", record[0]);
        return;
    }
    notification.session_id = session_id_;

    if (pre_existing_filter_.ShouldIgnore(notification)) {
        if (IsAncsPreExisting(notification.event_flags)) {
            ESP_LOGI(kTag, "Ignoring pre-existing ANCS notification uid=%" PRIu32,
                     notification.uid);
        }
        return;
    }
    if (notification.event == AncsEventType::kRemoved) {
        HandleRemoved(std::move(notification));
    } else {
        QueueAttributeRequest(std::move(notification));
    }
}

void AncsClient::QueueAttributeRequest(AncsNotification notification) {
    if (pending_requests_.size() >= kMaxPendingRequests) {
        ESP_LOGW(kTag, "ANCS attribute queue full; dropping oldest request");
        pending_requests_.pop_front();
    }
    pending_requests_.push_back({.notification = std::move(notification)});
    StartNextRequest();
}

void AncsClient::StartNextRequest() {
    if (request_active_ || pending_requests_.empty() || !ancs_ready_ ||
        control_point_handle_ == 0) {
        return;
    }
    active_request_ = std::move(pending_requests_.front());
    pending_requests_.pop_front();
    request_active_ = true;
    attribute_parser_.Reset();

    const uint32_t uid = active_request_.notification.uid;
    const std::array<uint8_t, 15> command = {
        kCommandGetNotificationAttributes,
        static_cast<uint8_t>(uid),
        static_cast<uint8_t>(uid >> 8),
        static_cast<uint8_t>(uid >> 16),
        static_cast<uint8_t>(uid >> 24),
        kAttributeAppIdentifier,
        kAttributeTitle,
        static_cast<uint8_t>(kTitleMaxLength),
        static_cast<uint8_t>(kTitleMaxLength >> 8),
        kAttributeSubtitle,
        static_cast<uint8_t>(kSubtitleMaxLength),
        static_cast<uint8_t>(kSubtitleMaxLength >> 8),
        kAttributeMessage,
        static_cast<uint8_t>(kMessageMaxLength),
        static_cast<uint8_t>(kMessageMaxLength >> 8),
    };
    const int result =
        ble_gattc_write_flat(connection_handle_, control_point_handle_, command.data(),
                             command.size(), ControlPointWriteComplete, this);
    if (result != 0) {
        ESP_LOGW(kTag, "Attribute request failed to start: %d", result);
        FinishRequest(false);
        return;
    }
    ble_npl_callout_reset(&data_timeout_, ble_npl_time_ms_to_ticks32(kDataTimeoutMs));
}

void AncsClient::FinishRequest(bool attributes_complete) {
    if (!request_active_) {
        return;
    }
    ble_npl_callout_stop(&data_timeout_);
    AncsNotification notification = std::move(active_request_.notification);
    request_active_ = false;
    attribute_parser_.Reset();
    if (!attributes_complete) {
        notification.truncated = true;
    }
    if (notification.event != AncsEventType::kRemoved) {
        StoreInCache(notification);
    }
    Publish(std::move(notification));
    StartNextRequest();
}

void AncsClient::HandleRemoved(AncsNotification notification) {
    pending_requests_.erase(std::remove_if(pending_requests_.begin(), pending_requests_.end(),
                                           [&](const PendingRequest& request) {
                                               return request.notification.uid == notification.uid;
                                           }),
                            pending_requests_.end());
    if (request_active_ && active_request_.notification.uid == notification.uid) {
        active_request_.notification.event = AncsEventType::kRemoved;
        active_request_.notification.event_flags = notification.event_flags;
        active_request_.notification.category_id = notification.category_id;
        auto cached = notification_cache_.find(notification.uid);
        if (cached != notification_cache_.end()) {
            active_request_.notification.app_identifier = cached->second.app_identifier;
            active_request_.notification.title = cached->second.title;
            active_request_.notification.subtitle = cached->second.subtitle;
            active_request_.notification.message = cached->second.message;
            active_request_.notification.truncated = cached->second.truncated;
            notification_cache_.erase(cached);
        }
        return;
    }
    auto cached = notification_cache_.find(notification.uid);
    if (cached != notification_cache_.end()) {
        AncsNotification completed = cached->second;
        completed.event = AncsEventType::kRemoved;
        completed.event_flags = notification.event_flags;
        completed.category_id = notification.category_id;
        notification_cache_.erase(cached);
        Publish(std::move(completed));
        return;
    }
    Publish(std::move(notification));
}

void AncsClient::Publish(AncsNotification notification) {
    notification.session_id = session_id_;
    notification.sequence = ++sequence_;
    if (on_notification_) {
        on_notification_(std::move(notification));
    }
}

void AncsClient::StoreInCache(const AncsNotification& notification) {
    if (notification_cache_.size() >= kMaxCacheEntries &&
        notification_cache_.find(notification.uid) == notification_cache_.end()) {
        notification_cache_.erase(notification_cache_.begin());
    }
    notification_cache_[notification.uid] = notification;
}

void AncsClient::ResetConnection() {
    connected_ = false;
    encrypted_ = false;
    connection_handle_ = BLE_HS_CONN_HANDLE_NONE;
    session_id_.clear();
    sequence_ = 0;
    notification_cache_.clear();
    pre_existing_filter_.Clear();
    InvalidateAncs();
    ble_npl_callout_stop(&discovery_timeout_);
    discovery_target_ = DiscoveryTarget::kNone;
    descriptor_target_ = DescriptorTarget::kNone;
    subscription_target_ = DescriptorTarget::kNone;
}

int AncsClient::GapEvent(struct ble_gap_event* event, void* arg) {
    auto* client = static_cast<AncsClient*>(arg);
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                ESP_LOGW(kTag, "Connection failed: %d", event->connect.status);
                client->StartAdvertising();
                return 0;
            }
            client->ResetConnection();
            client->connected_ = true;
            client->connection_handle_ = event->connect.conn_handle;
            client->session_id_ = NewSessionId();
            ESP_LOGI(kTag, "iPhone connected; session=%s", client->session_id_.c_str());
            client->BeginSecurity(event->connect.conn_handle);
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(kTag, "iPhone disconnected: reason=%d", event->disconnect.reason);
            client->ResetConnection();
            client->StartAdvertising();
            return 0;

        case BLE_GAP_EVENT_ENC_CHANGE:
            if (event->enc_change.status != 0 ||
                !client->VerifyBondedPeer(event->enc_change.conn_handle)) {
                ESP_LOGW(kTag, "Encryption rejected: status=%d", event->enc_change.status);
                ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
                return 0;
            }
            client->encrypted_ = true;
            ESP_LOGI(kTag, "Encrypted ANCS connection established");
            ble_gattc_exchange_mtu(event->enc_change.conn_handle, nullptr, nullptr);
            client->DiscoverServiceChanged();
            return 0;

        case BLE_GAP_EVENT_NOTIFY_RX:
            if (event->notify_rx.attr_handle == client->service_changed_handle_) {
                ESP_LOGI(kTag, "iOS GATT database changed; rediscovering ANCS");
                client->InvalidateAncs();
                client->ScheduleAncsDiscovery(250);
            } else if (event->notify_rx.attr_handle == client->notification_source_handle_) {
                client->HandleNotificationSource(event->notify_rx.om);
            } else if (event->notify_rx.attr_handle == client->data_source_handle_) {
                client->HandleDataSource(event->notify_rx.om);
            }
            return 0;

        case BLE_GAP_EVENT_REPEAT_PAIRING:
            ESP_LOGW(kTag, "Repeat pairing refused; erase flash before changing phones");
            return BLE_GAP_REPEAT_PAIRING_IGNORE;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            client->StartAdvertising();
            return 0;

        default:
            return 0;
    }
}

int AncsClient::ServiceDiscovered(uint16_t connection_handle, const ble_gatt_error* error,
                                  const ble_gatt_svc* service, void* arg) {
    auto* client = static_cast<AncsClient*>(arg);
    if (connection_handle != client->connection_handle_) {
        return 0;
    }
    if (error->status == 0 && service != nullptr) {
        client->service_start_handle_ = service->start_handle;
        client->service_end_handle_ = service->end_handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        ESP_LOGW(kTag, "Service discovery failed: %d", error->status);
        client->ScheduleAncsDiscovery(kDiscoveryRetryMs);
        return 0;
    }
    if (client->service_start_handle_ == 0) {
        if (client->discovery_target_ == DiscoveryTarget::kGattService) {
            client->DiscoverAncs();
        } else {
            ESP_LOGI(kTag, "ANCS service not currently available; waiting for iOS");
            client->ScheduleAncsDiscovery(kDiscoveryRetryMs);
        }
        return 0;
    }
    client->characteristics_.clear();
    client->DiscoverCharacteristics();
    return 0;
}

int AncsClient::CharacteristicDiscovered(uint16_t connection_handle, const ble_gatt_error* error,
                                         const ble_gatt_chr* characteristic, void* arg) {
    auto* client = static_cast<AncsClient*>(arg);
    if (connection_handle != client->connection_handle_) {
        return 0;
    }
    if (error->status == 0 && characteristic != nullptr) {
        uint8_t kind = 0;
        if (client->discovery_target_ == DiscoveryTarget::kGattService &&
            ble_uuid_cmp(&characteristic->uuid.u, &kServiceChangedUuid.u) == 0) {
            kind = 4;
            client->service_changed_handle_ = characteristic->val_handle;
        } else if (client->discovery_target_ == DiscoveryTarget::kAncsService) {
            if (ble_uuid_cmp(&characteristic->uuid.u, &kDataSourceUuid.u) == 0) {
                kind = 1;
                client->data_source_handle_ = characteristic->val_handle;
            } else if (ble_uuid_cmp(&characteristic->uuid.u, &kNotificationSourceUuid.u) == 0) {
                kind = 2;
                client->notification_source_handle_ = characteristic->val_handle;
            } else if (ble_uuid_cmp(&characteristic->uuid.u, &kControlPointUuid.u) == 0) {
                kind = 3;
                client->control_point_handle_ = characteristic->val_handle;
            }
        }
        client->characteristics_.push_back({.definition_handle = characteristic->def_handle,
                                            .value_handle = characteristic->val_handle,
                                            .kind = kind});
        return 0;
    }
    if (error->status != BLE_HS_EDONE) {
        ESP_LOGW(kTag, "Characteristic discovery failed: %d", error->status);
        client->ScheduleAncsDiscovery(kDiscoveryRetryMs);
        return 0;
    }

    std::sort(client->characteristics_.begin(), client->characteristics_.end(),
              [](const RemoteCharacteristic& left, const RemoteCharacteristic& right) {
                  return left.definition_handle < right.definition_handle;
              });
    for (size_t index = 0; index < client->characteristics_.size(); ++index) {
        client->characteristics_[index].descriptor_end_handle =
            index + 1 < client->characteristics_.size()
                ? client->characteristics_[index + 1].definition_handle - 1
                : client->service_end_handle_;
    }

    if (client->discovery_target_ == DiscoveryTarget::kGattService) {
        auto found =
            std::find_if(client->characteristics_.begin(), client->characteristics_.end(),
                         [](const RemoteCharacteristic& value) { return value.kind == 4; });
        if (found == client->characteristics_.end()) {
            client->DiscoverAncs();
        } else {
            client->DiscoverDescriptors(DescriptorTarget::kServiceChanged, found->value_handle,
                                        found->descriptor_end_handle);
        }
        return 0;
    }

    const auto data_source =
        std::find_if(client->characteristics_.begin(), client->characteristics_.end(),
                     [](const RemoteCharacteristic& value) { return value.kind == 1; });
    const bool complete = data_source != client->characteristics_.end() &&
                          client->notification_source_handle_ != 0 &&
                          client->control_point_handle_ != 0;
    if (!complete) {
        ESP_LOGW(kTag, "ANCS service is missing required characteristics");
        client->ScheduleAncsDiscovery(kDiscoveryRetryMs);
        return 0;
    }
    client->DiscoverDescriptors(DescriptorTarget::kDataSource, data_source->value_handle,
                                data_source->descriptor_end_handle);
    return 0;
}

int AncsClient::DescriptorDiscovered(uint16_t connection_handle, const ble_gatt_error* error,
                                     uint16_t characteristic_handle, const ble_gatt_dsc* descriptor,
                                     void* arg) {
    auto* client = static_cast<AncsClient*>(arg);
    if (connection_handle != client->connection_handle_) {
        return 0;
    }
    if (error->status == 0 && descriptor != nullptr) {
        if (ble_uuid_cmp(&descriptor->uuid.u, &kClientConfigurationUuid.u) == 0) {
            client->descriptor_cccd_handle_ = descriptor->handle;
        }
        return 0;
    }
    const DescriptorTarget target = client->descriptor_target_;
    if (error->status != BLE_HS_EDONE || client->descriptor_cccd_handle_ == 0) {
        ESP_LOGW(kTag, "CCCD discovery failed: %d", error->status);
        client->FinishSubscription(target, false);
        return 0;
    }
    client->Subscribe(target, client->descriptor_cccd_handle_);
    return 0;
}

int AncsClient::SubscriptionComplete(uint16_t connection_handle, const ble_gatt_error* error,
                                     ble_gatt_attr* attribute, void* arg) {
    auto* client = static_cast<AncsClient*>(arg);
    if (connection_handle == client->connection_handle_) {
        const DescriptorTarget target = client->subscription_target_;
        client->FinishSubscription(target, error->status == 0);
    }
    return 0;
}

int AncsClient::ControlPointWriteComplete(uint16_t connection_handle, const ble_gatt_error* error,
                                          ble_gatt_attr* attribute, void* arg) {
    auto* client = static_cast<AncsClient*>(arg);
    if (connection_handle == client->connection_handle_ && error->status != 0) {
        ESP_LOGW(kTag, "ANCS Control Point write failed: %d", error->status);
        client->FinishRequest(false);
    }
    return 0;
}

void AncsClient::DataTimeout(struct ble_npl_event* event) {
    auto* client = static_cast<AncsClient*>(ble_npl_event_get_arg(event));
    if (client->request_active_) {
        ESP_LOGW(kTag, "ANCS attribute request timed out");
        client->FinishRequest(false);
    }
}

void AncsClient::DiscoveryTimeout(struct ble_npl_event* event) {
    auto* client = static_cast<AncsClient*>(ble_npl_event_get_arg(event));
    if (client->discovery_target_ == DiscoveryTarget::kGattService) {
        client->DiscoverServiceChanged();
    } else {
        client->DiscoverAncs();
    }
}

void AncsClient::OnReset(int reason) {
    ESP_LOGE(kTag, "NimBLE host reset: %d", reason);
    if (instance_ != nullptr) {
        instance_->ResetConnection();
    }
}

void AncsClient::OnSync() {
    if (instance_ == nullptr) {
        return;
    }
    const int result = ble_hs_util_ensure_addr(0);
    if (result != 0 || ble_hs_id_infer_auto(0, &instance_->own_address_type_) != 0) {
        ESP_LOGE(kTag, "Unable to select BLE identity address: %d", result);
        return;
    }
    instance_->StartAdvertising();
}

void AncsClient::HostTask(void* arg) {
    ESP_LOGI(kTag, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}
