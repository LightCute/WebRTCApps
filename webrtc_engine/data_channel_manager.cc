#include "apps/webrtc_engine/data_channel_manager.h"

#include <string>

#include "apps/webrtc_engine/json_helpers.h"
#include "rtc_base/logging.h"

DataChannelManager::DataChannelManager() = default;
DataChannelManager::~DataChannelManager() = default;

void DataChannelManager::SetEventCallback(EventCallback cb) {
  event_cb_ = std::move(cb);
}

bool DataChannelManager::Add(webrtc::PeerConnectionInterface* pc) {
  if (!pc) {
    RTC_LOG(LS_WARNING) << "DataChannelManager::Add: no peer connection";
    return false;
  }
  if (channel_) {
    RTC_LOG(LS_WARNING) << "DataChannelManager::Add: channel already exists";
    return false;
  }

  webrtc::DataChannelInit config;
  config.ordered = true;
  config.negotiated = true;
  config.id = 0;

  auto dc_or_error = pc->CreateDataChannelOrError("chat", &config);
  if (!dc_or_error.ok()) {
    RTC_LOG(LS_ERROR) << "Failed to create DataChannel: "
                      << dc_or_error.error().message();
    return false;
  }

  channel_ = std::move(dc_or_error.value());
  channel_->RegisterObserver(this);
  RTC_LOG(LS_INFO) << "DataChannel created - label: " << channel_->label()
                   << " - state: " << channel_->state();
  return true;
}

void DataChannelManager::OnRemoteDataChannel(
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
  if (!channel) {
    RTC_LOG(LS_ERROR) << "DataChannelManager: received null DataChannel";
    return;
  }
  if (channel->label() != "chat") {
    RTC_LOG(LS_WARNING) << "DataChannelManager: unexpected label: "
                        << channel->label();
    return;
  }
  if (channel_) {
    RTC_LOG(LS_WARNING) << "DataChannelManager: channel already exists, replacing";
  }
  channel_ = channel;
  channel_->RegisterObserver(this);
  RTC_LOG(LS_INFO) << "DataChannel received and observer registered"
                   << " - label: " << channel_->label();
}

void DataChannelManager::Send(const std::string& text) {
  if (channel_ && channel_->state() == webrtc::DataChannelInterface::kOpen) {
    channel_->Send(webrtc::DataBuffer(text));
  } else {
    RTC_LOG(LS_WARNING) << "DataChannelManager::Send: channel not open";
  }
}

void DataChannelManager::Shutdown() {
  channel_ = nullptr;
}

void DataChannelManager::OnStateChange() {
  if (channel_) {
    const char* state_str =
        DataChannelStateToString(channel_->state());
    RTC_LOG(LS_INFO) << "DataChannel state: " << state_str;
    if (event_cb_)
      event_cb_(std::string(R"({"event":"data_channel_state","state":")") +
                state_str + R"("})");
  }
}

void DataChannelManager::OnMessage(const webrtc::DataBuffer& buffer) {
  RTC_LOG(LS_INFO) << "DataChannel message received";
  std::string text(buffer.data.data<char>(), buffer.data.size());
  if (event_cb_)
    event_cb_(R"({"event":"data_received","text":")" +
              EscapeJsonString(text) + R"("})");
}
