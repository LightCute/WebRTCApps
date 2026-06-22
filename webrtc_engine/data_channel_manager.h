#ifndef APPS_PEERCONNECTION_CLIENT_DATA_CHANNEL_MANAGER_H_
#define APPS_PEERCONNECTION_CLIENT_DATA_CHANNEL_MANAGER_H_

#include <functional>
#include <string>

#include "api/data_channel_interface.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"

// Manages the lifecycle of a single negotiated DataChannel.
// Implements DataChannelObserver to receive state changes and messages.
class DataChannelManager : public webrtc::DataChannelObserver {
 public:
  using EventCallback = std::function<void(const std::string& json)>;

  DataChannelManager();
  ~DataChannelManager() override;

  void SetEventCallback(EventCallback cb);

  // Create the local DataChannel on the given PeerConnection.
  bool Add(webrtc::PeerConnectionInterface* pc);

  // Handle an incoming DataChannel from the remote peer.
  void OnRemoteDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> channel);

  // Send text through the DataChannel.
  void Send(const std::string& text);

  // Release the DataChannel.
  void Shutdown();

  // DataChannelObserver
  void OnStateChange() override;
  void OnMessage(const webrtc::DataBuffer& buffer) override;
  void OnBufferedAmountChange(uint64_t) override {}
  bool IsOkToCallOnTheNetworkThread() override { return false; }

 private:
  webrtc::scoped_refptr<webrtc::DataChannelInterface> channel_;
  EventCallback event_cb_;
};

#endif  // APPS_PEERCONNECTION_CLIENT_DATA_CHANNEL_MANAGER_H_
