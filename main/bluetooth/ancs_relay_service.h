#pragma once

#include "ancs_client.h"
#include "notification_relay.h"

class AncsRelayService {
public:
    AncsRelayService();
    ~AncsRelayService();

    AncsRelayService(const AncsRelayService&) = delete;
    AncsRelayService& operator=(const AncsRelayService&) = delete;

    bool Start();
    void Stop();
    void SetNetworkConnected(bool connected);

private:
    NotificationRelay relay_;
    AncsClient client_;
    bool started_ = false;
};
