#include "pipeline.h"

#include <opencv2/core.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

#include "channel.h"
#include "control_relay.h"
#include "emission_policy.h"
#include "frame_decoder.h"
#include "frame_mat.h"
#include "frame_pool.h"
#include "metrics.h"
#include "overlay.h"
#include "serial_queue.h"
#include "slot.h"
#include "target_selector.h"
#include "track_message.h"
#include "v4l2_device.h"

namespace capture_eye {
namespace {

using Clock = std::chrono::steady_clock;

// Enough buffers for one being filled, one in the slot, one held by inference,
// two queued for the overlay, and one spare.
constexpr std::size_t kPoolSize = 6;
constexpr std::size_t kOverlayQueue = 3;
constexpr std::size_t kCommandQueue = 16;

} // namespace

struct Pipeline::Impl {
  AppConfig config;
  Stages stages;

  V4l2Device device;
  FrameDecoder decoder;
  FramePool pool;

  Slot<FrameRef> latest_frame;
  Slot<DetectionResult> latest_detections;
  Channel<FrameRef> to_overlay{kOverlayQueue, Overflow::drop_oldest};
  // Tracks overwrite (newest wins); commands from the control relay queue in
  // order and are never dropped silently. Both feed the same device-owning
  // thread, which stays the sole writer to the serial port even with several
  // relay clients attached.
  SerialQueue to_device{kCommandQueue};
  std::unique_ptr<ControlRelay> relay;  // null unless config.ingress.socket_path is set

  Metrics metrics;

  // Last computed rates, so the on-screen readout can show them without the
  // overlay stage having to keep its own timing history.
  std::atomic<std::uint64_t> last_capture_fps{0};
  std::atomic<std::uint64_t> last_inference_fps{0};

  // The pool holds a mutex and so cannot be moved in; it is built in place from
  // the decoder's output size.
  Impl(AppConfig cfg, Stages stgs, V4l2Device dev, FrameDecoder dec)
      : config{std::move(cfg)},
        stages{std::move(stgs)},
        device{std::move(dev)},
        decoder{dec},
        pool{kPoolSize, dec.output_width(), dec.output_height()} {}

