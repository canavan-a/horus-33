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

[[nodiscard]] Result<TargetPolicy> parse_policy(std::string_view text) {
  if (text == "sticky") return TargetPolicy::sticky_largest;
  if (text == "largest") return TargetPolicy::largest_area;
  if (text == "confident") return TargetPolicy::most_confident;
  if (text == "closest") return TargetPolicy::nearest_center;
  return fail(ErrorCode::config_invalid,
              std::format("--policy: expected sticky|largest|confident|closest, got '{}'", text));
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

Camera:
  --device PATH        video device (default /dev/video0)
  --size WxH           capture resolution (default 1280x720)
  --fourcc CODE        pixel format, 4 chars (default MJPG)
  --fps N              frame rate (default 60)
  --decode-scale N     JPEG decode downscale: 1, 2 or 4 (default 1)
  --loose-format       accept whatever the driver grants instead of failing

Model:
  --model PATH         use this .onnx file; never fetch
  --model-variant NAME model|model_fp16|model_int8|model_quantized (default model)
  --model-url URL      override the download URL
  --model-sha HEX      override the expected sha256
  --allow-unpinned     permit a download with no known hash
  --offline            never use the network; require a warm cache

Inference:
  --conf F             host confidence threshold (default 0.35)
  --intra-threads N    ONNX Runtime intra-op threads (default 2)
  --fake-detector      synthetic detections; no model, for testing the pipeline

Tracking:
  --policy NAME        sticky|largest|confident|closest (default sticky)

Serial:
  --serial PATH        device port (default /dev/ttyACM0)
  --no-serial          print track messages to stdout instead
  --track-seq          include seq so the device acks (debugging only)

Output:
  --preview            show an annotated preview window
  --snapshot PATH      periodically write the annotated frame to a JPEG
  --rtsp URL           publish H.264 to an RTSP server
  --bitrate KBPS       encoder bitrate (default 4000)
  --no-hw-encode       use libx264 instead of VAAPI
  --vaapi-device PATH  render node for hardware encode (default /dev/dri/renderD128)

Modes:
  --list-formats       print what the camera supports, then exit
  --dump-model-io      print the model's tensor shapes, then exit
  --detect-image PATH  detect in one image file and print the boxes, then exit
  -h, --help           this text
)";
}

Result<Invocation> parse_args(std::span<const std::string_view> args) {
  Invocation inv;

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

    if (arg == "--device") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      inv.config.capture.device = *v;
    } else if (arg == "--size") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto size = parse_size(*v);
      if (!size) return std::unexpected(size.error());
      inv.config.capture.width = size->first;
      inv.config.capture.height = size->second;
    } else if (arg == "--fourcc") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      if (v->size() != 4) {
        return fail(ErrorCode::config_invalid,
                    std::format("--fourcc: expected 4 characters, got '{}'", *v));
      }
      const char code[5] = {(*v)[0], (*v)[1], (*v)[2], (*v)[3], '\0'};
      inv.config.capture.fourcc = fourcc_of(code);
    } else if (arg == "--fps") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto fps = parse_int(arg, *v);
      if (!fps) return std::unexpected(fps.error());
      if (*fps <= 0) return fail(ErrorCode::config_invalid, "--fps: must be positive");
      inv.config.capture.fps = *fps;
    } else if (arg == "--decode-scale") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto scale = parse_int(arg, *v);
      if (!scale) return std::unexpected(scale.error());
      if (*scale != 1 && *scale != 2 && *scale != 4) {
        return fail(ErrorCode::config_invalid, "--decode-scale: expected 1, 2 or 4");
      }
      inv.config.capture.decode_scale = *scale;
    } else if (arg == "--loose-format") {
      inv.config.capture.strict_format = false;

    } else if (arg == "--model") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      inv.config.model.path = *v;
    } else if (arg == "--model-variant") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      inv.config.model.variant = *v;
    } else if (arg == "--model-url") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      inv.config.model.url = *v;
    } else if (arg == "--model-sha") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      inv.config.model.sha256 = *v;
    } else if (arg == "--allow-unpinned") {
      inv.config.model.allow_unpinned = true;
    } else if (arg == "--offline") {
      inv.config.model.offline = true;

    } else if (arg == "--conf") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto conf = parse_float(arg, *v);
      if (!conf) return std::unexpected(conf.error());
      if (*conf < 0.0f || *conf > 1.0f) {
        return fail(ErrorCode::config_invalid, "--conf: must be within [0, 1]");
      }
      inv.config.inference.conf_threshold = *conf;
    } else if (arg == "--intra-threads") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto threads = parse_int(arg, *v);
      if (!threads) return std::unexpected(threads.error());
      if (*threads < 1) return fail(ErrorCode::config_invalid, "--intra-threads: must be >= 1");
      inv.config.inference.intra_op_threads = *threads;
    } else if (arg == "--fake-detector") {
      inv.config.inference.fake = true;

    } else if (arg == "--policy") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto policy = parse_policy(*v);
      if (!policy) return std::unexpected(policy.error());
      inv.config.tracking.policy = *policy;

    } else if (arg == "--serial") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      inv.config.serial.port = *v;
    } else if (arg == "--no-serial") {
      inv.config.serial.enabled = false;
    } else if (arg == "--track-seq") {
      inv.config.serial.send_seq = true;

    } else if (arg == "--preview") {
      inv.config.sink.preview = true;
    } else if (arg == "--snapshot") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      inv.config.sink.snapshot_path = *v;
    } else if (arg == "--rtsp") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      inv.config.sink.rtsp_url = *v;
    } else if (arg == "--bitrate") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      const auto kbps = parse_int(arg, *v);
      if (!kbps) return std::unexpected(kbps.error());
      if (*kbps <= 0) return fail(ErrorCode::config_invalid, "--bitrate: must be positive");
      inv.config.sink.bitrate_kbps = *kbps;
    } else if (arg == "--no-hw-encode") {
      inv.config.sink.hardware_encode = false;
    } else if (arg == "--vaapi-device") {
      const auto v = value_for(arg);
      if (!v) return std::unexpected(v.error());
      inv.config.sink.vaapi_device = *v;

    } else {
      return fail(ErrorCode::config_invalid, std::format("unknown flag: '{}'", arg));
    }
  }

  return inv;
}

} // namespace capture_eye
