#include "media/audio_device.h"

#include "api/audio/create_audio_device_module.h"
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
  const int16_t count = adm->RecordingDevices();
  for (int16_t i = 0; i < count; ++i) {
    char name[webrtc::kAdmMaxDeviceNameSize] = {0};
    char guid[webrtc::kAdmMaxGuidSize] = {0};
    if (adm->RecordingDeviceName(static_cast<uint16_t>(i), name, guid) != 0) {
      continue;
    }
    AudioDeviceInfo info;
    info.index = i;
    info.name = name;  // Pulse source 名，作为稳定 id
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
