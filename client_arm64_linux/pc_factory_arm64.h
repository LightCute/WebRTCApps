// pc_factory_arm64.h — ARM64 IPcFactory adapter (MPP hardware codecs).
#ifndef APPS_CLIENT_ARM64_LINUX_PC_FACTORY_ARM64_H_
#define APPS_CLIENT_ARM64_LINUX_PC_FACTORY_ARM64_H_

#include <memory>
#include "apps/webrtc_engine/pc_factory_interface.h"

class PcFactoryArm64 : public IPcFactory {
 public:
  PcFactoryArm64() = default;
  ~PcFactoryArm64() override = default;

  PcComponents Create(webrtc::Thread* network_thread,
                      webrtc::Thread* worker_thread,
                      webrtc::Thread* signaling_thread,
                      const webrtc::Environment& env,
                      webrtc::AudioDeviceModule* adm,
                      webrtc::PeerConnectionObserver* observer) override;
};

#endif  // APPS_CLIENT_ARM64_LINUX_PC_FACTORY_ARM64_H_
