#include "ancs_relay_service.h"

#include <esp_log.h>

#include <utility>

namespace {
constexpr char kTag[] = "AncsRelayService";
}

AncsRelayService::AncsRelayService()
    : client_([this](AncsNotification notification) { relay_.Enqueue(std::move(notification)); }) {}

AncsRelayService::~AncsRelayService() { Stop(); }

bool AncsRelayService::Start() {
    if (started_) {
        return true;
    }
    if (!relay_.Start()) {
        return false;
    }
    if (!client_.Start()) {
        relay_.Stop();
        return false;
    }
    started_ = true;
    ESP_LOGI(kTag, "iOS ANCS relay started");
    return true;
}

void AncsRelayService::Stop() {
    if (!started_) {
        return;
    }
    client_.Stop();
    relay_.Stop();
    started_ = false;
}

void AncsRelayService::SetNetworkConnected(bool connected) {
    relay_.SetNetworkConnected(connected);
}
