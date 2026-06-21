// shm_common.h — delegates to WebRTC source canonical definition
#pragma once
#include "apps/peerconnection/client/shm_common.h"

// Qt-local key path helpers (video local/remote use different SHM regions than WebRTC's default)
// Audio helpers (shm_audio_cap_key_path, shm_audio_playout_key_path) come from the
// WebRTC shm_common.h included above — no need to redefine.
inline std::string shm_video_local_key_path() {
    const char* dir = getenv("WEBRTC_RUNTIME_DIR");
    return std::string(dir ? dir : "/tmp/webrtc_runtime") + "/shm_video_buf_local";
}
inline std::string shm_video_remote_key_path() {
    const char* dir = getenv("WEBRTC_RUNTIME_DIR");
    return std::string(dir ? dir : "/tmp/webrtc_runtime") + "/shm_video_buf_remote";
}

inline constexpr int SHM_VIDEO_LOCAL_PROJ_ID  = 0x89;
inline constexpr int SHM_VIDEO_REMOTE_PROJ_ID = 0x8a;
