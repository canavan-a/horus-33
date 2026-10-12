#include "audio_track.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <format>
#include <mutex>
#include <thread>

#include "h264_encoder.h"

namespace capture_eye {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kSampleRate = 48000;

// How far the sample count may wander from the wall clock before it is pulled
// back. The mic's crystal and the host clock disagree by tens of ppm, which
// over hours is seconds of lip-sync error if never corrected; 100ms is well
// under what anyone notices and well over normal read jitter.
constexpr std::int64_t kMaxDriftSamples = kSampleRate / 10;

struct InputDeleter {
  void operator()(AVFormatContext* ctx) const { avformat_close_input(&ctx); }
};
struct FifoDeleter {
  void operator()(AVAudioFifo* fifo) const { av_audio_fifo_free(fifo); }
};

} // namespace

struct AudioTrack::Impl {
  std::unique_ptr<AVFormatContext, InputDeleter> input;
  std::unique_ptr<AVCodecContext, CodecDeleter> codec;
  std::unique_ptr<AVAudioFifo, FifoDeleter> fifo;
  std::unique_ptr<AVFrame, FrameDeleter> frame;
  std::unique_ptr<AVPacket, PacketDeleter> captured;
  std::unique_ptr<AVPacket, PacketDeleter> encoded;
  AVStream* stream = nullptr;  // owned by the muxer
  std::string device;

  // Polled by libav's interrupt callback, so a read blocked inside ALSA
  // returns promptly on shutdown instead of waiting for the next period.
  std::atomic<bool> stopping{false};

  // Last member: destroyed (and therefore joined) before everything the
  // thread uses.
  std::jthread reader;

  static int interrupted(void* opaque) {
    return static_cast<Impl*>(opaque)->stopping.load(std::memory_order_relaxed) ? 1 : 0;
  }

  [[nodiscard]] Result<void> encode(AVFrame* to_encode, const Writer& write);
  void run(std::stop_token token, Clock::time_point epoch, const Writer& write);
};

AudioTrack::AudioTrack(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}

AudioTrack::~AudioTrack() {
  if (impl_) impl_->stopping.store(true, std::memory_order_relaxed);
}

