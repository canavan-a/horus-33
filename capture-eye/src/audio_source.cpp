#include "audio_source.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

[[nodiscard]] Clock::duration samples_to_duration(std::int64_t samples) {
  return std::chrono::duration_cast<Clock::duration>(
      std::chrono::nanoseconds{samples * 1'000'000'000 / kSampleRate});
}

[[nodiscard]] std::int64_t duration_to_samples(Clock::duration duration) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() * kSampleRate /
         1'000'000'000;
}

struct InputDeleter {
  void operator()(AVFormatContext* ctx) const { avformat_close_input(&ctx); }
};
struct FifoDeleter {
  void operator()(AVAudioFifo* fifo) const { av_audio_fifo_free(fifo); }
};

} // namespace

struct AudioSource::Impl {
  std::unique_ptr<AVFormatContext, InputDeleter> input;
  std::unique_ptr<AVCodecContext, CodecDeleter> codec;
  std::unique_ptr<AVAudioFifo, FifoDeleter> fifo;
  std::unique_ptr<AVFrame, FrameDeleter> frame;
  std::unique_ptr<AVPacket, PacketDeleter> captured;
  std::unique_ptr<AVPacket, PacketDeleter> encoded;
  std::string device;
  std::vector<Subscriber> subscribers;

  // Polled by libav's interrupt callback, so a read blocked inside ALSA
  // returns promptly on shutdown instead of waiting for the next period.
  std::atomic<bool> stopping{false};

  // Last member: destroyed (and therefore joined) before everything the
  // thread uses, subscribers included.
  std::jthread reader;

  static int interrupted(void* opaque) {
    return static_cast<Impl*>(opaque)->stopping.load(std::memory_order_relaxed) ? 1 : 0;
  }

  [[nodiscard]] Result<void> encode(AVFrame* to_encode, Clock::time_point epoch);
  void run(std::stop_token token);
};

AudioSource::AudioSource(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}

AudioSource::~AudioSource() {
  if (impl_) impl_->stopping.store(true, std::memory_order_relaxed);
}

Result<std::shared_ptr<AudioSource>> AudioSource::open(const AudioConfig& config) {
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
  // One encoder feeds several muxers (RTSP, every clip's MP4), so its setup
  // header always goes out-of-band where any of them can copy it.
  codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  if (const int ok = avcodec_open2(codec_ctx, opus, nullptr); ok < 0) {
    return fail(ErrorCode::sink_failed, std::format("open libopus: {}", av_error(ok)));
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
  return std::shared_ptr<AudioSource>{new AudioSource{std::move(impl)}};
}

Result<AVStream*> AudioSource::add_stream(AVFormatContext* muxer) const {
  AVStream* stream = avformat_new_stream(muxer, nullptr);
  if (stream == nullptr) return fail(ErrorCode::sink_failed, "cannot create audio stream");
  stream->time_base = impl_->codec->time_base;
  if (const int copied = avcodec_parameters_from_context(stream->codecpar, impl_->codec.get());
      copied < 0) {
    return fail(ErrorCode::sink_failed, std::format("audio stream parameters: {}", av_error(copied)));
  }
  return stream;
}

void AudioSource::subscribe(Subscriber subscriber) {
  impl_->subscribers.push_back(std::move(subscriber));
}

void AudioSource::start() {
  impl_->reader = std::jthread{[impl = impl_.get()](std::stop_token token) { impl->run(token); }};
}

Result<void> AudioSource::Impl::encode(AVFrame* to_encode, Clock::time_point epoch) {
  if (const int sent = avcodec_send_frame(codec.get(), to_encode); sent < 0) {
    return fail(ErrorCode::sink_failed, std::format("audio encode: {}", av_error(sent)));
  }
  for (;;) {
    const int received = avcodec_receive_packet(codec.get(), encoded.get());
    if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
    if (received < 0) {
      return fail(ErrorCode::sink_failed, std::format("audio packet: {}", av_error(received)));
    }
    AudioPacket packet{
        .data = {encoded->data, encoded->data + encoded->size},
        .captured_at = epoch + samples_to_duration(encoded->pts),
        .duration_samples = encoded->duration,
    };
    av_packet_unref(encoded.get());
    for (const auto& subscriber : subscribers) subscriber(packet);
  }
  return {};
}

void AudioSource::Impl::run(std::stop_token token) {
  // This source's own timeline; packets leave stamped in absolute steady-clock
  // time, so no consumer ever sees it.
  const auto epoch = Clock::now();
  const int frame_size = codec->frame_size;
  std::int64_t next_pts = -1;  // in samples since epoch

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
    const std::int64_t wall = duration_to_samples(arrived - epoch);
    const std::int64_t buffered = av_audio_fifo_size(fifo.get());
    if (next_pts < 0) {
      next_pts = std::max<std::int64_t>(0, wall - buffered);
    } else {
      const std::int64_t drift = next_pts + buffered - wall;
      // Behind: skip forward (a short gap). Ahead: drop samples rather than
      // step pts backwards, which every muxer downstream would reject.
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

      if (const auto ok = encode(frame.get(), epoch); !ok) {
        std::fprintf(stderr, "audio: %s\n", to_string(ok.error()).c_str());
      }
    }
  }
}

Result<void> mux_audio_packet(Encoder& muxer, const AVStream* stream, const AudioPacket& packet,
                              Clock::time_point epoch) {
  if (packet.captured_at < epoch) return {};

  std::unique_ptr<AVPacket, PacketDeleter> out{av_packet_alloc()};
  if (!out) return fail(ErrorCode::sink_failed, "cannot allocate audio packet");
  if (const int made = av_new_packet(out.get(), static_cast<int>(packet.data.size())); made < 0) {
    return fail(ErrorCode::sink_failed, std::format("audio packet: {}", av_error(made)));
  }
  std::memcpy(out->data, packet.data.data(), packet.data.size());

  out->pts = duration_to_samples(packet.captured_at - epoch);
  out->dts = out->pts;
  out->duration = packet.duration_samples;
  out->stream_index = stream->index;
  av_packet_rescale_ts(out.get(), AVRational{1, kSampleRate}, stream->time_base);
  return muxer.write(out.get());
}

} // namespace capture_eye
