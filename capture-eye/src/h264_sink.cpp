#include "h264_sink.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

#include <cstdio>
#include <format>
#include <memory>

#include "audio_source.h"
#include "h264_encoder.h"

namespace capture_eye {
namespace {

[[nodiscard]] Result<std::shared_ptr<Encoder>> build(const SinkConfig& config,
                                                     const std::shared_ptr<AudioSource>& audio,
                                                     int width, int height, int fps) {
  auto encoder = std::make_shared<Encoder>();

  AVFormatContext* format = nullptr;
  const int allocated =
      avformat_alloc_output_context2(&format, nullptr, "rtsp", config.rtsp_url.c_str());
  if (allocated < 0 || format == nullptr) {
    return fail(ErrorCode::sink_failed, std::format("rtsp output: {}", av_error(allocated)));
  }
  encoder->format.reset(format);

  // TCP rather than UDP: a dropped packet in an annotated video stream is far
  // more annoying than the small latency cost, and it traverses loopback and
  // most networks without tuning.
  av_opt_set(format->priv_data, "rtsp_transport", "tcp", 0);
  // Default RTP payloads come out at 1460 bytes, which MediaMTX then has to
  // re-fragment; emitting them small enough in the first place avoids a remux
  // on every packet.
  av_opt_set(format->priv_data, "pkt_size", "1200", 0);

  if (const auto configured = configure_codec(*encoder, width, height, fps, config.bitrate_kbps,
                                              config.hardware_encode, config.vaapi_device);
      !configured) {
    return std::unexpected(configured.error());
  }

  // Both streams have to exist before the header goes out.
  AVStream* audio_stream = nullptr;
  if (audio) {
    auto added = audio->add_stream(format);
    if (!added) return std::unexpected(added.error());
    audio_stream = *added;
    // The interleaver holds each stream's packets until the other catches
    // up; if video stalls (camera hiccup), don't let audio back up for the
    // default 10s before it is flushed anyway.
    format->max_interleave_delta = 500'000;
  }

  // RTSP's output format carries AVFMT_NOFILE — avformat_write_header opens
  // the network connection itself, so there is no avio handle for us to close.
  encoder->needs_avio_close = false;

  if (const int header = avformat_write_header(format, nullptr); header < 0) {
    return fail(ErrorCode::sink_failed,
                std::format("connect {}: {}", config.rtsp_url, av_error(header)));
  }
  encoder->header_written = true;

  std::fprintf(stderr, "h264: %s %dx%d @%dfps %dkbps -> %s\n", encoder->encoder_name.c_str(),
               width, height, fps, config.bitrate_kbps, config.rtsp_url.c_str());

  if (audio_stream != nullptr) {
    // One epoch for both streams, fixed now rather than at the first video
    // frame, so audio that arrives first is already on the shared timeline.
    // Every frame this sink sees is dequeued after this point, so video pts
    // stay non-negative.
    encoder->first_frame = std::chrono::steady_clock::now();
    encoder->started = true;
    // Holds the encoder, so the stream stays writable for as long as the
    // source can still call this — see AudioSource::subscribe.
    audio->subscribe([encoder, audio_stream, failing = false](const AudioPacket& packet) mutable {
      // Logs the first failure of a run, not 50 a second while MediaMTX is down.
      if (const auto ok = mux_audio_packet(*encoder, audio_stream, packet, encoder->first_frame);
          !ok) {
        if (!failing) std::fprintf(stderr, "audio: rtsp: %s\n", to_string(ok.error()).c_str());
        failing = true;
      } else {
        failing = false;
      }
    });
  }
  return encoder;
}

} // namespace

Result<FrameSink> make_h264_rtsp_sink(const SinkConfig& config, std::shared_ptr<AudioSource> audio,
                                      int width, int height, int fps) {
  if (config.rtsp_url.empty()) {
    return fail(ErrorCode::config_invalid, "rtsp sink needs a url");
  }
  avformat_network_init();

  auto encoder = build(config, audio, width, height, fps);
  if (!encoder) return std::unexpected(encoder.error());

  FrameSink sink;
  sink.name = "h264";
  sink.submit = [state = *encoder](const AnnotatedFrame& annotated) -> Result<void> {
    return state->encode(annotated);
  };
  return sink;
}

} // namespace capture_eye
