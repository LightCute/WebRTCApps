#ifndef APPS_WEBRTC_CLIENT_MEDIA_VIDEO_CODEC_H_
#define APPS_WEBRTC_CLIENT_MEDIA_VIDEO_CODEC_H_

#include <memory>

#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"

// 编解码器配置 + 远端帧计数 sink。
// 单独编译单元（无 RTTI）：VideoEncoderFactory 等 WebRTC 内部类是 -fno-rtti 编的。
namespace webrtc_media {

// 远端视频帧计数 sink（只计数 + 打日志，不渲染）。
std::unique_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>>
CreateRemoteVideoFrameCounter();

// 给 factory deps 配音视频 codec + EnableMedia。
void ConfigureVideoCodecs(webrtc::PeerConnectionFactoryDependencies& deps);

}  // namespace webrtc_media

#endif  // APPS_WEBRTC_CLIENT_MEDIA_VIDEO_CODEC_H_
