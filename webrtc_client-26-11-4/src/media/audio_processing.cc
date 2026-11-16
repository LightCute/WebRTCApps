#include "media/audio_processing.h"

#include <cerrno>
#include <climits>
#include <cstdlib>

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment.h"
#include "rtc_base/logging.h"

namespace webrtc_media {

namespace {

using Config = webrtc::AudioProcessing::Config;

// 包一层内置 builder，把构建出来的 APM 存下来，供运行时 ApplyConfig 使用。
class CapturingApmBuilder : public webrtc::AudioProcessingBuilderInterface {
 public:
  explicit CapturingApmBuilder(
      webrtc::scoped_refptr<webrtc::AudioProcessing>* out_apm)
      : out_apm_(out_apm) {}

  webrtc::scoped_refptr<webrtc::AudioProcessing> Build(
      const webrtc::Environment& env) override {
    webrtc::scoped_refptr<webrtc::AudioProcessing> apm = builder_.Build(env);
    if (apm && out_apm_ != nullptr) {
      *out_apm_ = apm;
    }
    return apm;
  }

 private:
  webrtc::BuiltinAudioProcessingBuilder builder_;
  webrtc::scoped_refptr<webrtc::AudioProcessing>* out_apm_;
};

bool ParseBool(const std::string& value, bool* out) {
  if (value == "on" || value == "true" || value == "1") {
    *out = true;
    return true;
  }
  if (value == "off" || value == "false" || value == "0") {
    *out = false;
    return true;
  }
  return false;
}

bool ParseLong(const std::string& value, long* out) {
  if (value.empty()) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str() || *end != '\0') {
    return false;
  }
  *out = parsed;
  return true;
}

bool ParseInt(const std::string& value, int* out) {
  long parsed = 0;
  if (!ParseLong(value, &parsed) || parsed < INT_MIN || parsed > INT_MAX) {
    return false;
  }
  *out = static_cast<int>(parsed);
  return true;
}

bool ParseFloat(const std::string& value, float* out) {
  if (value.empty()) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const float parsed = std::strtof(value.c_str(), &end);
  if (errno != 0 || end == value.c_str() || *end != '\0') {
    return false;
  }
  *out = parsed;
  return true;
}

bool ParseIntInRange(const std::string& value, int lo, int hi, int* out) {
  int parsed = 0;
  if (!ParseInt(value, &parsed) || parsed < lo || parsed > hi) {
    return false;
  }
  *out = parsed;
  return true;
}

bool ParseNsLevel(const std::string& value,
                  Config::NoiseSuppression::Level* out) {
  if (value == "low") {
    *out = Config::NoiseSuppression::kLow;
  } else if (value == "moderate") {
    *out = Config::NoiseSuppression::kModerate;
  } else if (value == "high") {
    *out = Config::NoiseSuppression::kHigh;
  } else if (value == "very_high") {
    *out = Config::NoiseSuppression::kVeryHigh;
  } else {
    return false;
  }
  return true;
}

bool ParseAgcMode(const std::string& value, Config::GainController1::Mode* out) {
  if (value == "adaptive_analog") {
    *out = Config::GainController1::kAdaptiveAnalog;
  } else if (value == "adaptive_digital") {
    *out = Config::GainController1::kAdaptiveDigital;
  } else if (value == "fixed_digital") {
    *out = Config::GainController1::kFixedDigital;
  } else {
    return false;
  }
  return true;
}

bool ParseClippingPredictorMode(
    const std::string& value,
    Config::GainController1::AnalogGainController::ClippingPredictor::Mode* out) {
  using Mode =
      Config::GainController1::AnalogGainController::ClippingPredictor::Mode;
  if (value == "clipping_event_prediction") {
    *out = Mode::kClippingEventPrediction;
  } else if (value == "adaptive_step_clipping_peak_prediction") {
    *out = Mode::kAdaptiveStepClippingPeakPrediction;
  } else if (value == "fixed_step_clipping_peak_prediction") {
    *out = Mode::kFixedStepClippingPeakPrediction;
  } else {
    return false;
  }
  return true;
}

void SetError(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
}

std::string CanonicalKey(const std::string& key) {
  if (key == "aec") return "echo_canceller.enabled";
  if (key == "ns") return "noise_suppression.enabled";
  if (key == "ns_level") return "noise_suppression.level";
  if (key == "agc") return "gain_controller1.enabled";
  if (key == "agc_mode") return "gain_controller1.mode";
  if (key == "hpf") return "high_pass_filter.enabled";
  if (key == "transient") return "transient_suppression.enabled";
  if (key == "agc2") return "gain_controller2.enabled";
  return key;
}

}  // namespace

