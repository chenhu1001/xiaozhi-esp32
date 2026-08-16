#pragma once

#include "ancs_attribute_parser.h"
#include "ancs_notification.h"
#include "ancs_preexisting_filter.h"

#include <nimble/nimble_npl.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct ble_gap_event;
struct ble_gatt_attr;
struct ble_gatt_chr;
struct ble_gatt_dsc;
struct ble_gatt_error;
struct ble_gatt_svc;
struct os_mbuf;

class AncsClient {
public:
    using NotificationCallback = std::function<void(AncsNotification)>;

    explicit AncsClient(NotificationCallback callback);
    ~AncsClient();

    AncsClient(const AncsClient&) = delete;
    AncsClient& operator=(const AncsClient&) = delete;

    bool Start();
    void Stop();

private:
    enum class DiscoveryTarget {
        kNone,
        kGattService,
        kAncsService,
    };

    enum class DescriptorTarget {
        kNone,
        kServiceChanged,
        kDataSource,
        kNotificationSource,
    };

    struct RemoteCharacteristic {
        uint16_t definition_handle = 0;
        uint16_t value_handle = 0;
        uint16_t descriptor_end_handle = 0;
        uint8_t kind = 0;
    };

    struct PendingRequest {
        AncsNotification notification;
    };

    void StartAdvertising();
    void BeginSecurity(uint16_t connection_handle);
    bool VerifyBondedPeer(uint16_t connection_handle) const;
    void DiscoverServiceChanged();
    void DiscoverAncs();
    void DiscoverCharacteristics();
    void DiscoverDescriptors(DescriptorTarget target, uint16_t value_handle, uint16_t end_handle);
    void Subscribe(DescriptorTarget target, uint16_t cccd_handle);
    void FinishSubscription(DescriptorTarget target, bool success);
    void ScheduleAncsDiscovery(uint32_t delay_ms);
    void InvalidateAncs();

    void HandleNotificationSource(struct os_mbuf* mbuf);
    void HandleDataSource(struct os_mbuf* mbuf);
    void HandleNotificationRecord(const uint8_t* record);
    void QueueAttributeRequest(AncsNotification notification);
    void StartNextRequest();
    void FinishRequest(bool attributes_complete);
    void HandleRemoved(AncsNotification notification);
    void Publish(AncsNotification notification);
    void StoreInCache(const AncsNotification& notification);
    void ResetConnection();

    static int GapEvent(struct ble_gap_event* event, void* arg);
    static int ServiceDiscovered(uint16_t connection_handle, const struct ble_gatt_error* error,
                                 const struct ble_gatt_svc* service, void* arg);
    static int CharacteristicDiscovered(uint16_t connection_handle,
                                        const struct ble_gatt_error* error,
                                        const struct ble_gatt_chr* characteristic, void* arg);
    static int DescriptorDiscovered(uint16_t connection_handle, const struct ble_gatt_error* error,
                                    uint16_t characteristic_handle,
                                    const struct ble_gatt_dsc* descriptor, void* arg);
    static int SubscriptionComplete(uint16_t connection_handle, const struct ble_gatt_error* error,
                                    struct ble_gatt_attr* attribute, void* arg);
    static int ControlPointWriteComplete(uint16_t connection_handle,
                                         const struct ble_gatt_error* error,
                                         struct ble_gatt_attr* attribute, void* arg);
    static void DataTimeout(struct ble_npl_event* event);
    static void DiscoveryTimeout(struct ble_npl_event* event);
    static void OnReset(int reason);
    static void OnSync();
    static void HostTask(void* arg);

    static AncsClient* instance_;

    NotificationCallback on_notification_;
    uint8_t own_address_type_ = 0;
    uint16_t connection_handle_ = 0xffff;
    uint16_t service_start_handle_ = 0;
    uint16_t service_end_handle_ = 0;
    uint16_t service_changed_handle_ = 0;
    uint16_t notification_source_handle_ = 0;
    uint16_t data_source_handle_ = 0;
    uint16_t control_point_handle_ = 0;
    uint16_t descriptor_cccd_handle_ = 0;
    DiscoveryTarget discovery_target_ = DiscoveryTarget::kNone;
    DescriptorTarget descriptor_target_ = DescriptorTarget::kNone;
    DescriptorTarget subscription_target_ = DescriptorTarget::kNone;
    std::vector<RemoteCharacteristic> characteristics_;
    std::vector<uint8_t> notification_source_buffer_;
    std::deque<PendingRequest> pending_requests_;
    PendingRequest active_request_;
    bool request_active_ = false;
    AncsAttributeParser attribute_parser_;
    std::unordered_map<uint32_t, AncsNotification> notification_cache_;
    AncsPreExistingFilter pre_existing_filter_{256};
    std::string session_id_;
    uint64_t sequence_ = 0;
    bool started_ = false;
    bool connected_ = false;
    bool encrypted_ = false;
    bool ancs_ready_ = false;
    struct ble_npl_callout data_timeout_ = {};
    struct ble_npl_callout discovery_timeout_ = {};
};
