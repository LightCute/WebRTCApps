// x64 PC Factory — software codecs (OpenH264, VP8, VP9, AV1).
#ifndef APPS_PEERCONNECTION_PLATFORM_X64_PC_FACTORY_X64_H_
#define APPS_PEERCONNECTION_PLATFORM_X64_PC_FACTORY_X64_H_

#include <memory>

#include "apps/peerconnection/engine/pc_factory_interface.h"

class PcFactoryX64 : public IPcFactory {
 public:
  PcFactoryX64() = default;
  ~PcFactoryX64() override = default;

  PcComponents Create(webrtc::Thread* network_thread,
                      webrtc::Thread* worker_thread,
                      webrtc::Thread* signaling_thread,
                      const webrtc::Environment& env,
                      webrtc::AudioDeviceModule* adm,
                      webrtc::PeerConnectionObserver* observer) override;
};

#endif  // APPS_PEERCONNECTION_PLATFORM_X64_PC_FACTORY_X64_H_
