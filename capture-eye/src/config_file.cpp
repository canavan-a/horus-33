#include "config_file.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <span>
#include <sstream>
#include <string_view>

namespace capture_eye {
namespace {

using Json = nlohmann::json;

[[nodiscard]] Result<void> reject_unknown_keys(const Json& obj, std::string_view section,
                                                std::span<const std::string_view> known) {
  for (const auto& [key, value] : obj.items()) {
    (void)value;
    const bool recognized =
        std::find(known.begin(), known.end(), key) != known.end();
    if (!recognized) {
      return fail(ErrorCode::config_invalid,
                  std::format("{}: unknown key '{}'", section, key));
    }
  }
  return {};
}

[[nodiscard]] bool matches_type(const Json& value, const std::string&) { return value.is_string(); }
[[nodiscard]] bool matches_type(const Json& value, const int&) { return value.is_number_integer(); }
[[nodiscard]] bool matches_type(const Json& value, const float&) { return value.is_number(); }
[[nodiscard]] bool matches_type(const Json& value, const double&) { return value.is_number(); }
[[nodiscard]] bool matches_type(const Json& value, const bool&) { return value.is_boolean(); }

template <typename T>
[[nodiscard]] Result<T> as(const Json& value, std::string_view where) {
  if (!matches_type(value, T{})) {
    return fail(ErrorCode::config_invalid, std::format("{}: wrong type", where));
  }
  return value.template get<T>();
}

[[nodiscard]] Result<TargetPolicy> parse_policy(const Json& value, std::string_view where) {
  const auto text = as<std::string>(value, where);
  if (!text) return std::unexpected(text.error());
  if (*text == "sticky") return TargetPolicy::sticky_largest;
  if (*text == "largest") return TargetPolicy::largest_area;
  if (*text == "confident") return TargetPolicy::most_confident;
  if (*text == "closest") return TargetPolicy::nearest_center;
  return fail(ErrorCode::config_invalid,
              std::format("{}: expected sticky|largest|confident|closest, got '{}'", where, *text));
}

[[nodiscard]] Result<std::uint32_t> parse_fourcc(const Json& value, std::string_view where) {
  const auto text = as<std::string>(value, where);
  if (!text) return std::unexpected(text.error());
  if (text->size() != 4) {
    return fail(ErrorCode::config_invalid,
                std::format("{}: expected 4 characters, got '{}'", where, *text));
  }
  const char code[5] = {(*text)[0], (*text)[1], (*text)[2], (*text)[3], '\0'};
  return fourcc_of(code);
}

#define TRY_ASSIGN(dest, expr)               \
  do {                                       \
    auto _r = (expr);                        \
    if (!_r) return std::unexpected(_r.error()); \
    dest = std::move(*_r);                   \
  } while (false)

[[nodiscard]] Result<void> parse_capture(const Json& obj, CaptureConfigOverlay& c) {
  static constexpr std::array<std::string_view, 10> kKnown{
      "device",       "width",        "height",         "fourcc",
      "fps",          "buffer_count", "decode_scale",   "strict_format",
      "flip_horizontal", "flip_vertical"};
  if (const auto ok = reject_unknown_keys(obj, "capture", kKnown); !ok) return ok;

  if (obj.contains("device")) TRY_ASSIGN(c.device, as<std::string>(obj["device"], "capture.device"));
  if (obj.contains("width")) TRY_ASSIGN(c.width, as<int>(obj["width"], "capture.width"));
  if (obj.contains("height")) TRY_ASSIGN(c.height, as<int>(obj["height"], "capture.height"));
  if (obj.contains("fourcc")) TRY_ASSIGN(c.fourcc, parse_fourcc(obj["fourcc"], "capture.fourcc"));
  if (obj.contains("fps")) TRY_ASSIGN(c.fps, as<int>(obj["fps"], "capture.fps"));
  if (obj.contains("buffer_count"))
    TRY_ASSIGN(c.buffer_count, as<int>(obj["buffer_count"], "capture.buffer_count"));
  if (obj.contains("decode_scale"))
    TRY_ASSIGN(c.decode_scale, as<int>(obj["decode_scale"], "capture.decode_scale"));
  if (obj.contains("strict_format"))
    TRY_ASSIGN(c.strict_format, as<bool>(obj["strict_format"], "capture.strict_format"));
  if (obj.contains("flip_horizontal"))
    TRY_ASSIGN(c.flip_horizontal, as<bool>(obj["flip_horizontal"], "capture.flip_horizontal"));
  if (obj.contains("flip_vertical"))
    TRY_ASSIGN(c.flip_vertical, as<bool>(obj["flip_vertical"], "capture.flip_vertical"));
  return {};
}

[[nodiscard]] Result<void> parse_model(const Json& obj, ModelConfigOverlay& c) {
  static constexpr std::array<std::string_view, 6> kKnown{
      "variant", "url", "sha256", "path", "offline", "allow_unpinned"};
  if (const auto ok = reject_unknown_keys(obj, "model", kKnown); !ok) return ok;

  if (obj.contains("variant")) TRY_ASSIGN(c.variant, as<std::string>(obj["variant"], "model.variant"));
  if (obj.contains("url")) TRY_ASSIGN(c.url, as<std::string>(obj["url"], "model.url"));
  if (obj.contains("sha256")) TRY_ASSIGN(c.sha256, as<std::string>(obj["sha256"], "model.sha256"));
  if (obj.contains("path")) TRY_ASSIGN(c.path, as<std::string>(obj["path"], "model.path"));
  if (obj.contains("offline")) TRY_ASSIGN(c.offline, as<bool>(obj["offline"], "model.offline"));
  if (obj.contains("allow_unpinned"))
    TRY_ASSIGN(c.allow_unpinned, as<bool>(obj["allow_unpinned"], "model.allow_unpinned"));
  return {};
}

[[nodiscard]] Result<void> parse_inference(const Json& obj, InferenceConfigOverlay& c) {
  static constexpr std::array<std::string_view, 5> kKnown{
      "input_size", "conf_threshold", "intra_op_threads", "inter_op_threads", "fake"};
  if (const auto ok = reject_unknown_keys(obj, "inference", kKnown); !ok) return ok;

  if (obj.contains("input_size"))
    TRY_ASSIGN(c.input_size, as<int>(obj["input_size"], "inference.input_size"));
  if (obj.contains("conf_threshold"))
    TRY_ASSIGN(c.conf_threshold, as<float>(obj["conf_threshold"], "inference.conf_threshold"));
  if (obj.contains("intra_op_threads"))
    TRY_ASSIGN(c.intra_op_threads, as<int>(obj["intra_op_threads"], "inference.intra_op_threads"));
  if (obj.contains("inter_op_threads"))
    TRY_ASSIGN(c.inter_op_threads, as<int>(obj["inter_op_threads"], "inference.inter_op_threads"));
  if (obj.contains("fake")) TRY_ASSIGN(c.fake, as<bool>(obj["fake"], "inference.fake"));
  return {};
}

[[nodiscard]] Result<void> parse_serial(const Json& obj, SerialConfigOverlay& c) {
  static constexpr std::array<std::string_view, 6> kKnown{
      "port", "baud", "enabled", "max_hz", "lost_repeat_hz", "send_seq"};
  if (const auto ok = reject_unknown_keys(obj, "serial", kKnown); !ok) return ok;

  if (obj.contains("port")) TRY_ASSIGN(c.port, as<std::string>(obj["port"], "serial.port"));
  if (obj.contains("baud")) TRY_ASSIGN(c.baud, as<int>(obj["baud"], "serial.baud"));
  if (obj.contains("enabled")) TRY_ASSIGN(c.enabled, as<bool>(obj["enabled"], "serial.enabled"));
  if (obj.contains("max_hz")) TRY_ASSIGN(c.max_hz, as<int>(obj["max_hz"], "serial.max_hz"));
  if (obj.contains("lost_repeat_hz"))
    TRY_ASSIGN(c.lost_repeat_hz, as<int>(obj["lost_repeat_hz"], "serial.lost_repeat_hz"));
  if (obj.contains("send_seq")) TRY_ASSIGN(c.send_seq, as<bool>(obj["send_seq"], "serial.send_seq"));
  return {};
}

[[nodiscard]] Result<void> parse_tracking(const Json& obj, TrackingConfigOverlay& c) {
  static constexpr std::array<std::string_view, 3> kKnown{"policy", "lock_iou", "lost_grace_ms"};
  if (const auto ok = reject_unknown_keys(obj, "tracking", kKnown); !ok) return ok;

  if (obj.contains("policy")) TRY_ASSIGN(c.policy, parse_policy(obj["policy"], "tracking.policy"));
  if (obj.contains("lock_iou"))
    TRY_ASSIGN(c.lock_iou, as<float>(obj["lock_iou"], "tracking.lock_iou"));
  if (obj.contains("lost_grace_ms")) {
    int ms = 0;
    TRY_ASSIGN(ms, as<int>(obj["lost_grace_ms"], "tracking.lost_grace_ms"));
    c.lost_grace = std::chrono::milliseconds{ms};
  }
  return {};
}

[[nodiscard]] Result<void> parse_sink(const Json& obj, SinkConfigOverlay& c) {
  static constexpr std::array<std::string_view, 7> kKnown{
      "preview", "snapshot_path", "snapshot_every", "rtsp_url", "bitrate_kbps",
      "hardware_encode", "vaapi_device"};
  if (const auto ok = reject_unknown_keys(obj, "sink", kKnown); !ok) return ok;

  if (obj.contains("preview")) TRY_ASSIGN(c.preview, as<bool>(obj["preview"], "sink.preview"));
  if (obj.contains("snapshot_path"))
    TRY_ASSIGN(c.snapshot_path, as<std::string>(obj["snapshot_path"], "sink.snapshot_path"));
  if (obj.contains("snapshot_every"))
    TRY_ASSIGN(c.snapshot_every, as<int>(obj["snapshot_every"], "sink.snapshot_every"));
  if (obj.contains("rtsp_url")) TRY_ASSIGN(c.rtsp_url, as<std::string>(obj["rtsp_url"], "sink.rtsp_url"));
  if (obj.contains("bitrate_kbps"))
    TRY_ASSIGN(c.bitrate_kbps, as<int>(obj["bitrate_kbps"], "sink.bitrate_kbps"));
  if (obj.contains("hardware_encode"))
    TRY_ASSIGN(c.hardware_encode, as<bool>(obj["hardware_encode"], "sink.hardware_encode"));
  if (obj.contains("vaapi_device"))
    TRY_ASSIGN(c.vaapi_device, as<std::string>(obj["vaapi_device"], "sink.vaapi_device"));
  return {};
}

[[nodiscard]] Result<void> parse_ingress(const Json& obj, IngressConfigOverlay& c) {
  static constexpr std::array<std::string_view, 2> kKnown{"socket_path", "max_clients"};
  if (const auto ok = reject_unknown_keys(obj, "ingress", kKnown); !ok) return ok;

  if (obj.contains("socket_path"))
    TRY_ASSIGN(c.socket_path, as<std::string>(obj["socket_path"], "ingress.socket_path"));
  if (obj.contains("max_clients"))
    TRY_ASSIGN(c.max_clients, as<int>(obj["max_clients"], "ingress.max_clients"));
  return {};
}

[[nodiscard]] Result<void> parse_clipping(const Json& obj, ClippingConfigOverlay& c) {
  static constexpr std::array<std::string_view, 8> kKnown{
      "enabled",     "output_dir",  "admin_socket_path", "pre_roll_seconds",
      "stop_after_ticks", "bitrate_kbps", "hardware_encode", "vaapi_device"};
  if (const auto ok = reject_unknown_keys(obj, "clipping", kKnown); !ok) return ok;

  if (obj.contains("enabled")) TRY_ASSIGN(c.enabled, as<bool>(obj["enabled"], "clipping.enabled"));
  if (obj.contains("output_dir"))
    TRY_ASSIGN(c.output_dir, as<std::string>(obj["output_dir"], "clipping.output_dir"));
  if (obj.contains("admin_socket_path"))
    TRY_ASSIGN(c.admin_socket_path,
               as<std::string>(obj["admin_socket_path"], "clipping.admin_socket_path"));
  if (obj.contains("pre_roll_seconds"))
    TRY_ASSIGN(c.pre_roll_seconds, as<double>(obj["pre_roll_seconds"], "clipping.pre_roll_seconds"));
  if (obj.contains("stop_after_ticks"))
    TRY_ASSIGN(c.stop_after_ticks, as<int>(obj["stop_after_ticks"], "clipping.stop_after_ticks"));
  if (obj.contains("bitrate_kbps"))
    TRY_ASSIGN(c.bitrate_kbps, as<int>(obj["bitrate_kbps"], "clipping.bitrate_kbps"));
  if (obj.contains("hardware_encode"))
    TRY_ASSIGN(c.hardware_encode, as<bool>(obj["hardware_encode"], "clipping.hardware_encode"));
  if (obj.contains("vaapi_device"))
    TRY_ASSIGN(c.vaapi_device, as<std::string>(obj["vaapi_device"], "clipping.vaapi_device"));
  return {};
}

#undef TRY_ASSIGN

} // namespace

Result<ConfigOverlay> load_config_file(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file) {
    return fail(ErrorCode::config_invalid,
                std::format("--config: cannot open '{}'", path.string()));
  }