void ConfigureAudioProcessing(
    webrtc::PeerConnectionFactoryDependencies& deps,
    webrtc::scoped_refptr<webrtc::AudioProcessing>* out_apm) {
  deps.audio_processing_builder =
      std::make_unique<CapturingApmBuilder>(out_apm);
}

webrtc::AudioProcessing::Config DefaultAudioProcessingConfig() {
  Config config;
  config.echo_canceller.enabled = false;
  config.noise_suppression.enabled = true;
  config.noise_suppression.level = Config::NoiseSuppression::kHigh;
  config.gain_controller1.enabled = true;
  config.gain_controller1.mode = Config::GainController1::kAdaptiveAnalog;
  config.high_pass_filter.enabled = true;
  config.transient_suppression.enabled = false;
  config.gain_controller2.enabled = false;
  return config;
}

bool SetAudioProcessingKey(Config* config, const std::string& raw_key,
                           const std::string& value, std::string* error) {
  if (config == nullptr) {
    SetError(error, "config is null");
    return false;
  }
  const std::string key = CanonicalKey(raw_key);
  bool handled = false;
  bool value_ok = true;

  if (key == "echo_canceller.enabled") {
    value_ok = ParseBool(value, &config->echo_canceller.enabled);
    handled = true;
  } else if (key == "echo_canceller.export_linear_aec_output") {
    value_ok = ParseBool(value, &config->echo_canceller.export_linear_aec_output);
    handled = true;
  } else if (key == "echo_canceller.enforce_high_pass_filtering") {
    value_ok =
        ParseBool(value, &config->echo_canceller.enforce_high_pass_filtering);
    handled = true;
  } else if (key == "noise_suppression.enabled") {
    value_ok = ParseBool(value, &config->noise_suppression.enabled);
    handled = true;
  } else if (key == "noise_suppression.level") {
    value_ok = ParseNsLevel(value, &config->noise_suppression.level);
    handled = true;
  } else if (key ==
             "noise_suppression.analyze_linear_aec_output_when_available") {
    value_ok = ParseBool(
        value, &config->noise_suppression.analyze_linear_aec_output_when_available);
    handled = true;
  } else if (key == "transient_suppression.enabled") {
    value_ok = ParseBool(value, &config->transient_suppression.enabled);
    handled = true;
  } else if (key == "high_pass_filter.enabled") {
    value_ok = ParseBool(value, &config->high_pass_filter.enabled);
    handled = true;
  } else if (key == "high_pass_filter.apply_in_full_band") {
    value_ok = ParseBool(value, &config->high_pass_filter.apply_in_full_band);
    handled = true;
  } else if (key == "gain_controller1.enabled") {
    value_ok = ParseBool(value, &config->gain_controller1.enabled);
    handled = true;
  } else if (key == "gain_controller1.mode") {
    value_ok = ParseAgcMode(value, &config->gain_controller1.mode);
    handled = true;
  } else if (key == "gain_controller1.target_level_dbfs") {
    value_ok = ParseIntInRange(value, 0, 31,
                               &config->gain_controller1.target_level_dbfs);
    handled = true;
  } else if (key == "gain_controller1.compression_gain_db") {
    value_ok = ParseIntInRange(value, 0, 90,
                               &config->gain_controller1.compression_gain_db);
    handled = true;
  } else if (key == "gain_controller1.enable_limiter") {
    value_ok = ParseBool(value, &config->gain_controller1.enable_limiter);
    handled = true;
  } else if (key == "gain_controller1.analog_gain_controller.enabled") {
    value_ok = ParseBool(
        value, &config->gain_controller1.analog_gain_controller.enabled);
    handled = true;
  } else if (key ==
             "gain_controller1.analog_gain_controller.startup_min_volume") {
    value_ok = ParseInt(
        value,
        &config->gain_controller1.analog_gain_controller.startup_min_volume);
    handled = true;
  } else if (key ==
             "gain_controller1.analog_gain_controller.clipped_level_min") {
    value_ok = ParseInt(
        value,
        &config->gain_controller1.analog_gain_controller.clipped_level_min);
    handled = true;
  } else if (key ==
             "gain_controller1.analog_gain_controller.enable_digital_adaptive") {
    value_ok = ParseBool(
        value,
        &config->gain_controller1.analog_gain_controller.enable_digital_adaptive);
    handled = true;
  } else if (key ==
             "gain_controller1.analog_gain_controller.clipped_level_step") {
    value_ok = ParseIntInRange(
        value, 1, 255,
        &config->gain_controller1.analog_gain_controller.clipped_level_step);
    handled = true;
  } else if (key ==
             "gain_controller1.analog_gain_controller.clipped_ratio_threshold") {
    float parsed = 0.0f;
    value_ok = ParseFloat(value, &parsed) && parsed > 0.0f && parsed < 1.0f;
    if (value_ok) {
      config->gain_controller1.analog_gain_controller
          .clipped_ratio_threshold = parsed;
    }
    handled = true;
  } else if (key ==
             "gain_controller1.analog_gain_controller.clipped_wait_frames") {
    int parsed = 0;
    value_ok = ParseInt(value, &parsed) && parsed > 0;
    if (value_ok) {
      config->gain_controller1.analog_gain_controller.clipped_wait_frames =
          parsed;
    }
    handled = true;
  } else if (key ==
             "gain_controller1.analog_gain_controller.clipping_predictor.enabled") {
    value_ok = ParseBool(
        value,
        &config->gain_controller1.analog_gain_controller.clipping_predictor
             .enabled);
    handled = true;
  } else if (key ==
             "gain_controller1.analog_gain_controller.clipping_predictor.mode") {
    value_ok = ParseClippingPredictorMode(
        value,
        &config->gain_controller1.analog_gain_controller.clipping_predictor
             .mode);
    handled = true;
  } else if (key ==
             "gain_controller1.analog_gain_controller.clipping_predictor.window_length") {
    int parsed = 0;
    value_ok = ParseInt(value, &parsed) && parsed > 0;
    if (value_ok) {
      config->gain_controller1.analog_gain_controller.clipping_predictor
          .window_length = parsed;
    }
    handled = true;
  } else if (key ==
             "gain_controller1.analog_gain_controller.clipping_predictor.reference_window_length") {
    int parsed = 0;
    value_ok = ParseInt(value, &parsed) && parsed > 0;
    if (value_ok) {
      config->gain_controller1.analog_gain_controller.clipping_predictor
          .reference_window_length = parsed;
    }
    handled = true;
  } else if (key ==
             "gain_controller1.analog_gain_controller.clipping_predictor.reference_window_delay") {
    int parsed = 0;
    value_ok = ParseInt(value, &parsed) && parsed >= 0;
    if (value_ok) {
      config->gain_controller1.analog_gain_controller.clipping_predictor
          .reference_window_delay = parsed;
    }
    handled = true;
  } else if (key ==
             "gain_controller1.analog_gain_controller.clipping_predictor.clipping_threshold") {
    value_ok = ParseFloat(
        value,
        &config->gain_controller1.analog_gain_controller.clipping_predictor
             .clipping_threshold);
    handled = true;
  } else if (key ==
             "gain_controller1.analog_gain_controller.clipping_predictor.crest_factor_margin") {
    value_ok = ParseFloat(
        value,
        &config->gain_controller1.analog_gain_controller.clipping_predictor
             .crest_factor_margin);
    handled = true;
  } else if (key ==
             "gain_controller1.analog_gain_controller.clipping_predictor.use_predicted_step") {
    value_ok = ParseBool(
        value,
        &config->gain_controller1.analog_gain_controller.clipping_predictor
             .use_predicted_step);
    handled = true;
  } else if (key == "gain_controller2.enabled") {
    value_ok = ParseBool(value, &config->gain_controller2.enabled);
    handled = true;
  } else if (key == "gain_controller2.input_volume_controller.enabled") {
    value_ok = ParseBool(
        value, &config->gain_controller2.input_volume_controller.enabled);
    handled = true;
  } else if (key == "gain_controller2.adaptive_digital.enabled") {
    value_ok = ParseBool(
        value, &config->gain_controller2.adaptive_digital.enabled);
    handled = true;
  } else if (key == "gain_controller2.adaptive_digital.headroom_db") {
    value_ok = ParseFloat(
        value, &config->gain_controller2.adaptive_digital.headroom_db);
    handled = true;
  } else if (key == "gain_controller2.adaptive_digital.max_gain_db") {
    value_ok = ParseFloat(
        value, &config->gain_controller2.adaptive_digital.max_gain_db);
    handled = true;
  } else if (key == "gain_controller2.adaptive_digital.initial_gain_db") {
    value_ok = ParseFloat(
        value, &config->gain_controller2.adaptive_digital.initial_gain_db);
    handled = true;
  } else if (key ==
             "gain_controller2.adaptive_digital.max_gain_change_db_per_second") {
    value_ok = ParseFloat(
        value,
        &config->gain_controller2.adaptive_digital.max_gain_change_db_per_second);
    handled = true;
  } else if (key ==
             "gain_controller2.adaptive_digital.max_output_noise_level_dbfs") {
    value_ok = ParseFloat(
        value,
        &config->gain_controller2.adaptive_digital.max_output_noise_level_dbfs);
    handled = true;
  } else if (key == "gain_controller2.fixed_digital.gain_db") {
    float parsed = 0.0f;
    value_ok = ParseFloat(value, &parsed) && parsed >= 0.0f && parsed <= 90.0f;
    if (value_ok) {
      config->gain_controller2.fixed_digital.gain_db = parsed;
    }
    handled = true;
  } else if (key == "pre_amplifier.enabled") {
    value_ok = ParseBool(value, &config->pre_amplifier.enabled);
    handled = true;
  } else if (key == "pre_amplifier.fixed_gain_factor") {
    value_ok = ParseFloat(value, &config->pre_amplifier.fixed_gain_factor);
    handled = true;
  } else if (key == "capture_level_adjustment.enabled") {
    value_ok = ParseBool(value, &config->capture_level_adjustment.enabled);
    handled = true;
  } else if (key == "capture_level_adjustment.pre_gain_factor") {
    value_ok =
        ParseFloat(value, &config->capture_level_adjustment.pre_gain_factor);
    handled = true;
  } else if (key == "capture_level_adjustment.post_gain_factor") {
    value_ok =
        ParseFloat(value, &config->capture_level_adjustment.post_gain_factor);
    handled = true;
  } else if (key ==
             "capture_level_adjustment.analog_mic_gain_emulation.enabled") {
    value_ok = ParseBool(
        value,
        &config->capture_level_adjustment.analog_mic_gain_emulation.enabled);
    handled = true;
  } else if (key ==
             "capture_level_adjustment.analog_mic_gain_emulation.initial_level") {
    value_ok = ParseIntInRange(
        value, 0, 255,
        &config->capture_level_adjustment.analog_mic_gain_emulation
             .initial_level);
    handled = true;
  }

  if (!handled) {
    SetError(error, "unknown audio config key: " + raw_key);
    return false;
  }
  if (!value_ok) {
    SetError(error, "invalid value for " + raw_key + ": " + value);
    return false;
  }

  // 冲突自动裁决：以「本次设置的 key」为准，决定谁关掉谁。
  if (key == "gain_controller2.enabled" && config->gain_controller2.enabled) {
    config->gain_controller1.enabled = false;
  }
  if (key == "gain_controller1.enabled" && config->gain_controller1.enabled) {
    config->gain_controller2.enabled = false;
  }
  if (key == "capture_level_adjustment.enabled" &&
      config->capture_level_adjustment.enabled) {
    config->pre_amplifier.enabled = false;
  }
  if (key == "pre_amplifier.enabled" && config->pre_amplifier.enabled) {
    config->capture_level_adjustment.enabled = false;
  }
  // 兜底：若外部状态本身冲突，AGC2 / capture_level_adjustment 优先。
  if (config->gain_controller2.enabled) {
    config->gain_controller1.enabled = false;
  }
  if (config->capture_level_adjustment.enabled) {
    config->pre_amplifier.enabled = false;
  }
  // AEC 要求强制高通时，HPF 必须为开。
  if (config->echo_canceller.enabled &&
      config->echo_canceller.enforce_high_pass_filtering) {
    config->high_pass_filter.enabled = true;
  }
  return true;
}

