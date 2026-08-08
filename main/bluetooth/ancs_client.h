#pragma once

#include "phone_notification.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

struct ble_gap_event;
struct ble_gatt_error;
struct ble_gatt_svc;
struct ble_gatt_chr;
struct ble_gatt_dsc;
struct ble_gatt_attr;

class AncsClient {
public:
    using NotificationCallback = std::function<void(PhoneNotification)>;

    explicit AncsClient(NotificationCallback callback);
    ~AncsClient();

    AncsClient(const AncsClient&) = delete;
    AncsClient& operator=(const AncsClient&) = delete;

    bool Start();
    void Stop();

private:
    static constexpr size_t kMaxDataSourceBytes = 2048;
    void StartAdvertising();
    void DiscoverAncs(uint16_t conn_handle);
    void RequestNotificationAttributes(uint32_t uid);
    void QueueNotificationAttributes(uint32_t uid);
    void ProcessDataSource();
    void PublishPendingNotification();
    void ResetConnection();

    static int GapEvent(struct ble_gap_event* event, void* arg);
    static int ServiceDiscovered(uint16_t conn_handle, const struct ble_gatt_error* error,
                                 const struct ble_gatt_svc* service, void* arg);
    static int CharacteristicDiscovered(uint16_t conn_handle, const struct ble_gatt_error* error,
                                        const struct ble_gatt_chr* characteristic, void* arg);
    static int DescriptorDiscovered(uint16_t conn_handle, const struct ble_gatt_error* error,
                                    uint16_t characteristic_handle,
                                    const struct ble_gatt_dsc* descriptor, void* arg);
    static int SubscriptionComplete(uint16_t conn_handle, const struct ble_gatt_error* error,
                                    struct ble_gatt_attr* attr, void* arg);
    static void OnDataSourceTimeout(void* arg);
    static void OnReset(int reason);
    static void OnSync();
    static void HostTask(void* arg);

    static AncsClient* instance_;

    NotificationCallback on_notification_;
    uint8_t own_addr_type_ = 0;
    uint16_t connection_handle_ = 0xffff;
    uint16_t service_end_handle_ = 0;
    uint16_t notification_source_handle_ = 0;
    uint16_t data_source_handle_ = 0;
    uint16_t control_point_handle_ = 0;
    uint32_t pending_uid_ = 0;
    std::deque<uint32_t> pending_requests_;
    bool started_ = false;
    bool connected_ = false;
    bool ancs_ready_ = false;
    bool data_timer_created_ = false;
    void* data_timer_ = nullptr;
    std::vector<uint8_t> data_source_buffer_;
};
