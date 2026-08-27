#include "args.h"

#include <charconv>
#include <format>

namespace capture_eye {
namespace {

[[nodiscard]] Result<int> parse_int(std::string_view flag, std::string_view text) {
  int value = 0;
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, value);
  if (ec != std::errc{} || ptr != end) {
    return fail(ErrorCode::config_invalid, std::format("{}: not an integer: '{}'", flag, text));
  }
  return value;
}

[[nodiscard]] Result<float> parse_float(std::string_view flag, std::string_view text) {
  float value = 0;
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, value);
  if (ec != std::errc{} || ptr != end) {
    return fail(ErrorCode::config_invalid, std::format("{}: not a number: '{}'", flag, text));
  }
  return value;
}

[[nodiscard]] Result<double> parse_double(std::string_view flag, std::string_view text) {
  double value = 0;
  const auto* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, value);
  if (ec != std::errc{} || ptr != end) {
    return fail(ErrorCode::config_invalid, std::format("{}: not a number: '{}'", flag, text));
  }
  return value;
}

[[nodiscard]] Result<TargetPolicy> parse_policy(std::string_view text) {
  if (text == "sticky") return TargetPolicy::sticky_largest;
  if (text == "largest") return TargetPolicy::largest_area;
  if (text == "confident") return TargetPolicy::most_confident;
  if (text == "closest") return TargetPolicy::nearest_center;
  return fail(ErrorCode::config_invalid,
              std::format("--policy: expected sticky|largest|confident|closest, got '{}'", text));
}

[[nodiscard]] Result<InferenceBackend> parse_backend(std::string_view text) {
  if (text == "onnx") return InferenceBackend::onnx;
  if (text == "openvino") return InferenceBackend::openvino;
  return fail(ErrorCode::config_invalid,
              std::format("--backend: expected onnx|openvino, got '{}'", text));
}

} // namespace

Result<std::pair<int, int>> parse_size(std::string_view text) {
  const auto x = text.find('x');
  if (x == std::string_view::npos) {
    return fail(ErrorCode::config_invalid, std::format("--size: expected WxH, got '{}'", text));
  }
  const auto width = parse_int("--size", text.substr(0, x));
  if (!width) return std::unexpected(width.error());
  const auto height = parse_int("--size", text.substr(x + 1));
  if (!height) return std::unexpected(height.error());
  if (*width <= 0 || *height <= 0) {
    return fail(ErrorCode::config_invalid, std::format("--size: must be positive, got '{}'", text));
  }
  return std::pair{*width, *height};
}

std::string usage() {
  return R"(capture-eye — person tracking for horus-33

Usage: capture-eye [options]

Config:
  --config PATH         load defaults from a JSON file (see docs/config.md);
                         precedence is: built-in defaults < --config < flags

Camera:
  --device PATH        video device (default /dev/video0)
  --size WxH           capture resolution (default 1280x720)
  --fourcc CODE        pixel format, 4 chars (default MJPG)
  --fps N              frame rate (default 60)
  --decode-scale N     JPEG decode downscale: 1, 2 or 4 (default 1)
  --loose-format       accept whatever the driver grants instead of failing
  --flip-h             mirror the frame horizontally (mount correction)
  --flip-v             flip the frame vertically (mount correction)

Model:
  --model PATH         use this .onnx file; never fetch
  --model-variant NAME model|model_fp16|model_int8|model_quantized (default model)
  --model-url URL      override the download URL
  --model-sha HEX      override the expected sha256
  --allow-unpinned     permit a download with no known hash
  --offline            never use the network; require a warm cache

Inference:
  --backend NAME       onnx|openvino (default onnx); openvino needs a build
                        configured with -DCAPTURE_EYE_OPENVINO=ON and an
                        explicit --model to an IR .xml
  --conf F             host confidence threshold (default 0.35)
  --intra-threads N    ONNX Runtime intra-op threads (default 2)
  --fake-detector      synthetic detections; no model, for testing the pipeline

Tracking:
  --policy NAME        sticky|largest|confident|closest (default sticky)

Serial:
  --serial PATH        device port (default /dev/ttyACM0)
  --no-serial          print track messages to stdout instead
  --track-seq          include seq so the device acks (debugging only)

Control relay:
  --control-socket PATH   listen here for other processes to send device
                           commands (describe/set/ping) through capture-eye;
                           off unless given, since only one process may hold
                           the serial port
  --control-max-clients N max simultaneous relay clients (default 8)

Output:
  --preview            show an annotated preview window
  --snapshot PATH      periodically write the annotated frame to a JPEG
  --rtsp URL           publish H.264 to an RTSP server
  --bitrate KBPS       encoder bitrate (default 4000)
  --hw-encode          encode with VAAPI instead of libx264 (off by default)
  --no-hw-encode       force software encode (libx264)
  --vaapi-device PATH  render node for hardware encode (default /dev/dri/renderD128)

Clipping:
  --clip                  record a clip whenever a person is detected; needs --clip-dir
  --clip-dir PATH         directory finished clips are written to
  --clip-admin-socket PATH  listen here for live enable/disable + status
                            (host-only, off unless given)
  --clip-preroll SECONDS  seconds of video to keep before the detection instant
                           (default 1.0)
  --clip-stop-ticks N     consecutive no-person inference ticks before a clip
                           stops (default 40)

Modes:
  --check-config       load and validate the config, then exit; opens no
                        camera, serial port or model, so it is safe to run
                        against a live host (exit 0 = valid, 2 = not)
  --list-formats       print what the camera supports, then exit
  --dump-model-io      print the model's tensor shapes, then exit
  --detect-image PATH  detect in one image file and print the boxes, then exit
  -h, --help           this text
)";
}

