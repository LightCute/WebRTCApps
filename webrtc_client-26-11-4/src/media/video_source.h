#ifndef APPS_WEBRTC_CLIENT_MEDIA_VIDEO_SOURCE_H_
#define APPS_WEBRTC_CLIENT_MEDIA_VIDEO_SOURCE_H_

#include "api/media_stream_interface.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/task_queue_factory.h"
#include "api/video/video_source_interface.h"
#include "media/video_device.h"

// 视频源：内部用 VideoBroadcaster 做稳定路由器（换设备时不丢 sink）。
// 单独编译单元（无 RTTI）：VideoTrackSource / capturer 相关类都是 -fno-rtti 编的。
namespace webrtc_media {

// 按设备创建视频源；失败返回 nullptr。
webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> CreateVideoSource(
    const VideoDeviceInfo& dev,
    webrtc::TaskQueueFactory& task_queue_factory);

// 通话中切换采集设备（不重协商：track/sender/SSRC 不变）。
// 必须在本端 worker 线程调用；失败返回 false 且保留原设备。
bool SwitchVideoSourceDevice(webrtc::VideoTrackSourceInterface* source,
                             const VideoDeviceInfo& dev);

}  // namespace webrtc_media

#endif  // APPS_WEBRTC_CLIENT_MEDIA_VIDEO_SOURCE_H_