  std::stringstream buffer;
  buffer << file.rdbuf();

  Json root;
  try {
    root = Json::parse(buffer.str());
  } catch (const Json::parse_error& e) {
    return fail(ErrorCode::config_invalid,
                std::format("--config: {}: invalid JSON: {}", path.string(), e.what()));
  }

  if (!root.is_object()) {
    return fail(ErrorCode::config_invalid,
                std::format("--config: {}: top level must be an object", path.string()));
  }

  static constexpr std::array<std::string_view, 8> kSections{
      "capture", "model", "inference", "serial", "tracking", "sink", "ingress", "clipping"};
  if (const auto ok = reject_unknown_keys(root, "<top level>", kSections); !ok) {
    return std::unexpected(ok.error());
  }

  ConfigOverlay overlay;
  if (root.contains("capture")) {
    if (const auto ok = parse_capture(root["capture"], overlay.capture); !ok)
      return std::unexpected(ok.error());
  }
  if (root.contains("model")) {
    if (const auto ok = parse_model(root["model"], overlay.model); !ok)
      return std::unexpected(ok.error());
  }
  if (root.contains("inference")) {
    if (const auto ok = parse_inference(root["inference"], overlay.inference); !ok)
      return std::unexpected(ok.error());
  }
  if (root.contains("serial")) {
    if (const auto ok = parse_serial(root["serial"], overlay.serial); !ok)
      return std::unexpected(ok.error());
  }
  if (root.contains("tracking")) {
    if (const auto ok = parse_tracking(root["tracking"], overlay.tracking); !ok)
      return std::unexpected(ok.error());
  }
  if (root.contains("sink")) {
    if (const auto ok = parse_sink(root["sink"], overlay.sink); !ok)
      return std::unexpected(ok.error());
  }
  if (root.contains("ingress")) {
    if (const auto ok = parse_ingress(root["ingress"], overlay.ingress); !ok)
      return std::unexpected(ok.error());
  }
  if (root.contains("clipping")) {
    if (const auto ok = parse_clipping(root["clipping"], overlay.clipping); !ok)
      return std::unexpected(ok.error());
  }
  return overlay;
}

} // namespace capture_eye