Result<Invocation> parse_args(std::span<const std::string_view> args) {
  Invocation inv;
  ConfigOverlay& o = inv.overlay;

  // Returns the value belonging to a flag, or an error if it is missing.
  std::size_t i = 0;
  const auto value_for = [&](std::string_view flag) -> Result<std::string_view> {
    if (i + 1 >= args.size()) {
      return fail(ErrorCode::config_invalid, std::format("{}: expected a value", flag));
    }
    return args[++i];
  };

  for (; i < args.size(); ++i) {
    const std::string_view arg = args[i];

    if (arg == "-h" || arg == "--help") {
      inv.command = Command::help;
      return inv;
    }
    if (arg == "--list-formats") {
      inv.command = Command::list_formats;
      continue;
    }
    if (arg == "--check-config") {
      inv.command = Command::check_config;
      continue;
    }
    if (arg == "--dump-model-io") {
      inv.command = Command::dump_model_io;
      continue;
    }
    if (arg == "--detect-image") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      inv.command = Command::detect_image;
      inv.image = *v;
      continue;
    }

    if (arg == "--config") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      inv.config_file = *v;

    } else if (arg == "--device") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      o.capture.device = *v;
    } else if (arg == "--size") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto size = parse_size(*v);
      if (!size) return std::unexpected(size.error());
      o.capture.width = size->first;
      o.capture.height = size->second;
    } else if (arg == "--fourcc") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      if (v->size() != 4) {
        return fail(ErrorCode::config_invalid,
                    std::format("--fourcc: expected 4 characters, got '{}'", *v));
      }
      const char code[5] = {(*v)[0], (*v)[1], (*v)[2], (*v)[3], '\0'};
      o.capture.fourcc = fourcc_of(code);
    } else if (arg == "--fps") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto fps = parse_int(arg, *v);
      if (!fps) return std::unexpected(fps.error());
      o.capture.fps = *fps;
    } else if (arg == "--decode-scale") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto scale = parse_int(arg, *v);
      if (!scale) return std::unexpected(scale.error());
      o.capture.decode_scale = *scale;
    } else if (arg == "--loose-format") {
      o.capture.strict_format = false;
    } else if (arg == "--flip-h") {
      o.capture.flip_horizontal = true;
    } else if (arg == "--flip-v") {
      o.capture.flip_vertical = true;

    } else if (arg == "--model") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      o.model.path = *v;
    } else if (arg == "--model-variant") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      o.model.variant = std::string{*v};
    } else if (arg == "--model-url") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      o.model.url = std::string{*v};
    } else if (arg == "--model-sha") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      o.model.sha256 = std::string{*v};
    } else if (arg == "--allow-unpinned") {
      o.model.allow_unpinned = true;
    } else if (arg == "--offline") {
      o.model.offline = true;

    } else if (arg == "--backend") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto backend = parse_backend(*v);
      if (!backend) return std::unexpected(backend.error());
      o.inference.backend = *backend;
    } else if (arg == "--conf") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto conf = parse_float(arg, *v);
      if (!conf) return std::unexpected(conf.error());
      o.inference.conf_threshold = *conf;
    } else if (arg == "--intra-threads") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto threads = parse_int(arg, *v);
      if (!threads) return std::unexpected(threads.error());
      o.inference.intra_op_threads = *threads;
    } else if (arg == "--fake-detector") {
      o.inference.fake = true;

    } else if (arg == "--policy") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto policy = parse_policy(*v);
      if (!policy) return std::unexpected(policy.error());
      o.tracking.policy = *policy;

    } else if (arg == "--serial") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      o.serial.port = *v;
    } else if (arg == "--no-serial") {
      o.serial.enabled = false;
    } else if (arg == "--track-seq") {
      o.serial.send_seq = true;

    } else if (arg == "--control-socket") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      o.ingress.socket_path = *v;
    } else if (arg == "--control-max-clients") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto n = parse_int(arg, *v);
      if (!n) return std::unexpected(n.error());
      o.ingress.max_clients = *n;

    } else if (arg == "--preview") {
      o.sink.preview = true;
    } else if (arg == "--snapshot") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      o.sink.snapshot_path = *v;
    } else if (arg == "--rtsp") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      o.sink.rtsp_url = std::string{*v};
    } else if (arg == "--bitrate") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto kbps = parse_int(arg, *v);
      if (!kbps) return std::unexpected(kbps.error());
      o.sink.bitrate_kbps = *kbps;
    } else if (arg == "--hw-encode") {
      o.sink.hardware_encode = true;
    } else if (arg == "--no-hw-encode") {
      o.sink.hardware_encode = false;
    } else if (arg == "--vaapi-device") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      o.sink.vaapi_device = *v;

    } else if (arg == "--clip") {
      o.clipping.enabled = true;
    } else if (arg == "--clip-dir") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      o.clipping.output_dir = *v;
    } else if (arg == "--clip-admin-socket") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      o.clipping.admin_socket_path = *v;
    } else if (arg == "--clip-preroll") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto seconds = parse_double(arg, *v);
      if (!seconds) return std::unexpected(seconds.error());
      o.clipping.pre_roll_seconds = *seconds;
    } else if (arg == "--clip-stop-ticks") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto n = parse_int(arg, *v);
      if (!n) return std::unexpected(n.error());
      o.clipping.stop_after_ticks = *n;

    } else {
      return fail(ErrorCode::config_invalid, std::format("unknown flag: '{}'", arg));
    }
  }

  // Fully resolved for the common case (no --config file). When a config file
  // is also given, the caller redoes this with merge(AppConfig{}, file, o) —
  // see args.h.
  inv.config = merge(AppConfig{}, {}, o);
  if (const auto ok = validate(inv.config); !ok) return std::unexpected(ok.error());
  return inv;
}

} // namespace capture_eye
