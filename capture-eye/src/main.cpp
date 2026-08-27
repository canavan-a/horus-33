#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <format>
#include <string_view>
#include <thread>
#include <vector>

#include "args.h"
#include "clip_admin.h"
#include "clip_sink.h"
#include "config_file.h"
#include "detector.h"
#include "device_link.h"
#include "error.h"
#include "frame_sink.h"
#include "h264_sink.h"
#include "model_store.h"
#include "onnx_detector.h"
#ifdef CAPTURE_EYE_OPENVINO
#include "openvino_detector.h"
#endif
#include "pipeline.h"
#include "serial_port.h"
#include "track_message.h"
#include "v4l2_device.h"

namespace capture_eye {
namespace {

// The one piece of process-wide mutable state, and only because a signal
// handler cannot be given a context. A lock-free atomic rather than
// volatile sig_atomic_t: the flag is written in the handler and read from the
// watchdog thread, and only an atomic gives that cross-thread ordering.
std::atomic<bool> g_interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free,
              "the signal handler may only touch a lock-free atomic");

extern "C" void handle_interrupt(int) {
  // A handler must leave errno as it found it; the interrupted code may be
  // midway through checking it.
  const int saved_errno = errno;
  g_interrupted.store(true, std::memory_order_relaxed);
  errno = saved_errno;
}

[[nodiscard]] Result<int> list_formats(const AppConfig& config) {
  const auto formats = enumerate_formats(config.capture.device);
  if (!formats) return std::unexpected(formats.error());

  std::printf("%s:\n", config.capture.device.c_str());
  for (const auto& format : *formats) {
    std::printf("  %s (%s)\n", fourcc_string(format.fourcc).c_str(), format.description.c_str());
    for (const auto& size : format.sizes) {
      std::printf("    %dx%d:", size.width, size.height);
      for (const int fps : size.fps_options) std::printf(" %dfps", fps);
      std::printf("\n");
    }
  }
  return 0;
}

// Builds whichever real inference backend the config asks for. Asking for one
// this binary was not built with is an error, not a fallback: the only reason
// to select a backend is to get its performance, so silently running the other
// one would make a benchmark lie.
[[nodiscard]] Result<Detector> make_backend_detector(const InferenceConfig& config,
                                                     const std::filesystem::path& model) {
  switch (config.backend) {
    case InferenceBackend::openvino:
#ifdef CAPTURE_EYE_OPENVINO
      return make_openvino_detector(config, model);
#else
      return fail(ErrorCode::config_invalid,
                  "inference.backend is openvino, but this capture-eye was built without it "
                  "(configure with -DCAPTURE_EYE_OPENVINO=ON)");
#endif
    case InferenceBackend::onnx:
      break;
  }
  return make_onnx_detector(config, model);
}

// Runs the detector over one image file. This is how detection accuracy gets
// checked against a known result without a camera, a gimbal, or a person.
[[nodiscard]] Result<int> detect_image(const Invocation& inv) {
  const cv::Mat image = cv::imread(inv.image.string(), cv::IMREAD_COLOR);
  if (image.empty()) {
    return fail(ErrorCode::decode_failed, std::format("cannot read {}", inv.image.string()));
  }

  const auto cache_root = default_cache_root();
  if (!cache_root) return std::unexpected(cache_root.error());
  const auto model = ensure_model(inv.config.model, *cache_root);
  if (!model) return std::unexpected(model.error());

  auto detector = make_backend_detector(inv.config.inference, *model);
  if (!detector) return std::unexpected(detector.error());

  Frame frame;
  frame.width = image.cols;
  frame.height = image.rows;
  frame.bytes = std::span{reinterpret_cast<std::byte*>(image.data),
                          static_cast<std::size_t>(image.total()) * image.elemSize()};

  const auto started = std::chrono::steady_clock::now();
  const auto people = detector->detect(frame);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  if (!people) return std::unexpected(people.error());

  std::printf("%s: %dx%d, %zu people in %.1f ms\n", inv.image.string().c_str(), image.cols,
              image.rows, people->size(),
              std::chrono::duration<double, std::milli>(elapsed).count());
  for (const auto& person : *people) {
    const auto track =
        track_from_box(person.box, frame.width, frame.height, person.confidence);
    std::printf("  conf %.3f  box [%.1f %.1f %.1f %.1f]  track x=%+.4f y=%+.4f\n",
                person.confidence, person.box.x1, person.box.y1, person.box.x2, person.box.y2,
                track.x, track.y);
  }
  return 0;
}

[[nodiscard]] Result<int> run_pipeline(const AppConfig& config) {
  Pipeline::Stages stages;
  if (config.inference.fake) {
    // 40ms matches the measured CPU cost of yolo26n, so the rate decoupling can
    // be exercised without a model or a GPU.
    stages.detector = make_fake_detector(std::chrono::milliseconds{40});
    std::fprintf(stderr, "detector: fake\n");
  } else {
    const auto cache_root = default_cache_root();
    if (!cache_root) return std::unexpected(cache_root.error());
    const auto model = ensure_model(config.model, *cache_root);
    if (!model) return std::unexpected(model.error());

    auto detector = make_backend_detector(config.inference, *model);
    if (!detector) return std::unexpected(detector.error());
    std::fprintf(stderr, "detector: %s %s\n", detector->name.c_str(),
                 model->filename().string().c_str());
    stages.detector = std::move(*detector);
  }

  if (!config.sink.snapshot_path.empty()) {
    auto snapshot = make_snapshot_sink(config.sink);
    if (!snapshot) return std::unexpected(snapshot.error());
    stages.frame_sinks.push_back(std::move(*snapshot));
  }

  if (config.sink.preview) {
    auto preview = make_preview_sink(config.sink);
    if (!preview) return std::unexpected(preview.error());
    stages.frame_sinks.push_back(std::move(*preview));
  }

  if (config.serial.enabled) {
    auto serial = make_serial_device_link(config.serial);
    if (!serial) return std::unexpected(serial.error());
    stages.device_link = std::move(*serial);
  } else {
    stages.device_link = make_stdout_device_link();
  }

  // The encoder needs the real decoded frame size, which is only known once the
  // camera has negotiated a format. clip_runtime escapes the lambda so a
  // ClipAdmin can be bound to it afterward, once the pipeline is up.
  std::shared_ptr<ClipRuntime> clip_runtime;
  const auto late_sinks = [&config, &clip_runtime](int width, int height,
                                                   int fps) -> Result<std::vector<FrameSink>> {
    std::vector<FrameSink> sinks;
    if (!config.sink.rtsp_url.empty()) {
      auto h264 = make_h264_rtsp_sink(config.sink, width, height, fps);
      if (!h264) return std::unexpected(h264.error());
      sinks.push_back(std::move(*h264));
    }
    if (config.clipping.enabled || !config.clipping.output_dir.empty()) {
      auto clip = make_clip_sink(config.clipping, width, height, fps);
      if (!clip) return std::unexpected(clip.error());
      clip_runtime = clip->runtime;
      sinks.push_back(std::move(clip->sink));
    }
    return sinks;
  };

  auto pipeline = Pipeline::create(config, std::move(stages), late_sinks);
  if (!pipeline) return std::unexpected(pipeline.error());

  std::unique_ptr<ClipAdmin> clip_admin;
  if (clip_runtime && !config.clipping.admin_socket_path.empty()) {
    auto admin = ClipAdmin::create(config.clipping.admin_socket_path, clip_runtime);
    if (!admin) return std::unexpected(admin.error());
    clip_admin = std::move(*admin);
  }

  std::stop_source stop;
  std::jthread watchdog{[&stop](std::stop_token token) {
    while (!token.stop_requested()) {
      if (g_interrupted.load(std::memory_order_relaxed)) {
        stop.request_stop();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
  }};

  const auto result = (*pipeline)->run(stop.get_token());
  watchdog.request_stop();
  if (!result) return std::unexpected(result.error());
  return 0;
}

// Layers a config file between the built-in defaults and the parsed flags.
// parse_args already resolved inv.config assuming no file was given (args.cpp
// is pure — it never touches the filesystem), so when --config is present
// that resolution has to be redone with the file layer in the middle:
// defaults < --config FILE < flags.
[[nodiscard]] Result<AppConfig> resolve_config(const Invocation& inv) {
  if (!inv.config_file.has_value()) return inv.config;

  const auto file_overlay = load_config_file(*inv.config_file);
  if (!file_overlay) return std::unexpected(file_overlay.error());

  const AppConfig merged = merge(AppConfig{}, *file_overlay, inv.overlay);
  if (const auto ok = validate(merged); !ok) return std::unexpected(ok.error());
  return merged;
}

[[nodiscard]] Result<int> run(const Invocation& inv) {
  switch (inv.command) {
    case Command::help:
      std::fputs(usage().c_str(), stdout);
      return 0;

    // Deliberately the whole of what this command does: resolve_config already
    // loads the file, merges it under the flags, and validates the result — the
    // same code path `run` itself takes. Anything more (opening the camera,
    // fetching the model) would defeat the point, which is a check that stays
    // safe to run as any user while the service is up. Silent on success, so
    // `capture-eye --check-config --config tmp && mv tmp real` reads cleanly.
    case Command::check_config: {
      const auto config = resolve_config(inv);
      if (!config) return std::unexpected(config.error());
      return 0;
    }

    case Command::list_formats: {
      const auto config = resolve_config(inv);
      if (!config) return std::unexpected(config.error());
      return list_formats(*config);
    }

    case Command::detect_image: {
      const auto config = resolve_config(inv);
      if (!config) return std::unexpected(config.error());
      Invocation resolved = inv;
      resolved.config = *config;
      return detect_image(resolved);
    }

    case Command::dump_model_io: {
      const auto config = resolve_config(inv);
      if (!config) return std::unexpected(config.error());
      const auto cache_root = default_cache_root();
      if (!cache_root) return std::unexpected(cache_root.error());
      const auto model = ensure_model(config->model, *cache_root);
      if (!model) return std::unexpected(model.error());

      const auto description = describe_model_io(*model);
      if (!description) return std::unexpected(description.error());
      std::fputs(description->c_str(), stdout);
      return 0;
    }

    case Command::run: {
      const auto config = resolve_config(inv);
      if (!config) return std::unexpected(config.error());
      return run_pipeline(*config);
    }
  }
  return 0;
}

} // namespace
} // namespace capture_eye

int main(int argc, char** argv) {
  const std::vector<std::string_view> args{argv + 1, argv + argc};

  const auto inv = capture_eye::parse_args(args);
  if (!inv) {
    std::fprintf(stderr, "capture-eye: %s\n", capture_eye::to_string(inv.error()).c_str());
    return 2;
  }

  std::signal(SIGINT, capture_eye::handle_interrupt);
  std::signal(SIGTERM, capture_eye::handle_interrupt);

  const auto result = capture_eye::run(*inv);
  if (!result) {
    std::fprintf(stderr, "capture-eye: %s\n", capture_eye::to_string(result.error()).c_str());
    return capture_eye::exit_code_for(result.error().code);
  }
  return *result;
}
