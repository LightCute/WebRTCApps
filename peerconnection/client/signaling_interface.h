#ifndef APPS_PEERCONNECTION_CLIENT_SIGNALING_INTERFACE_H_
#define APPS_PEERCONNECTION_CLIENT_SIGNALING_INTERFACE_H_

#include <map>
#include <string>

typedef std::map<int, std::string> Peers;

// Observer interface for signaling events.
// Engine implements this to react to server/peer state changes.
struct PeerConnectionClientObserver {
  virtual void OnSignedIn() = 0;
  virtual void OnDisconnected() = 0;
  virtual void OnPeerConnected(int id, const std::string& name) = 0;
  virtual void OnPeerDisconnected(int peer_id) = 0;
  virtual void OnPeerBusy(int peer_id) = 0;
  virtual void OnMessageFromPeer(int peer_id, const std::string& message) = 0;
  virtual void OnMessageSent(int err) = 0;
  virtual void OnServerConnectionFailure() = 0;

 protected:
  virtual ~PeerConnectionClientObserver() {}
};

// Abstract interface for signaling server communication.
// PeerConnectionClient is the concrete HTTP long-polling implementation.
class SignalingInterface {
 public:
  virtual ~SignalingInterface() = default;

  virtual int id() const = 0;
  virtual bool is_connected() const = 0;
  virtual const Peers& peers() const = 0;

  virtual void RegisterObserver(PeerConnectionClientObserver* callback) = 0;

  virtual void Connect(const std::string& server,
                       int port,
                       const std::string& client_name) = 0;

  virtual bool SendToPeer(int peer_id, const std::string& message) = 0;
  virtual bool SendHangUp(int peer_id) = 0;
  virtual void SendHangUpConfirm() = 0;
  virtual bool IsSendingMessage() = 0;

  virtual bool SignOut() = 0;
  virtual void Close() = 0;
};

#endif  // APPS_PEERCONNECTION_CLIENT_SIGNALING_INTERFACE_H_