  void capture_loop(std::stop_token token);
  void inference_loop(std::stop_token token);
  void overlay_loop(std::stop_token token);
  void device_loop(std::stop_token token);
  void metrics_loop(std::stop_token token);
};

Result<std::unique_ptr<Pipeline>> Pipeline::create(const AppConfig& config, Stages stages,
                                                   const SinkFactory& late_sinks) {
  auto device = V4l2Device::open(config.capture);
  if (!device) return std::unexpected(device.error());

  const auto& granted = device->granted();
  std::fprintf(stderr, "camera: %s %dx%d @%dfps (%s)\n", config.capture.device.c_str(),
               granted.width, granted.height, granted.fps,
               fourcc_string(granted.fourcc).c_str());

  auto decoder = FrameDecoder::create(granted.fourcc, granted.width, granted.height,
                                      config.capture.decode_scale);
  if (!decoder) return std::unexpected(decoder.error());

  if (late_sinks) {
    auto extra = late_sinks(decoder->output_width(), decoder->output_height(), granted.fps);
    if (!extra) return std::unexpected(extra.error());
    for (auto& sink : *extra) stages.frame_sinks.push_back(std::move(sink));
  }

  auto impl = std::make_unique<Impl>(config, std::move(stages), std::move(*device), *decoder);

  if (!config.ingress.socket_path.empty()) {
    auto relay = ControlRelay::create(config.ingress.socket_path,
                                      static_cast<std::size_t>(config.ingress.max_clients),
                                      impl->to_device);
    if (!relay) return std::unexpected(relay.error());
    impl->relay = std::move(*relay);
  }

  return std::unique_ptr<Pipeline>{new Pipeline{std::move(impl)}};
}

Pipeline::Pipeline(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
Pipeline::~Pipeline() = default;

void Pipeline::Impl::capture_loop(std::stop_token token) {
  std::uint64_t seq = 0;

  while (!token.stop_requested()) {
    auto buffer = device.next(std::chrono::milliseconds{200});
    if (!buffer) {
      std::fprintf(stderr, "capture: %s\n", to_string(buffer.error()).c_str());
      break;
    }
    if (!buffer->has_value()) continue;  // timeout; check the stop token again

    // Stamped at dequeue, before decode, so the reported lag covers everything
    // that happens to a frame after the driver hands it over.
    const auto arrived = Clock::now();

    auto lease = pool.acquire();
    if (!lease.has_value()) {
      // Every buffer is downstream. Dropping here is correct: stalling would
      // back up the driver's queue and cost us more than one frame.
      metrics.pool_starved.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    Frame& frame = lease->frame();
    if (const auto decoded = decoder.decode((*buffer)->data(), frame); !decoded) {
      metrics.decode_failures.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    // Mount-orientation correction, before anything else — see apply_flip's
    // comment for why this happens here and only here.
    if (config.capture.flip_horizontal || config.capture.flip_vertical) {
      cv::Mat mat = mat_for(frame);
      apply_flip(mat, config.capture.flip_horizontal, config.capture.flip_vertical);
    }

    frame.seq = seq++;
    frame.captured_at = arrived;

    // Release the driver buffer before publishing: it is a scarce resource and
    // downstream stages hold pool slabs, not driver buffers.
    buffer->reset();

    const FrameRef shared = std::make_shared<const FrameLease>(std::move(*lease));
    latest_frame.publish(shared);
    to_overlay.push(shared);
    metrics.frames_captured.fetch_add(1, std::memory_order_relaxed);
  }

  latest_frame.close();
  to_overlay.close();
}

void Pipeline::Impl::inference_loop(std::stop_token token) {
  TargetSelector selector;
  selector.policy = config.tracking.policy;
  selector.lock_iou = config.tracking.lock_iou;

  std::uint32_t track_seq = 0;

  EmissionPolicy emission;
  emission.max_hz = config.serial.max_hz;
  emission.lost_repeat_hz = config.serial.lost_repeat_hz;
  emission.lost_grace = config.tracking.lost_grace;

  while (!token.stop_requested()) {
    auto frame_ref = latest_frame.take_blocking(token);
    if (!frame_ref.has_value()) break;

    const Frame& frame = (*frame_ref)->frame();
    const auto started = Clock::now();
    auto people = stages.detector.detect(frame);
    const auto finished = Clock::now();

    if (!people) {
      metrics.inference_failures.fetch_add(1, std::memory_order_relaxed);
      std::fprintf(stderr, "inference: %s\n", to_string(people.error()).c_str());
      continue;
    }

    const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(finished - started);
    metrics.inferences.fetch_add(1, std::memory_order_relaxed);
    metrics.inference_latency_us.store(static_cast<std::uint64_t>(latency.count()),
                                       std::memory_order_relaxed);

    const auto selected = selector.select(*people);

    DetectionResult result;
    result.frame_seq = frame.seq;
    result.captured_at = frame.captured_at;
    result.people = *people;
    result.latency = latency;
    result.selected = selected;
    latest_detections.publish(std::move(result));

    // Selection and emission run here rather than on the serial thread, so the
    // serial stage stays a dumb writer and the policy sees inference timing.
    std::optional<TrackMessage> message;
    if (selected.has_value()) {
      message = track_from_box(selected->box, frame.width, frame.height, selected->confidence);
    }

    const auto decision = emission.update(message, finished);
    if (decision.forget_target) selector.forget();
    if (decision.message.has_value()) {
      auto outgoing = *decision.message;
      if (config.serial.send_seq) {
        // Starts at 1: zero would read as "no seq" on the device. Stays well
        // clear of the control relay's own seq range (0x8000'0000+), so a
        // reply to one can never be misrouted as a reply to the other.
        outgoing.seq = ++track_seq;
      }
      to_device.publish_track(outgoing);
    }
  }

  latest_detections.close();
}

void Pipeline::Impl::overlay_loop(std::stop_token token) {
  // Scratch buffer: the pooled pixels are shared and immutable, so drawing
  // happens on a copy. One memcpy per frame is cheap next to the alternative of
  // synchronising writers against readers.
  cv::Mat scratch;
  Frame annotated_frame;

  while (!token.stop_requested()) {
    auto frame_ref = to_overlay.pop_blocking(token);
    if (!frame_ref.has_value()) break;

    const Frame& source = (*frame_ref)->frame();
    mat_for(source).copyTo(scratch);

    annotated_frame = source;
    annotated_frame.bytes = std::span{reinterpret_cast<std::byte*>(scratch.data),
                                      static_cast<std::size_t>(scratch.total()) * scratch.elemSize()};

    const auto detections = latest_detections.peek();
    std::span<const Detection> people;
    std::optional<Detection> selected;
    HudInfo hud;
    const auto now = Clock::now();

    if (detections.has_value()) {
      people = detections->people;
      selected = detections->selected;  // the selector's real choice, not a guess
      hud.detection_age =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - detections->captured_at);
      hud.inference_ms = static_cast<double>(detections->latency.count()) / 1000.0;
      metrics.detection_age_us.store(
          static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(now - detections->captured_at)
                  .count()),
          std::memory_order_relaxed);
    }
    if (selected.has_value()) {
      hud.track = track_from_box(selected->box, source.width, source.height, selected->confidence);
    }
    hud.capture_fps = last_capture_fps.load(std::memory_order_relaxed);
    hud.inference_fps = last_inference_fps.load(std::memory_order_relaxed);

    draw_detections(scratch, people, selected);
    draw_hud(scratch, hud);

    metrics.render_lag_us.store(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - source.captured_at)
                .count()),
        std::memory_order_relaxed);

    const AnnotatedFrame payload{.image = annotated_frame,
                                 .detections = people,
                                 .selected = selected,
                                 .seq = source.seq,
                                 .captured_at = source.captured_at,
                                 .detection_seq = detections.has_value() ? detections->frame_seq : 0};

    for (const auto& sink : stages.frame_sinks) {
      // Backstop: sinks are expected to return errors, but they wrap libraries
      // that throw (OpenCV does), and a debug snapshot failing must never take
      // down capture and tracking with it.
      try {
        if (const auto submitted = sink.submit(payload); !submitted) {
          metrics.sink_errors.fetch_add(1, std::memory_order_relaxed);
          std::fprintf(stderr, "sink %s: %s\n", sink.name.c_str(),
                       to_string(submitted.error()).c_str());
        }
      } catch (const std::exception& error) {
        metrics.sink_errors.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "sink %s threw: %s\n", sink.name.c_str(), error.what());
      }
    }
    metrics.frames_rendered.fetch_add(1, std::memory_order_relaxed);
  }
}

