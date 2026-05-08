#ifndef APPS_PEERCONNECTION_CLIENT_ENGINE_CONTROLLER_H_
#define APPS_PEERCONNECTION_CLIENT_ENGINE_CONTROLLER_H_

#include <string>

// Events emitted by the engine. All callbacks fire on the signaling thread.
// Implementations must return quickly (non-blocking).
class EngineObserver {
 public:
  virtual ~EngineObserver() = default;
  virtual void OnEngineEvent(const std::string& json) = 0;
};

// Thread-safe control interface for the WebRTC engine.
// All methods may be called from any thread; the engine dispatches internally.
class EngineController {
 public:
  virtual ~EngineController() = default;

  virtual void RegisterObserver(EngineObserver* observer) = 0;
  virtual void UnregisterObserver() = 0;

  virtual void ConnectToServer(const std::string& server, int port) = 0;
  virtual void DisconnectFromServer() = 0;
  virtual void ConnectToPeer(int peer_id) = 0;
  virtual void HangUp() = 0;
  virtual void SetAudioMuted(bool muted) = 0;
  virtual void SetVideoPaused(bool paused) = 0;
  virtual void SendData(const std::string& text) = 0;
  virtual void QueryDevices() = 0;
  virtual void SetVideoDevice(int device_idx) = 0;
  virtual void SetAudioInputDevice(int device_idx) = 0;

  virtual bool connection_active() const = 0;
};

#endif  // APPS_PEERCONNECTION_CLIENT_ENGINE_CONTROLLER_H_
