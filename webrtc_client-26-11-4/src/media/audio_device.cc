#include "media/audio_device.h"

#include <cstring>
#include <vector>

#include "api/audio/create_audio_device_module.h"
#include "media/pulse_query.h"
#include "rtc_base/logging.h"

namespace webrtc_media {

webrtc::scoped_refptr<webrtc::AudioDeviceModule> CreatePlatformAudioDeviceModule(
    const webrtc::Environment& env) {
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm =
      webrtc::CreateAudioDeviceModule(
          env, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
  if (!adm) {
    RTC_LOG(LS_ERROR) << "CreateAudioDeviceModule failed";
    return nullptr;
  }
  if (adm->Init() != 0) {
    RTC_LOG(LS_ERROR) << "AudioDeviceModule Init() failed";
    return nullptr;
  }
  RTC_LOG(LS_INFO) << "AudioDeviceModule created (recording devices="
                   << adm->RecordingDevices() << ")";
  return adm;
}

std::vector<AudioDeviceInfo> EnumerateRecordingDevices(
    webrtc::AudioDeviceModule* adm) {
  std::vector<AudioDeviceInfo> devices;
  if (adm == nullptr) {
    return devices;
  }
  // 额外查一次 Pulse，带上端口可用性/硬件采样率（ADM 不暴露这些）。
  const std::vector<PulseSourceInfo> sources = QueryPulseSources();
  std::vector<bool> used(sources.size(), false);
  int source_cursor = 0;
  constexpr char kDefaultPrefix[] = "default: ";

  const int16_t count = adm->RecordingDevices();
  for (int16_t i = 0; i < count; ++i) {
    char name[webrtc::kAdmMaxDeviceNameSize] = {0};
    char guid[webrtc::kAdmMaxGuidSize] = {0};
    if (adm->RecordingDeviceName(static_cast<uint16_t>(i), name, guid) != 0) {
      continue;
    }
    AudioDeviceInfo info;
    info.index = i;
    info.name = name;

    const std::string device_name = name;
    const bool is_default =
        device_name.rfind(kDefaultPrefix, 0) == 0;
    const std::string match_description =
        is_default ? device_name.substr(sizeof(kDefaultPrefix) - 1)
                   : device_name;

    int match = -1;
    for (size_t j = 0; j < sources.size(); ++j) {
      if (!used[j] && sources[j].description == match_description) {
        match = static_cast<int>(j);
        break;
      }
    }
    if (match < 0 && !is_default && source_cursor >= 0 &&
        static_cast<size_t>(source_cursor) < sources.size() &&
        !used[source_cursor]) {
      // 描述对不上（例如 Pulse 改过描述）时按顺序兜底对齐。
      match = source_cursor;
    }
    info.id = info.name;  // 兜底：没有 Pulse 信息时用描述
    if (match >= 0) {
      info.available = sources[static_cast<size_t>(match)].available;
      info.hardware_sample_rate =
          sources[static_cast<size_t>(match)].sample_rate;
      info.hardware_channels =
          sources[static_cast<size_t>(match)].channels;
      // default: 是别名，只读取属性、不消费真实源，否则会抢走同名真实设备。
      if (!is_default) {
        info.id = sources[static_cast<size_t>(match)].name;
        used[static_cast<size_t>(match)] = true;
        source_cursor = match + 1;
      }
    }
    if (is_default) {
      // 默认源是一个别名，即使端口状态未知也可尝试使用。
      info.available = "yes";
      info.id = "default";
    }
    devices.push_back(std::move(info));
  }
  return devices;
}

bool SetRecordingDeviceByName(webrtc::AudioDeviceModule* adm,
                              const std::string& name) {
  if (adm == nullptr || name.empty()) {
    return false;
  }
  for (const AudioDeviceInfo& info : EnumerateRecordingDevices(adm)) {
    if (info.name == name) {
      const int32_t result = adm->SetRecordingDevice(
          static_cast<uint16_t>(info.index));
      RTC_LOG(LS_INFO) << "SetRecordingDevice(" << info.index << ", " << name
                       << ") result=" << result;
      return result == 0;
    }
  }
  RTC_LOG(LS_WARNING) << "SetRecordingDeviceByName: not found: " << name;
  return false;
}

}  // namespace webrtc_media
