#include "media/video_source.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "api/test/create_frame_generator.h"
#include "api/video/video_frame.h"
#include "media/base/video_broadcaster.h"
#include "modules/video_capture/video_capture.h"
#include "modules/video_capture/video_capture_factory.h"
#include "pc/video_track_source.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/clock.h"
#include "test/frame_generator_capturer.h"
#include "test/platform_video_capturer.h"

namespace webrtc_media {

namespace {

// 视频源：VideoBroadcaster 是稳定路由器，编码器 sink 永远挂在它上面；
// 切换设备只改「broadcaster 订阅哪个 capturer」，sink 列表不动。
class MediaVideoSource : public webrtc::VideoTrackSource {
 public:
  static webrtc::scoped_refptr<MediaVideoSource> Create(
      const VideoDeviceInfo& dev,
      webrtc::TaskQueueFactory& task_queue_factory) {
    auto capturer = MakeCapturer(dev, task_queue_factory);
    if (!capturer) {
      RTC_LOG(LS_ERROR) << "CreateVideoSource: MakeCapturer failed for "
                        << dev.id;
      return nullptr;
    }
    auto source = webrtc::make_ref_counted<MediaVideoSource>(&task_queue_factory);
    source->SetCapturer(std::move(capturer));
    source->current_ = dev;
    return source;
  }

  bool SwitchDevice(const VideoDeviceInfo& dev) {
    const bool same_device = (current_.id == dev.id);
    if (same_device) {
      // 同一设备换档位：V4L2 同一节点不能被两个实例同时打开，
      // 必须先释放旧 capturer 再建新的；失败时回滚到原档位。
      const VideoDeviceInfo previous = current_;
      if (capturer_) {
        capturer_->Stop();
        capturer_.reset();
      }
      auto new_capturer = MakeCapturer(dev, *task_queue_factory_);
      if (!new_capturer) {
        auto rollback = MakeCapturer(previous, *task_queue_factory_);
        if (rollback) {
          rollback->AddOrUpdateSink(&broadcaster_, broadcaster_.wants());
          rollback->Start();
          capturer_ = std::move(rollback);
        }
        RTC_LOG(LS_ERROR) << "SwitchDevice(profile): MakeCapturer failed for "
                          << dev.id << " " << dev.width << "x" << dev.height
                          << "@" << dev.fps;
        return false;
      }
      new_capturer->AddOrUpdateSink(&broadcaster_, broadcaster_.wants());
      new_capturer->Start();
      capturer_ = std::move(new_capturer);
      current_ = dev;
      RTC_LOG(LS_INFO) << "Video profile switched to " << dev.width << "x"
                       << dev.height << "@" << dev.fps;
      return true;
    }

    // 不同设备：先建新的再停旧的（尽量无缝）。
    auto new_capturer = MakeCapturer(dev, *task_queue_factory_);
    if (!new_capturer) {
      RTC_LOG(LS_ERROR) << "SwitchDevice: MakeCapturer failed for " << dev.id;
      return false;  // 保留原设备
    }
    // broadcaster_ 不变，编码器 sink 不受影响。
    new_capturer->AddOrUpdateSink(&broadcaster_, broadcaster_.wants());
    new_capturer->Start();
    if (capturer_) {
      capturer_->Stop();
      capturer_.reset();
    }
    capturer_ = std::move(new_capturer);
    current_ = dev;
    RTC_LOG(LS_INFO) << "Video device switched to " << dev.id << " ("
                     << dev.width << "x" << dev.height << "@" << dev.fps << ")";
    return true;
  }

  // 编码器/渲染器的 wants（尤其 max_framerate）通过这里进来。
  // 先存进 broadcaster，再把**聚合后的 wants** 转给 capturer —— 否则
  // WebRTC 的 SetFrameRateUpperLimit 到不了采集端，set_framerate 不生效。
  void AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink,
                       const webrtc::VideoSinkWants& wants) override {
    broadcaster_.AddOrUpdateSink(sink, wants);
    ApplyWantsToCapturer();
  }

  void RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame>* sink) override {
    broadcaster_.RemoveSink(sink);
    ApplyWantsToCapturer();
  }

  // 稳定路由器：sink 全部挂在 broadcaster 上，永不随设备切换而迁移。
  webrtc::VideoSourceInterface<webrtc::VideoFrame>* source() override {
    return &broadcaster_;
  }

 protected:
  explicit MediaVideoSource(webrtc::TaskQueueFactory* task_queue_factory)
      : webrtc::VideoTrackSource(/*remote=*/false),
        task_queue_factory_(task_queue_factory) {}
  ~MediaVideoSource() override = default;

 private:
  void SetCapturer(
      std::unique_ptr<webrtc::test::TestVideoCapturer> capturer) {
    capturer->AddOrUpdateSink(&broadcaster_, broadcaster_.wants());
    capturer->Start();
    capturer_ = std::move(capturer);
  }

  // 把 broadcaster 聚合后的 wants 转给当前 capturer（帧率/分辨率自适应）。
  void ApplyWantsToCapturer() {
    if (capturer_) {
      capturer_->AddOrUpdateSink(&broadcaster_, broadcaster_.wants());
    }
  }

  static std::unique_ptr<webrtc::test::TestVideoCapturer> MakeCapturer(
      const VideoDeviceInfo& dev,
      webrtc::TaskQueueFactory& task_queue_factory) {
    if (dev.is_synthetic) {
      auto generator = webrtc::test::CreateSquareFrameGenerator(
          dev.width, dev.height, std::nullopt, std::nullopt);
      return std::make_unique<webrtc::test::FrameGeneratorCapturer>(
          webrtc::Clock::GetRealTimeClock(), std::move(generator),
          dev.fps > 0 ? dev.fps : 30, task_queue_factory);
    }
    const int index = ResolveV4l2Index(dev);
    if (index < 0) {
      RTC_LOG(LS_ERROR) << "MakeCapturer: V4L2 device not found: " << dev.id;
      return nullptr;
    }
    return webrtc::test::CreateVideoCapturer(dev.width, dev.height, dev.fps,
                                             index);
  }

  webrtc::TaskQueueFactory* task_queue_factory_;
  webrtc::VideoBroadcaster broadcaster_;
  std::unique_ptr<webrtc::test::TestVideoCapturer> capturer_;
  VideoDeviceInfo current_;  // 当前设备+档位（用于判断同设备换档位）
};

}  // namespace

webrtc::scoped_refptr<webrtc::VideoTrackSourceInterface> CreateVideoSource(
    const VideoDeviceInfo& dev,
    webrtc::TaskQueueFactory& task_queue_factory) {
  return MediaVideoSource::Create(dev, task_queue_factory);
}

bool SwitchVideoSourceDevice(webrtc::VideoTrackSourceInterface* source,
                             const VideoDeviceInfo& dev) {
  auto* media_source = static_cast<MediaVideoSource*>(source);
  if (!media_source) {
    return false;
  }
  return media_source->SwitchDevice(dev);
}

}  // namespace webrtc_media
