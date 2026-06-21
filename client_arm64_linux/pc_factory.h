#ifndef APPS_PEERCONNECTION_CLIENT_PC_FACTORY_H_
#define APPS_PEERCONNECTION_CLIENT_PC_FACTORY_H_

#include "api/environment/environment.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "apps/webrtc_engine/pc_factory_interface.h"
#include "rtc_base/thread.h"

// Pure factory: creates PeerConnectionFactory + PeerConnection
// from threads + ADM. No state, no callbacks.
class PcFactory {
 public:
  static PcComponents Create(webrtc::Thread* network_thread,
                             webrtc::Thread* worker_thread,
                             webrtc::Thread* signaling_thread,
                             const webrtc::Environment& env,
                             webrtc::AudioDeviceModule* adm,
                             webrtc::PeerConnectionObserver* observer);
};

#endif  // APPS_PEERCONNECTION_CLIENT_PC_FACTORY_H_