Result<std::unique_ptr<AudioTrack>> AudioTrack::open(const AudioConfig& config,
                                                     AVFormatContext* muxer) {
  static std::once_flag registered;
  std::call_once(registered, [] { avdevice_register_all(); });

  const AVInputFormat* alsa = av_find_input_format("alsa");
  if (alsa == nullptr) {
    return fail(ErrorCode::sink_failed, "this ffmpeg was built without ALSA input");
  }

  auto impl = std::make_unique<Impl>();
  impl->device = config.device;

  AVFormatContext* input = avformat_alloc_context();
  if (input == nullptr) return fail(ErrorCode::sink_failed, "cannot allocate audio input");
  input->interrupt_callback = AVIOInterruptCB{&Impl::interrupted, impl.get()};

  AVDictionary* options = nullptr;
  av_dict_set_int(&options, "sample_rate", kSampleRate, 0);
  av_dict_set(&options, "ch_layout", config.channels == 2 ? "stereo" : "mono", 0);
  const int opened = avformat_open_input(&input, config.device.c_str(), alsa, &options);
  av_dict_free(&options);
  if (opened < 0) {  // avformat_open_input frees `input` on failure
    return fail(ErrorCode::sink_failed, std::format("{}: {}", config.device, av_error(opened)));
  }
  impl->input.reset(input);

  // Like the camera, believe what the device granted, not what was asked for.
  const AVCodecParameters* granted = input->streams[0]->codecpar;
  if (granted->codec_id != AV_CODEC_ID_PCM_S16LE || granted->sample_rate != kSampleRate ||
      granted->ch_layout.nb_channels != config.channels) {
    return fail(ErrorCode::sink_failed,
                std::format("{}: granted {} {}Hz {}ch, need s16le {}Hz {}ch (try a plughw: device)",
                            config.device, avcodec_get_name(granted->codec_id),
                            granted->sample_rate, granted->ch_layout.nb_channels, kSampleRate,
                            config.channels));
  }

  const AVCodec* opus = avcodec_find_encoder_by_name("libopus");
  if (opus == nullptr) return fail(ErrorCode::sink_failed, "no libopus encoder available");

  AVCodecContext* codec_ctx = avcodec_alloc_context3(opus);
  if (codec_ctx == nullptr) return fail(ErrorCode::sink_failed, "cannot allocate audio encoder");
  impl->codec.reset(codec_ctx);

  codec_ctx->sample_rate = kSampleRate;
  codec_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
  av_channel_layout_default(&codec_ctx->ch_layout, config.channels);
  codec_ctx->bit_rate = static_cast<std::int64_t>(config.bitrate_kbps) * 1000;
  codec_ctx->time_base = AVRational{1, kSampleRate};
  if ((muxer->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
    codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  if (const int ok = avcodec_open2(codec_ctx, opus, nullptr); ok < 0) {
    return fail(ErrorCode::sink_failed, std::format("open libopus: {}", av_error(ok)));
  }

  impl->stream = avformat_new_stream(muxer, nullptr);
  if (impl->stream == nullptr) return fail(ErrorCode::sink_failed, "cannot create audio stream");
  impl->stream->time_base = codec_ctx->time_base;
  if (const int copied = avcodec_parameters_from_context(impl->stream->codecpar, codec_ctx);
      copied < 0) {
    return fail(ErrorCode::sink_failed, std::format("audio stream parameters: {}", av_error(copied)));
  }

  impl->fifo.reset(av_audio_fifo_alloc(AV_SAMPLE_FMT_S16, config.channels,
                                       codec_ctx->frame_size * 4));
  impl->frame.reset(av_frame_alloc());
  impl->captured.reset(av_packet_alloc());
  impl->encoded.reset(av_packet_alloc());
  if (!impl->fifo || !impl->frame || !impl->captured || !impl->encoded) {
    return fail(ErrorCode::sink_failed, "cannot allocate audio buffers");
  }
  impl->frame->format = AV_SAMPLE_FMT_S16;
  impl->frame->nb_samples = codec_ctx->frame_size;
  impl->frame->sample_rate = kSampleRate;
  av_channel_layout_copy(&impl->frame->ch_layout, &codec_ctx->ch_layout);
  if (const int got = av_frame_get_buffer(impl->frame.get(), 0); got < 0) {
    return fail(ErrorCode::sink_failed, std::format("audio frame buffer: {}", av_error(got)));
  }

  std::fprintf(stderr, "audio: %s %dHz %dch -> opus %dkbps\n", config.device.c_str(), kSampleRate,
               config.channels, config.bitrate_kbps);
  return std::unique_ptr<AudioTrack>{new AudioTrack{std::move(impl)}};
}

void AudioTrack::start(Clock::time_point epoch, Writer write) {
  impl_->reader = std::jthread{[impl = impl_.get(), epoch, write = std::move(write)](
                                   std::stop_token token) { impl->run(token, epoch, write); }};
}

Result<void> AudioTrack::Impl::encode(AVFrame* to_encode, const Writer& write) {
  if (const int sent = avcodec_send_frame(codec.get(), to_encode); sent < 0) {
    return fail(ErrorCode::sink_failed, std::format("audio encode: {}", av_error(sent)));
  }
  for (;;) {
    const int received = avcodec_receive_packet(codec.get(), encoded.get());
    if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
    if (received < 0) {
      return fail(ErrorCode::sink_failed, std::format("audio packet: {}", av_error(received)));
    }
    av_packet_rescale_ts(encoded.get(), codec->time_base, stream->time_base);
    encoded->stream_index = stream->index;
    if (const auto written = write(encoded.get()); !written) return written;
  }
  return {};
}

void AudioTrack::Impl::run(std::stop_token token, Clock::time_point epoch, const Writer& write) {
  const int frame_size = codec->frame_size;
  std::int64_t next_pts = -1;  // in samples since epoch
  bool failing = false;        // log a run of write failures once, not 50 times a second

  while (!token.stop_requested() && !stopping.load(std::memory_order_relaxed)) {
    const int read = av_read_frame(input.get(), captured.get());
    if (read == AVERROR(EAGAIN)) continue;
    if (read < 0) {
      // Unplugged or the device went away. Video carries on without audio;
      // a restart picks the mic back up.
      if (!stopping.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "audio: %s: %s; continuing without audio\n", device.c_str(),
                     av_error(read).c_str());
      }
      return;
    }
    const auto arrived = Clock::now();

    const int channels = codec->ch_layout.nb_channels;
    const int samples = captured->size / (2 * channels);
    void* planes[1] = {captured->data};
    av_audio_fifo_write(fifo.get(), planes, samples);
    av_packet_unref(captured.get());

    // The newest buffered sample was captured roughly now; compare where the
    // sample count says it is with where the clock says it is.
    const std::int64_t wall =
        std::chrono::duration_cast<std::chrono::microseconds>(arrived - epoch).count() *
        kSampleRate / 1'000'000;
    const std::int64_t buffered = av_audio_fifo_size(fifo.get());
    if (next_pts < 0) {
      next_pts = std::max<std::int64_t>(0, wall - buffered);
    } else {
      const std::int64_t drift = next_pts + buffered - wall;
      // Behind: skip forward (a short gap). Ahead: drop samples rather than
      // step pts backwards, which the muxer would reject.
      if (drift < -kMaxDriftSamples) next_pts -= drift;
      if (drift > kMaxDriftSamples) {
        av_audio_fifo_drain(fifo.get(), static_cast<int>(std::min(drift, buffered)));
      }
    }

    while (av_audio_fifo_size(fifo.get()) >= frame_size) {
      if (av_frame_make_writable(frame.get()) < 0) return;
      av_audio_fifo_read(fifo.get(), reinterpret_cast<void**>(frame->data), frame_size);
      frame->pts = next_pts;
      next_pts += frame_size;

      if (const auto ok = encode(frame.get(), write); !ok) {
        if (!failing) std::fprintf(stderr, "audio: %s\n", to_string(ok.error()).c_str());
        failing = true;
      } else {
        failing = false;
      }
    }
  }
}

} // namespace capture_eye
