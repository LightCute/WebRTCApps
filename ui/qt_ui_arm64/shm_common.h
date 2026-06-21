// shm_common.h — delegates to WebRTC source canonical definition
#pragma once
#include <cstdlib>
#include <string>

#include "apps/peerconnection/video_capture_shm_RGA/video_frame_shm_ctrl.h"

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
