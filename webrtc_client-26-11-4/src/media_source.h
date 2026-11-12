#ifndef APPS_WEBRTC_CLIENT_MEDIA_SOURCE_H_
#define APPS_WEBRTC_CLIENT_MEDIA_SOURCE_H_

#include <memory>

#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"

// 媒体源/编解码相关的工厂函数。单独放一个编译单元（不含信令头、无 RTTI），
// 因为 VideoTrackSource / VideoEncoderFactory 等 WebRTC 内部类是 -fno-rtti 编的，
// 派生它们的代码若开 RTTI 会引 typeinfo 未定义链接错误。
namespace webrtc_media {

// 合成视频源（移动方块测试图，640x480@30）。
webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface>
CreateSyntheticVideoSource(webrtc::TaskQueueFactory& task_queue_factory);

// 远端视频帧计数 sink（第一阶段：只计数 + 打日志）。
std::unique_ptr<webrtc::VideoSinkInterface<webrtc::VideoFrame>>
CreateRemoteVideoFrameCounter();

// 给 factory deps 配 VP8 编解码器 + EnableMedia。
void ConfigureVideoCodecs(webrtc::PeerConnectionFactoryDependencies& deps);

}  // namespace webrtc_media

#endif  // APPS_WEBRTC_CLIENT_MEDIA_SOURCE_H_
