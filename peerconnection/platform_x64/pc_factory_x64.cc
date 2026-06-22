#include "apps/peerconnection/platform_x64/pc_factory_x64.h"

#include "apps/peerconnection/client/pc_factory.h"

PcComponents PcFactoryX64::Create(
    webrtc::Thread* network_thread,
    webrtc::Thread* worker_thread,
    webrtc::Thread* signaling_thread,
    const webrtc::Environment& env,
    webrtc::AudioDeviceModule* adm,
    webrtc::PeerConnectionObserver* observer) {
  return PcFactory::Create(network_thread, worker_thread, signaling_thread,
                           env, adm, observer);
}
