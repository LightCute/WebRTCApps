/*
 *  Copyright 2011 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef APPS_PEERCONNECTION_CLIENT_PEER_CONNECTION_CLIENT_H_
#define APPS_PEERCONNECTION_CLIENT_PEER_CONNECTION_CLIENT_H_

#include <cstddef>
#include <memory>
#include <string>

#include "api/async_dns_resolver.h"
#include "api/task_queue/pending_task_safety_flag.h"
#include "apps/peerconnection/client_arm64/signaling_interface.h"
#include "rtc_base/socket.h"
#include "rtc_base/socket_address.h"

class PeerConnectionClient : public SignalingInterface {
 public:
  enum State {
    NOT_CONNECTED,
    RESOLVING,
    SIGNING_IN,
    CONNECTED,
    SIGNING_OUT_WAITING,
    SIGNING_OUT,
  };

  PeerConnectionClient();
  ~PeerConnectionClient();

  int id() const override;
  bool is_connected() const override;
  const Peers& peers() const override;

  void RegisterObserver(PeerConnectionClientObserver* callback) override;

  void Connect(const std::string& server,
               int port,
               const std::string& client_name) override;

  bool SendToPeer(int peer_id, const std::string& message) override;
  bool SendHangUp(int peer_id) override;
  void SendHangUpConfirm() override;
  bool IsSendingMessage() override;

  bool SignOut() override;
  void Close() override;

 protected:
  void DoConnect();
  void InitSocketSignals();
  bool ConnectControlSocket();
  void OnConnect(webrtc::Socket* socket);
  void OnHangingGetConnect(webrtc::Socket* socket);
  void OnMessageFromPeer(int peer_id, const std::string& message);

  // Quick and dirty support for parsing HTTP header values.
  bool GetHeaderValue(const std::string& data,
                      size_t eoh,
                      const char* header_pattern,
                      size_t* value);

  bool GetHeaderValue(const std::string& data,
                      size_t eoh,
                      const char* header_pattern,
                      std::string* value);

  // Returns true if the whole response has been read.
  bool ReadIntoBuffer(webrtc::Socket* socket,
                      std::string* data,
                      size_t* content_length);

  void OnRead(webrtc::Socket* socket);

  void OnHangingGetRead(webrtc::Socket* socket);

  // Parses a single line entry in the form "<name>,<id>,<connected>"
  bool ParseEntry(const std::string& entry,
                  std::string* name,
                  int* id,
                  bool* connected);

  int GetResponseStatus(const std::string& response);

  bool ParseServerResponse(const std::string& response,
                           size_t content_length,
                           size_t* peer_id,
                           size_t* eoh);

  void OnClose(webrtc::Socket* socket, int err);

  void OnResolveResult(const webrtc::AsyncDnsResolverResult& result);

  PeerConnectionClientObserver* callback_;
  webrtc::SocketAddress server_address_;
  std::unique_ptr<webrtc::AsyncDnsResolverInterface> resolver_;
  std::unique_ptr<webrtc::Socket> control_socket_;
  std::unique_ptr<webrtc::Socket> hanging_get_;
  std::string onconnect_data_;
  std::string control_data_;
  std::string notification_data_;
  std::string client_name_;
  Peers peers_;
  State state_;
  int my_id_;
  webrtc::scoped_refptr<webrtc::PendingTaskSafetyFlag> safety_;
};

#endif  // APPS_PEERCONNECTION_CLIENT_PEER_CONNECTION_CLIENT_H_