bool SetAudioSourceOption(AudioSourceOptions* options, const std::string& key,
                          const std::string& value, std::string* error) {
  if (options == nullptr) {
    SetError(error, "options is null");
    return false;
  }
  bool handled = true;
  bool value_ok = true;
  if (key == "source.stereo_swapping") {
    value_ok = ParseBool(value, &options->stereo_swapping);
  } else if (key == "source.jitter_max_packets") {
    value_ok = ParseIntInRange(value, 0, 10000,
                               &options->jitter_max_packets);
  } else if (key == "source.jitter_fast_accelerate") {
    value_ok = ParseBool(value, &options->jitter_fast_accelerate);
  } else if (key == "source.jitter_min_delay_ms") {
    value_ok = ParseIntInRange(value, 0, 10000,
                               &options->jitter_min_delay_ms);
  } else if (key == "source.init_recording_on_send") {
    value_ok = ParseBool(value, &options->init_recording_on_send);
  } else {
    handled = false;
  }
  if (!handled) {
    SetError(error, "unknown audio source key: " + key);
    return false;
  }
  if (!value_ok) {
    SetError(error, "invalid value for " + key + ": " + value);
    return false;
  }
  return true;
}

bool ApplyAudioProcessingConfig(webrtc::AudioProcessing* apm,
                                const Config& config) {
  if (apm == nullptr) {
    return false;
  }
  apm->ApplyConfig(config);
  // 这两个字段在 APM 初始化后必须走 RuntimeSetting，ApplyConfig 不保证生效。
  apm->SetRuntimeSetting(
      webrtc::AudioProcessing::RuntimeSetting::CreateCompressionGainDb(
          config.gain_controller1.compression_gain_db));
  apm->SetRuntimeSetting(
      webrtc::AudioProcessing::RuntimeSetting::CreateCaptureFixedPostGain(
          config.gain_controller2.fixed_digital.gain_db));
  RTC_LOG(LS_INFO) << "ApplyAudioProcessingConfig aec="
                   << config.echo_canceller.enabled << " ns="
                   << config.noise_suppression.enabled << " agc="
                   << config.gain_controller1.enabled << " agc2="
                   << config.gain_controller2.enabled;
  return true;
}

AudioProcessingState StateFromConfig(const Config& config) {
  AudioProcessingState state;
  state.aec = config.echo_canceller.enabled;
  state.ns = config.noise_suppression.enabled;
  state.agc = config.gain_controller1.enabled || config.gain_controller2.enabled;
  return state;
}

bool ApplyAudioProcessing(webrtc::AudioProcessing* apm,
                          const AudioProcessingState& state) {
  Config config = DefaultAudioProcessingConfig();
  config.echo_canceller.enabled = state.aec;
  config.noise_suppression.enabled = state.ns;
  config.gain_controller1.enabled = state.agc;
  config.gain_controller2.enabled = false;
  return ApplyAudioProcessingConfig(apm, config);
}

}  // namespace webrtc_media
