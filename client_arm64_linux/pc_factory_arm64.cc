// pc_factory_arm64.cc
#include "apps/client_arm64_linux/pc_factory_arm64.h"
#include "apps/client_arm64_linux/pc_factory.h"

PcComponents PcFactoryArm64::Create(
    webrtc::Thread* network_thread,
    webrtc::Thread* worker_thread,
    webrtc::Thread* signaling_thread,
    const webrtc::Environment& env,
    webrtc::AudioDeviceModule* adm,
    webrtc::PeerConnectionObserver* observer) {
  return PcFactory::Create(network_thread, worker_thread, signaling_thread,
                           env, adm, observer);
}
