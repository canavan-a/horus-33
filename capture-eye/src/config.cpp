#include "config.h"

#include <format>

namespace capture_eye {
namespace {

template <typename T>
void layer(T& target, const std::optional<T>& value) {
  if (value.has_value()) target = *value;
}

CaptureConfig merge_capture(CaptureConfig c, const CaptureConfigOverlay& o) {
  layer(c.device, o.device);
  layer(c.width, o.width);
  layer(c.height, o.height);
  layer(c.fourcc, o.fourcc);
  layer(c.fps, o.fps);
  layer(c.buffer_count, o.buffer_count);
  layer(c.decode_scale, o.decode_scale);
  layer(c.strict_format, o.strict_format);
  layer(c.flip_horizontal, o.flip_horizontal);
  layer(c.flip_vertical, o.flip_vertical);
  return c;
}

ModelConfig merge_model(ModelConfig c, const ModelConfigOverlay& o) {
  layer(c.variant, o.variant);
  layer(c.url, o.url);
  layer(c.sha256, o.sha256);
  layer(c.path, o.path);
  layer(c.offline, o.offline);
  layer(c.allow_unpinned, o.allow_unpinned);
  return c;
}

InferenceConfig merge_inference(InferenceConfig c, const InferenceConfigOverlay& o) {
  layer(c.input_size, o.input_size);
  layer(c.conf_threshold, o.conf_threshold);
  layer(c.intra_op_threads, o.intra_op_threads);
  layer(c.inter_op_threads, o.inter_op_threads);
  layer(c.fake, o.fake);
  return c;
}

SerialConfig merge_serial(SerialConfig c, const SerialConfigOverlay& o) {
  layer(c.port, o.port);
  layer(c.baud, o.baud);
  layer(c.enabled, o.enabled);
  layer(c.max_hz, o.max_hz);
  layer(c.lost_repeat_hz, o.lost_repeat_hz);
  layer(c.send_seq, o.send_seq);
  return c;
}

TrackingConfig merge_tracking(TrackingConfig c, const TrackingConfigOverlay& o) {
  layer(c.policy, o.policy);
  layer(c.lock_iou, o.lock_iou);
  layer(c.lost_grace, o.lost_grace);
  return c;
}

SinkConfig merge_sink(SinkConfig c, const SinkConfigOverlay& o) {
  layer(c.preview, o.preview);
  layer(c.snapshot_path, o.snapshot_path);
  layer(c.snapshot_every, o.snapshot_every);
  layer(c.rtsp_url, o.rtsp_url);
  layer(c.bitrate_kbps, o.bitrate_kbps);
  layer(c.hardware_encode, o.hardware_encode);
  layer(c.vaapi_device, o.vaapi_device);
  return c;
}

IngressConfig merge_ingress(IngressConfig c, const IngressConfigOverlay& o) {
  layer(c.socket_path, o.socket_path);
  layer(c.max_clients, o.max_clients);
  return c;
}

ClippingConfig merge_clipping(ClippingConfig c, const ClippingConfigOverlay& o) {
  layer(c.enabled, o.enabled);
  layer(c.output_dir, o.output_dir);
  layer(c.admin_socket_path, o.admin_socket_path);
  layer(c.pre_roll_seconds, o.pre_roll_seconds);
  layer(c.stop_after_ticks, o.stop_after_ticks);
  layer(c.bitrate_kbps, o.bitrate_kbps);
  layer(c.hardware_encode, o.hardware_encode);
  layer(c.vaapi_device, o.vaapi_device);
  return c;
}

} // namespace

AppConfig merge(const AppConfig& base, const ConfigOverlay& file, const ConfigOverlay& flags) {
  AppConfig result = base;
  result.capture = merge_capture(result.capture, file.capture);
  result.model = merge_model(result.model, file.model);
  result.inference = merge_inference(result.inference, file.inference);
  result.serial = merge_serial(result.serial, file.serial);
  result.tracking = merge_tracking(result.tracking, file.tracking);
  result.sink = merge_sink(result.sink, file.sink);
  result.ingress = merge_ingress(result.ingress, file.ingress);
  result.clipping = merge_clipping(result.clipping, file.clipping);

  result.capture = merge_capture(result.capture, flags.capture);
  result.model = merge_model(result.model, flags.model);
  result.inference = merge_inference(result.inference, flags.inference);
  result.serial = merge_serial(result.serial, flags.serial);
  result.tracking = merge_tracking(result.tracking, flags.tracking);
  result.sink = merge_sink(result.sink, flags.sink);
  result.ingress = merge_ingress(result.ingress, flags.ingress);
  result.clipping = merge_clipping(result.clipping, flags.clipping);
  return result;
}

Result<void> validate(const AppConfig& config) {
  if (config.capture.width <= 0 || config.capture.height <= 0) {
    return fail(ErrorCode::config_invalid,
                std::format("size: must be positive, got {}x{}", config.capture.width,
                            config.capture.height));
  }
  if (config.capture.fps <= 0) {
    return fail(ErrorCode::config_invalid,
                std::format("fps: must be positive, got {}", config.capture.fps));
  }
  if (config.capture.decode_scale != 1 && config.capture.decode_scale != 2 &&
      config.capture.decode_scale != 4) {
    return fail(ErrorCode::config_invalid, std::format("decode_scale: expected 1, 2 or 4, got {}",
                                                        config.capture.decode_scale));
  }
  if (config.inference.conf_threshold < 0.0f || config.inference.conf_threshold > 1.0f) {
    return fail(ErrorCode::config_invalid,
                std::format("conf: must be within [0, 1], got {}", config.inference.conf_threshold));
  }
  if (config.inference.intra_op_threads < 1) {
    return fail(ErrorCode::config_invalid,
                std::format("intra_threads: must be >= 1, got {}", config.inference.intra_op_threads));
  }
  if (config.inference.inter_op_threads < 1) {
    return fail(ErrorCode::config_invalid,
                std::format("inter_threads: must be >= 1, got {}", config.inference.inter_op_threads));
  }
  if (config.sink.bitrate_kbps <= 0) {
    return fail(ErrorCode::config_invalid,
                std::format("bitrate: must be positive, got {}", config.sink.bitrate_kbps));
  }
  if (config.tracking.lock_iou < 0.0f || config.tracking.lock_iou > 1.0f) {
    return fail(ErrorCode::config_invalid,
                std::format("lock_iou: must be within [0, 1], got {}", config.tracking.lock_iou));
  }
  if (config.ingress.max_clients < 1) {
    return fail(ErrorCode::config_invalid,
                std::format("ingress.max_clients: must be >= 1, got {}", config.ingress.max_clients));
  }
  if (config.clipping.enabled && config.clipping.output_dir.empty()) {
    return fail(ErrorCode::config_invalid, "clipping.output_dir: required when clipping.enabled");
  }
  if (config.clipping.pre_roll_seconds < 0.1 || config.clipping.pre_roll_seconds > 10.0) {
    return fail(ErrorCode::config_invalid,
                std::format("clipping.pre_roll_seconds: must be within [0.1, 10], got {}",
                            config.clipping.pre_roll_seconds));
  }
  if (config.clipping.stop_after_ticks < 1) {
    return fail(ErrorCode::config_invalid,
                std::format("clipping.stop_after_ticks: must be >= 1, got {}",
                            config.clipping.stop_after_ticks));
  }
  if (config.clipping.bitrate_kbps <= 0) {
    return fail(ErrorCode::config_invalid,
                std::format("clipping.bitrate_kbps: must be positive, got {}",
                            config.clipping.bitrate_kbps));
  }
  return {};
}

} // namespace capture_eye