void Pipeline::Impl::device_loop(std::stop_token token) {
  // The sole thread that touches the device link, whether that's tracks from
  // inference_loop or commands relayed from other processes — this is what
  // keeps capture-eye the sole writer to the serial port even with a control
  // relay attached.
  //
  // Bounded wait rather than an unbounded one: replies to relayed commands
  // (describe/set/ping) can arrive when no track is being sent, and used to
  // only surface after the next write (serial_port.cpp's old pull-driven
  // reads). Polling read() on this fixed cadence regardless of write
  // activity is the fix.
  constexpr auto kReadPollInterval = std::chrono::milliseconds{20};

  while (!token.stop_requested()) {
    if (auto item = to_device.take_blocking_for(token, kReadPollInterval); item.has_value()) {
      const std::string line = std::holds_alternative<TrackMessage>(*item)
                                    ? encode_track(std::get<TrackMessage>(*item))
                                    : std::get<std::string>(*item);
      const bool is_track = std::holds_alternative<TrackMessage>(*item);

      if (const auto written = stages.device_link.write(line); !written) {
        metrics.track_errors.fetch_add(1, std::memory_order_relaxed);
      } else if (is_track) {
        metrics.tracks_sent.fetch_add(1, std::memory_order_relaxed);
        // Fan the track out to relay clients too (not just the device), so
        // horus-server can derive a person-present signal from the same
        // stream that drives the PID loop.
        if (relay) relay->publish_local(line);
      }
    }

    if (auto replies = stages.device_link.read(); replies.has_value()) {
      for (const auto& reply : *replies) {
        std::fprintf(stderr, "device: %s\n", reply.c_str());
        if (relay) relay->dispatch_incoming(reply);
      }
    }
  }
}

