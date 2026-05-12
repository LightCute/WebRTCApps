#include "apps/peerconnection/client/pc_factory.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"

#include <memory>
#include <utility>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "api/video_codecs/video_decoder_factory_template.h"
#include "api/video_codecs/video_decoder_factory_template_dav1d_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_decoder_factory_template_open_h264_adapter.h"
#include "api/video_codecs/video_encoder_factory_template.h"
#include "api/video_codecs/video_encoder_factory_template_libaom_av1_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp8_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_libvpx_vp9_adapter.h"
#include "api/video_codecs/video_encoder_factory_template_open_h264_adapter.h"
#include "apps/peerconnection/client/defaults.h"
#include "apps/peerconnection/client/rk_mpp_codec.h"
#include "rtc_base/logging.h"

PcComponents PcFactory::Create(webrtc::Thread* network_thread,
                                webrtc::Thread* worker_thread,
                                webrtc::Thread* signaling_thread,
                                const webrtc::Environment& env,
                                webrtc::AudioDeviceModule* adm,
                                webrtc::PeerConnectionObserver* observer) {
  PcComponents result;

  webrtc::PeerConnectionFactoryDependencies deps;
  deps.network_thread = network_thread;
  deps.worker_thread = worker_thread;
  deps.signaling_thread = signaling_thread;
  deps.env = env;
  deps.adm = adm;
  deps.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  deps.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  deps.video_encoder_factory =
      std::make_unique<webrtc::VideoEncoderFactoryTemplate<
          //webrtc::MppH264EncoderTemplateAdapter,
          webrtc::LibvpxVp8EncoderTemplateAdapter,
          webrtc::LibvpxVp9EncoderTemplateAdapter,
          webrtc::OpenH264EncoderTemplateAdapter,
          webrtc::LibaomAv1EncoderTemplateAdapter>>();
  deps.video_decoder_factory =
      std::make_unique<webrtc::VideoDecoderFactoryTemplate<
          //webrtc::MppH264DecoderTemplateAdapter,
          webrtc::LibvpxVp8DecoderTemplateAdapter,
          webrtc::LibvpxVp9DecoderTemplateAdapter,
          webrtc::OpenH264DecoderTemplateAdapter,
          webrtc::Dav1dDecoderTemplateAdapter>>();
  webrtc::EnableMedia(deps);

  result.factory =
      webrtc::CreateModularPeerConnectionFactory(std::move(deps));
  if (!result.factory) {
    RTC_LOG(LS_ERROR) << "Failed to create PeerConnectionFactory";
    return result;
  }

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

  webrtc::PeerConnectionInterface::IceServer stun_server;
  stun_server.uri = GetSTUNServer();
  config.servers.push_back(stun_server);

  webrtc::PeerConnectionInterface::IceServer turn_server;
  turn_server.uri = GetTURNServer();
  turn_server.username = GetTurnUserName();
  turn_server.password = GetTurnPassword();
  config.servers.push_back(turn_server);

  webrtc::PeerConnectionDependencies pc_deps(observer);
  auto error_or = result.factory->CreatePeerConnectionOrError(
      config, std::move(pc_deps));
  if (error_or.ok()) {
    result.connection = std::move(error_or.value());
  }
  return result;
}

#pragma GCC diagnostic pop
