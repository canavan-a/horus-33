#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "error.h"

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
  // Mount-orientation correction, applied once right after decode — see
  // apply_flip (frame_mat.h). Config-only, not exposed at runtime: flipping
  // mid-run would need every in-flight detection's coordinates
  // reinterpreted, for a correction that is fixed by how the camera is
  // physically mounted and never needs to change while running.
  bool flip_horizontal = false;
  bool flip_vertical = false;
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

// Which runtime turns pixels into boxes. Both sit behind the same Detector
// seam, so this is the only thing that has to change to swap one for the other.
// openvino is only present in a binary built with -DCAPTURE_EYE_OPENVINO=ON;
// asking for it anywhere else is a startup error rather than a silent
// downgrade, because the whole point of choosing it is to get its performance.
enum class InferenceBackend { onnx, openvino };

struct InferenceConfig {
  InferenceBackend backend = InferenceBackend::onnx;
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
  // Opt-in: VAAPI keeps the encode off the CPU cores that inference wants, but
  // plenty of machines have no usable render node, so software encode is what
  // every deployment is guaranteed to start on. When hardware is asked for and
  // the device or driver turns out to be unavailable, falls back to libx264.
  bool hardware_encode = false;
  std::filesystem::path vaapi_device = "/dev/dri/renderD128";
};

// The control relay: lets other processes (horusctl, a future REST server)
// send describe/set/ping to the device through capture-eye, which is the only
// process allowed to hold the serial port. Off unless a socket path is given.
struct IngressConfig {
  std::filesystem::path socket_path;  // empty = disabled
  int max_clients = 8;
};

// Recording clips of people to disk. Off unless output_dir is set. A second,
// separate host-only admin socket (not the ingress relay above, which is a
// dumb ESP32-only pipe) lets horus-server flip `enabled` live and poll status
// without a restart.
struct ClippingConfig {
  bool enabled = false;
  std::filesystem::path output_dir;         // empty = disabled even if enabled=true
  std::filesystem::path admin_socket_path;  // empty = no live admin toggling
  double pre_roll_seconds = 1.0;
  // Consecutive inference ticks with nobody in frame before a clip stops —
  // ticks, not video frames or milliseconds, because capture and inference
  // run at different, drifting rates (see ClipPolicy).
  int stop_after_ticks = 40;
  int bitrate_kbps = 4000;
  bool hardware_encode = false;  // opt-in, for the reasons SinkConfig gives
  std::filesystem::path vaapi_device = "/dev/dri/renderD128";
};

struct AppConfig {
  CaptureConfig capture;
  ModelConfig model;
  InferenceConfig inference;
  SerialConfig serial;
  TrackingConfig tracking;
  SinkConfig sink;
  IngressConfig ingress;
  ClippingConfig clipping;
};

// Mirrors AppConfig field-for-field, but every field is optional: absent means
// "this source did not mention it" rather than "use the default". Two sources
// (a config file, the command line) each produce one of these; merge() decides
// what wins. This is what keeps "--fps not passed" distinguishable from
// "--fps 60 passed", so a config file's fps: 30 can't be silently overridden by
// a CLI default (args.cpp, parse_args).
struct CaptureConfigOverlay {
  std::optional<std::filesystem::path> device;
  std::optional<int> width;
  std::optional<int> height;
  std::optional<std::uint32_t> fourcc;
  std::optional<int> fps;
  std::optional<int> buffer_count;
  std::optional<int> decode_scale;
  std::optional<bool> strict_format;
  std::optional<bool> flip_horizontal;
  std::optional<bool> flip_vertical;
};

struct ModelConfigOverlay {
  std::optional<std::string> variant;
  std::optional<std::string> url;
  std::optional<std::string> sha256;
  std::optional<std::filesystem::path> path;
  std::optional<bool> offline;
  std::optional<bool> allow_unpinned;
};

struct InferenceConfigOverlay {
  std::optional<InferenceBackend> backend;
  std::optional<int> input_size;
  std::optional<float> conf_threshold;
  std::optional<int> intra_op_threads;
  std::optional<int> inter_op_threads;
  std::optional<bool> fake;
};

struct SerialConfigOverlay {
  std::optional<std::filesystem::path> port;
  std::optional<int> baud;
  std::optional<bool> enabled;
  std::optional<int> max_hz;
  std::optional<int> lost_repeat_hz;
  std::optional<bool> send_seq;
};

struct TrackingConfigOverlay {
  std::optional<TargetPolicy> policy;
  std::optional<float> lock_iou;
  std::optional<std::chrono::milliseconds> lost_grace;
};

struct SinkConfigOverlay {
  std::optional<bool> preview;
  std::optional<std::filesystem::path> snapshot_path;
  std::optional<int> snapshot_every;
  std::optional<std::string> rtsp_url;
  std::optional<int> bitrate_kbps;
  std::optional<bool> hardware_encode;
  std::optional<std::filesystem::path> vaapi_device;
};

struct IngressConfigOverlay {
  std::optional<std::filesystem::path> socket_path;
  std::optional<int> max_clients;
};

struct ClippingConfigOverlay {
  std::optional<bool> enabled;
  std::optional<std::filesystem::path> output_dir;
  std::optional<std::filesystem::path> admin_socket_path;
  std::optional<double> pre_roll_seconds;
  std::optional<int> stop_after_ticks;
  std::optional<int> bitrate_kbps;
  std::optional<bool> hardware_encode;
  std::optional<std::filesystem::path> vaapi_device;
};

struct ConfigOverlay {
  CaptureConfigOverlay capture;
  ModelConfigOverlay model;
  InferenceConfigOverlay inference;
  SerialConfigOverlay serial;
  TrackingConfigOverlay tracking;
  SinkConfigOverlay sink;
  IngressConfigOverlay ingress;
  ClippingConfigOverlay clipping;
};

// Layers `file` over `base`, then `flags` over the result. An absent optional
// never overwrites; a present one always does — this is the entire precedence
// rule (defaults < --config FILE < CLI flags) and it lives in exactly one
// place so nothing else has to reimplement it.
[[nodiscard]] AppConfig merge(const AppConfig& base, const ConfigOverlay& file,
                               const ConfigOverlay& flags);

// Range/sanity checks that used to be scattered across args.cpp's flag
// parsing. Runs after merge() so a bad value from the config file is rejected
// exactly like a bad flag, rather than only the flag path being checked.
[[nodiscard]] Result<void> validate(const AppConfig& config);

} // namespace capture_eye
