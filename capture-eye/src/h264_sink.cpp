#include "h264_sink.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

#include <cstdio>
#include <format>
#include <memory>

#include "audio_track.h"
#include "h264_encoder.h"

namespace capture_eye {
namespace {

// Member order is destruction order in reverse: the audio thread stops before
// the encoder writes its trailer and frees the muxer it was writing into.
struct RtspState {
  std::shared_ptr<Encoder> video;
  std::unique_ptr<AudioTrack> audio;
};

[[nodiscard]] Result<std::shared_ptr<RtspState>> build(const SinkConfig& config,
                                                       const AudioConfig& audio_config,
                                                       int width, int height, int fps) {
  auto state = std::make_shared<RtspState>();
  state->video = std::make_shared<Encoder>();
  Encoder& encoder = *state->video;

  AVFormatContext* format = nullptr;
  const int allocated =
      avformat_alloc_output_context2(&format, nullptr, "rtsp", config.rtsp_url.c_str());
  if (allocated < 0 || format == nullptr) {
    return fail(ErrorCode::sink_failed, std::format("rtsp output: {}", av_error(allocated)));
  }
  encoder.format.reset(format);

  // TCP rather than UDP: a dropped packet in an annotated video stream is far
  // more annoying than the small latency cost, and it traverses loopback and
  // most networks without tuning.
  av_opt_set(format->priv_data, "rtsp_transport", "tcp", 0);
  // Default RTP payloads come out at 1460 bytes, which MediaMTX then has to
  // re-fragment; emitting them small enough in the first place avoids a remux
  // on every packet.
  av_opt_set(format->priv_data, "pkt_size", "1200", 0);

  if (const auto configured = configure_codec(encoder, width, height, fps, config.bitrate_kbps,
                                              config.hardware_encode, config.vaapi_device);
      !configured) {
    return std::unexpected(configured.error());
  }

  // Both streams have to exist before the header goes out.
  if (!audio_config.device.empty()) {
    auto audio = AudioTrack::open(audio_config, format);
    if (audio) {
      state->audio = std::move(*audio);
      // The interleaver holds each stream's packets until the other catches
      // up; if video stalls (camera hiccup), don't let audio back up for the
      // default 10s before it is flushed anyway.
      format->max_interleave_delta = 500'000;
    } else {
      std::fprintf(stderr, "audio: %s; continuing without audio\n",
                   to_string(audio.error()).c_str());
    }
  }

  // RTSP's output format carries AVFMT_NOFILE — avformat_write_header opens
  // the network connection itself, so there is no avio handle for us to close.
  encoder.needs_avio_close = false;

  if (const int header = avformat_write_header(format, nullptr); header < 0) {
    return fail(ErrorCode::sink_failed,
                std::format("connect {}: {}", config.rtsp_url, av_error(header)));
  }
  encoder.header_written = true;

  std::fprintf(stderr, "h264: %s %dx%d @%dfps %dkbps -> %s\n", encoder.encoder_name.c_str(),
               width, height, fps, config.bitrate_kbps, config.rtsp_url.c_str());

  if (state->audio) {
    // One epoch for both streams, fixed now rather than at the first video
    // frame, so audio that arrives first is already on the shared timeline.
    // Every frame this sink sees is dequeued after this point, so video pts
    // stay non-negative.
    encoder.first_frame = std::chrono::steady_clock::now();
    encoder.started = true;
    state->audio->start(encoder.first_frame, [video = state->video.get()](AVPacket* packet) {
      return video->write(packet);
    });
  }
  return state;
}

} // namespace

Result<FrameSink> make_h264_rtsp_sink(const SinkConfig& config, const AudioConfig& audio,
                                      int width, int height, int fps) {
  if (config.rtsp_url.empty()) {
    return fail(ErrorCode::config_invalid, "rtsp sink needs a url");
  }
  avformat_network_init();

  auto state = build(config, audio, width, height, fps);
  if (!state) return std::unexpected(state.error());

  FrameSink sink;
  sink.name = "h264";
  sink.submit = [state = *state](const AnnotatedFrame& annotated) -> Result<void> {
    return state->video->encode(annotated);
  };
  return sink;
}

} // namespace capture_eye