void Pipeline::Impl::metrics_loop(std::stop_token token) {
  MetricsSnapshot previous;
  std::uint64_t previous_skipped = 0;
  std::uint64_t previous_shed = 0;

  while (!token.stop_requested()) {
    // Sleep in short slices so shutdown is not delayed by a whole second.
    for (int i = 0; i < 10 && !token.stop_requested(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    if (token.stop_requested()) break;

    MetricsSnapshot current;
    current.frames_captured = metrics.frames_captured.load(std::memory_order_relaxed);
    current.inferences = metrics.inferences.load(std::memory_order_relaxed);
    current.frames_rendered = metrics.frames_rendered.load(std::memory_order_relaxed);
    current.tracks_sent = metrics.tracks_sent.load(std::memory_order_relaxed);
    current.pool_starved = metrics.pool_starved.load(std::memory_order_relaxed);
    current.decode_failures = metrics.decode_failures.load(std::memory_order_relaxed);
    current.inference_failures = metrics.inference_failures.load(std::memory_order_relaxed);
    current.sink_errors = metrics.sink_errors.load(std::memory_order_relaxed);
    current.track_errors = metrics.track_errors.load(std::memory_order_relaxed);

    const std::uint64_t skipped = latest_frame.dropped();
    const std::uint64_t shed = to_overlay.dropped();

    const MetricsSnapshot delta{
        .frames_captured = current.frames_captured - previous.frames_captured,
        .inferences = current.inferences - previous.inferences,
        .frames_rendered = current.frames_rendered - previous.frames_rendered,
        .tracks_sent = current.tracks_sent - previous.tracks_sent,
        .inference_skipped = skipped - previous_skipped,
        .overlay_dropped = shed - previous_shed,
        .pool_starved = current.pool_starved - previous.pool_starved,
        .decode_failures = current.decode_failures - previous.decode_failures,
        .inference_failures = current.inference_failures - previous.inference_failures,
        .sink_errors = current.sink_errors - previous.sink_errors,
        .track_errors = current.track_errors - previous.track_errors,
        .inference_latency_us = metrics.inference_latency_us.load(std::memory_order_relaxed),
        .render_lag_us = metrics.render_lag_us.load(std::memory_order_relaxed),
    };
    std::fprintf(stderr, "%s\n", delta.render().c_str());

    last_capture_fps.store(delta.frames_captured, std::memory_order_relaxed);
    last_inference_fps.store(delta.inferences, std::memory_order_relaxed);

    previous = current;
    previous_skipped = skipped;
    previous_shed = shed;
  }
}

Result<void> Pipeline::run(std::stop_token token) {
  Impl& impl = *impl_;

  // Each stage gets its own stop source so shutdown can propagate in pipeline
  // order: stop capture, let the downstream stages drain, then stop them.
  std::stop_source capture_stop;
  std::stop_source downstream_stop;

  {
    // Declared in reverse order of shutdown: destructors run bottom-up, so the
    // consumers join before the producers they depend on are gone.
    std::jthread metrics_thread{[&](std::stop_token t) { impl.metrics_loop(t); }};
    std::jthread device_thread{
        [&] { impl.device_loop(downstream_stop.get_token()); }};
    std::jthread overlay_thread{
        [&] { impl.overlay_loop(downstream_stop.get_token()); }};
    std::jthread inference_thread{
        [&] { impl.inference_loop(downstream_stop.get_token()); }};
    std::jthread capture_thread{[&] { impl.capture_loop(capture_stop.get_token()); }};

    // Wait for the caller to ask us to stop.
    std::stop_callback wake{token, [&] { capture_stop.request_stop(); }};
    while (!token.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    // Capture stops first and closes its outputs, which wakes every downstream
    // waiter; then they are told to stop so they exit their loops.
    capture_stop.request_stop();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    downstream_stop.request_stop();
    impl.latest_frame.close();
    impl.latest_detections.close();
    impl.to_overlay.close();
    impl.to_device.close();
    metrics_thread.request_stop();
  }

  std::fprintf(stderr, "stopped: %llu frames captured, %llu inferences, %llu tracks\n",
               static_cast<unsigned long long>(impl.metrics.frames_captured.load()),
               static_cast<unsigned long long>(impl.metrics.inferences.load()),
               static_cast<unsigned long long>(impl.metrics.tracks_sent.load()));
  return {};
}

} // namespace capture_eye
