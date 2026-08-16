#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace capture_eye {

// Four chars in the order v4l2 wants them, e.g. fourcc_of("MJPG").
[[nodiscard]] constexpr std::uint32_t fourcc_of(const char (&s)[5]) {
  return static_cast<std::uint32_t>(s[0]) | (static_cast<std::uint32_t>(s[1]) << 8) |
         (static_cast<std::uint32_t>(s[2]) << 16) | (static_cast<std::uint32_t>(s[3]) << 24);
}

[[nodiscard]] inline std::string fourcc_string(std::uint32_t fourcc) {
  return {static_cast<char>(fourcc & 0xff), static_cast<char>((fourcc >> 8) & 0xff),
          static_cast<char>((fourcc >> 16) & 0xff), static_cast<char>((fourcc >> 24) & 0xff)};
}

struct CaptureConfig {
  std::filesystem::path device = "/dev/video0";
  int width = 1280;
  int height = 720;
  std::uint32_t fourcc = fourcc_of("MJPG");
  int fps = 60;
  int buffer_count = 4;  // v4l2 driver buffers
  int decode_scale = 1;  // 1, 2 or 4; JPEG decode downscale
  // The driver may grant something other than what we asked for. Failing loudly
  // beats silently running at 5fps.
  bool strict_format = true;
};

// The default is the fp32 export; the fp16/int8/quantized variants live at the
// same URL prefix and are worth benchmarking against it.
struct ModelConfig {
  std::string variant = "model";
  std::string url;      // empty = derive from variant
  std::string sha256;   // empty = look up the pinned hash for this variant
  std::filesystem::path path;  // non-empty = use this file, never fetch
  bool offline = false;
  bool allow_unpinned = false;
};

struct InferenceConfig {
  int input_size = 640;
  float conf_threshold = 0.35f;
  // NOT 0 (= every core). ORT's intra-op pool spin-waits, and starving the
  // capture thread costs more fps than the extra threads buy.
  int intra_op_threads = 2;
  int inter_op_threads = 1;
  // Runs the whole pipeline against a synthetic detector, so capture, tracking
  // and serial can be exercised without a model.
  bool fake = false;
};

struct SerialConfig {
  std::filesystem::path port = "/dev/ttyACM0";
  int baud = 115200;
  bool enabled = true;
  int max_hz = 60;
  int lost_repeat_hz = 5;
  // Off by default: a non-zero seq makes the device ack every track message,
  // doubling link traffic (firmware/src/main.cpp:113-115).
  bool send_seq = false;
};

enum class TargetPolicy { most_confident, largest_area, nearest_center, sticky_largest };

struct TrackingConfig {
  TargetPolicy policy = TargetPolicy::sticky_largest;
  float lock_iou = 0.3f;
  std::chrono::milliseconds lost_grace{200};
};

struct SinkConfig {
  bool preview = false;
  std::filesystem::path snapshot_path;  // empty = no snapshots
  int snapshot_every = 30;              // frames between writes

  std::string rtsp_url;  // empty = no video egress
  int bitrate_kbps = 4000;
  // VAAPI keeps the encode off the CPU cores that inference wants. Falls back
  // to libx264 when the device or driver is unavailable.
  bool hardware_encode = true;
  std::filesystem::path vaapi_device = "/dev/dri/renderD128";
};

struct AppConfig {
  CaptureConfig capture;
  ModelConfig model;
  InferenceConfig inference;
  SerialConfig serial;
  TrackingConfig tracking;
  SinkConfig sink;
};

} // namespace capture_eye
