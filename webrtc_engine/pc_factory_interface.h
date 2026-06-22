// PeerConnection factory abstraction — platform codecs differ.
#ifndef APPS_PEERCONNECTION_ENGINE_PC_FACTORY_INTERFACE_H_
#define APPS_PEERCONNECTION_ENGINE_PC_FACTORY_INTERFACE_H_

#include <memory>

#include "api/environment/environment.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "rtc_base/thread.h"

struct PcComponents {
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> connection;
};

class IPcFactory {
 public:
  virtual ~IPcFactory() = default;

  virtual PcComponents Create(
      webrtc::Thread* network_thread,
      webrtc::Thread* worker_thread,
      webrtc::Thread* signaling_thread,
      const webrtc::Environment& env,
      webrtc::AudioDeviceModule* adm,
      webrtc::PeerConnectionObserver* observer) = 0;
};

#endif  // APPS_PEERCONNECTION_ENGINE_PC_FACTORY_INTERFACE_H_
