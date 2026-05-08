#ifndef APPS_PEERCONNECTION_TEST_ENGINECONTROLLER_FAKE_ENGINE_CONTROLLER_H_
#define APPS_PEERCONNECTION_TEST_ENGINECONTROLLER_FAKE_ENGINE_CONTROLLER_H_

#include <string>
#include <vector>

#include "apps/peerconnection/client/engine_controller.h"

class FakeEngineController : public EngineController {
 public:
  struct CallRecord {
    std::string method;
    std::string arg_str;
    int arg_int = 0;
    bool arg_bool = false;
  };

  std::vector<CallRecord> calls;
  EngineObserver* observer = nullptr;

  // Control: set what connection_active() returns
  bool active = false;

  // Control: fire an event as if the engine emitted it
  void EmitEvent(const std::string& json) {
    if (observer)
      observer->OnEngineEvent(json);
  }

  // ---- EngineController ----

  void RegisterObserver(EngineObserver* obs) override { observer = obs; }
  void UnregisterObserver() override { observer = nullptr; }

  void ConnectToServer(const std::string& server, int port) override {
    calls.push_back({"ConnectToServer", server, port});
  }
  void DisconnectFromServer() override {
    calls.push_back({"DisconnectFromServer"});
  }
  void ConnectToPeer(int peer_id) override {
    calls.push_back({"ConnectToPeer", "", peer_id});
  }
  void HangUp() override {
    calls.push_back({"HangUp"});
  }
  void SetAudioMuted(bool muted) override {
    calls.push_back({"SetAudioMuted", "", 0, muted});
  }
  void SetVideoPaused(bool paused) override {
    calls.push_back({"SetVideoPaused", "", 0, paused});
  }
  void SendData(const std::string& text) override {
    calls.push_back({"SendData", text});
  }
  void QueryDevices() override {
    calls.push_back({"QueryDevices"});
  }
  void SetVideoDevice(int device_idx) override {
    calls.push_back({"SetVideoDevice", "", device_idx});
  }
  void SetAudioInputDevice(int device_idx) override {
    calls.push_back({"SetAudioInputDevice", "", device_idx});
  }
  bool connection_active() const override { return active; }
};

#endif  // APPS_PEERCONNECTION_TEST_ENGINECONTROLLER_FAKE_ENGINE_CONTROLLER_H_
