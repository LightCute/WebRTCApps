#include "media/pulse_query.h"

#include <dlfcn.h>
#include <pulse/pulseaudio.h>

#include <chrono>
#include <span>
#include <thread>

#include "rtc_base/logging.h"

namespace webrtc_media {

namespace {

constexpr auto kQueryTimeout = std::chrono::milliseconds(2000);

// libpulse 在运行时按需加载：部署机器（如云端信令服务器）可能没装
// PulseAudio，二进制不应因此无法启动。加载失败时查询返回空列表。
struct PulseApi {
  void* handle = nullptr;
  decltype(&pa_mainloop_new) mainloop_new = nullptr;
  decltype(&pa_mainloop_get_api) mainloop_get_api = nullptr;
  decltype(&pa_mainloop_iterate) mainloop_iterate = nullptr;
  decltype(&pa_mainloop_free) mainloop_free = nullptr;
  decltype(&pa_context_new) context_new = nullptr;
  decltype(&pa_context_connect) context_connect = nullptr;
  decltype(&pa_context_disconnect) context_disconnect = nullptr;
  decltype(&pa_context_unref) context_unref = nullptr;
  decltype(&pa_context_get_state) context_get_state = nullptr;
  decltype(&pa_context_set_state_callback) context_set_state_callback =
      nullptr;
  decltype(&pa_context_get_source_info_list) context_get_source_info_list =
      nullptr;
  decltype(&pa_operation_unref) operation_unref = nullptr;

  bool Load() {
    handle = dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      return false;
    }
    auto load = [this](const char* name) { return dlsym(handle, name); };
    mainloop_new =
        reinterpret_cast<decltype(mainloop_new)>(load("pa_mainloop_new"));
    mainloop_get_api = reinterpret_cast<decltype(mainloop_get_api)>(
        load("pa_mainloop_get_api"));
    mainloop_iterate = reinterpret_cast<decltype(mainloop_iterate)>(
        load("pa_mainloop_iterate"));
    mainloop_free =
        reinterpret_cast<decltype(mainloop_free)>(load("pa_mainloop_free"));
    context_new =
        reinterpret_cast<decltype(context_new)>(load("pa_context_new"));
    context_connect = reinterpret_cast<decltype(context_connect)>(
        load("pa_context_connect"));
    context_disconnect = reinterpret_cast<decltype(context_disconnect)>(
        load("pa_context_disconnect"));
    context_unref =
        reinterpret_cast<decltype(context_unref)>(load("pa_context_unref"));
    context_get_state = reinterpret_cast<decltype(context_get_state)>(
        load("pa_context_get_state"));
    context_set_state_callback =
        reinterpret_cast<decltype(context_set_state_callback)>(
            load("pa_context_set_state_callback"));
    context_get_source_info_list =
        reinterpret_cast<decltype(context_get_source_info_list)>(
            load("pa_context_get_source_info_list"));
    operation_unref = reinterpret_cast<decltype(operation_unref)>(
        load("pa_operation_unref"));
    return mainloop_new != nullptr && mainloop_get_api != nullptr &&
           mainloop_iterate != nullptr && mainloop_free != nullptr &&
           context_new != nullptr && context_connect != nullptr &&
           context_disconnect != nullptr && context_unref != nullptr &&
           context_get_state != nullptr &&
           context_set_state_callback != nullptr &&
           context_get_source_info_list != nullptr &&
           operation_unref != nullptr;
  }
};

PulseApi& GetPulseApi() {
  static PulseApi api;
  static const bool loaded = api.Load();
  (void)loaded;
  return api;
}

struct QueryState {
  bool ready = false;
  bool failed = false;
  bool done = false;
  std::vector<PulseSourceInfo> sources;
};

void ContextStateCallback(pa_context* context, void* userdata) {
  auto* state = static_cast<QueryState*>(userdata);
  switch (GetPulseApi().context_get_state(context)) {
    case PA_CONTEXT_READY:
      state->ready = true;
      break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
      state->failed = true;
      break;
    default:
      break;
  }
}

void SourceInfoCallback(pa_context* /*context*/, const pa_source_info* info,
                        int eol, void* userdata) {
  auto* state = static_cast<QueryState*>(userdata);
  if (eol > 0) {
    state->done = true;
    return;
  }
  if (info == nullptr) {
    return;
  }
  // 过滤掉输出的 monitor 回环源（与 ADM 的枚举口径一致）。
  if (info->monitor_of_sink != PA_INVALID_INDEX) {
    return;
  }
  PulseSourceInfo source;
  source.name = info->name != nullptr ? info->name : "";
  source.description = info->description != nullptr ? info->description : "";
  source.sample_rate = static_cast<int>(info->sample_spec.rate);
  source.channels = static_cast<int>(info->sample_spec.channels);

  bool any_yes = false;
  bool any_no = false;
  const std::span<pa_source_port_info* const> ports(info->ports,
                                                    info->n_ports);
  for (const pa_source_port_info* port : ports) {
    if (port == nullptr) {
      continue;
    }
    if (port->available == PA_PORT_AVAILABLE_YES) {
      any_yes = true;
    } else if (port->available == PA_PORT_AVAILABLE_NO) {
      any_no = true;
    }
  }
  source.available = any_yes ? "yes" : (any_no ? "no" : "unknown");
  state->sources.push_back(std::move(source));
}

}  // namespace

std::vector<PulseSourceInfo> QueryPulseSources() {
  PulseApi& api = GetPulseApi();
  if (api.mainloop_new == nullptr) {
    return {};  // 未安装 libpulse：静默降级
  }

  pa_mainloop* mainloop = api.mainloop_new();
  if (mainloop == nullptr) {
    return {};
  }
  pa_mainloop_api* mainloop_api = api.mainloop_get_api(mainloop);
  pa_context* context = api.context_new(mainloop_api, "webrtc-audio-query");
  if (context == nullptr) {
    api.mainloop_free(mainloop);
    return {};
  }

  QueryState state;
  api.context_set_state_callback(context, ContextStateCallback, &state);
  if (api.context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN,
                          nullptr) < 0) {
    api.context_unref(context);
    api.mainloop_free(mainloop);
    return {};
  }

  const auto deadline = std::chrono::steady_clock::now() + kQueryTimeout;
  while (!state.ready && !state.failed &&
         std::chrono::steady_clock::now() < deadline) {
    if (api.mainloop_iterate(mainloop, 0, nullptr) < 0) {
      state.failed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  if (state.ready && !state.failed) {
    pa_operation* operation = api.context_get_source_info_list(
        context, SourceInfoCallback, &state);
    while (!state.done && std::chrono::steady_clock::now() < deadline) {
      if (api.mainloop_iterate(mainloop, 0, nullptr) < 0) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (operation != nullptr) {
      api.operation_unref(operation);
    }
  }

  if (state.failed) {
    RTC_LOG(LS_WARNING) << "QueryPulseSources: PulseAudio context failed";
  } else if (!state.done) {
    RTC_LOG(LS_WARNING) << "QueryPulseSources: timeout";
  }

  api.context_disconnect(context);
  api.context_unref(context);
  api.mainloop_free(mainloop);
  return state.sources;
}

}  // namespace webrtc_media
