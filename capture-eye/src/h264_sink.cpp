#include "h264_sink.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <cstdio>
#include <format>
#include <memory>
#include <string>

#include "frame_mat.h"

namespace capture_eye {
namespace {

[[nodiscard]] std::string av_error(int code) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, buffer, sizeof(buffer));
  return buffer;
}

// FFmpeg hands back raw pointers with bespoke free functions; these give every
// one of them a destructor so no error path leaks.
struct FormatDeleter {
  void operator()(AVFormatContext* ctx) const {
    if (ctx == nullptr) return;
    avformat_free_context(ctx);
  }
};
struct CodecDeleter {
  void operator()(AVCodecContext* ctx) const { avcodec_free_context(&ctx); }
};
struct FrameDeleter {
  void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
struct PacketDeleter {
  void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
struct SwsDeleter {
  void operator()(SwsContext* ctx) const { sws_freeContext(ctx); }
};
struct BufferDeleter {
  void operator()(AVBufferRef* ref) const { av_buffer_unref(&ref); }
};

struct Encoder {
  std::unique_ptr<AVFormatContext, FormatDeleter> format;
  std::unique_ptr<AVCodecContext, CodecDeleter> codec;
  std::unique_ptr<AVFrame, FrameDeleter> software_frame;
  std::unique_ptr<AVFrame, FrameDeleter> hardware_frame;
  std::unique_ptr<AVPacket, PacketDeleter> packet;
  std::unique_ptr<SwsContext, SwsDeleter> scaler;
  std::unique_ptr<AVBufferRef, BufferDeleter> hw_device;
  std::unique_ptr<AVBufferRef, BufferDeleter> hw_frames;

  AVStream* stream = nullptr;  // owned by `format`
  bool header_written = false;
  bool hardware = false;
  int width = 0;
  int height = 0;
  std::string encoder_name;

  std::chrono::steady_clock::time_point first_frame{};
  bool started = false;

  ~Encoder() {
    // A trailer is what makes the recording valid for anything that stores it;
    // skipping it on shutdown leaves the far end with a truncated stream.
    if (header_written && format) av_write_trailer(format.get());
    if (format && (format->oformat->flags & AVFMT_NOFILE) == 0 && format->pb != nullptr) {
      avio_closep(&format->pb);
    }
  }

  [[nodiscard]] Result<void> encode(const AnnotatedFrame& annotated);
};

// Sets up hardware encoding. Returns an error rather than aborting so the caller
// can fall back to software.
[[nodiscard]] Result<void> init_vaapi(Encoder& encoder, const SinkConfig& config) {
  AVBufferRef* device = nullptr;
  const int opened = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI,
                                            config.vaapi_device.c_str(), nullptr, 0);
  if (opened < 0) {
    return fail(ErrorCode::sink_failed,
                std::format("vaapi {}: {}", config.vaapi_device.string(), av_error(opened)));
  }
  encoder.hw_device.reset(device);

  AVBufferRef* frames = av_hwframe_ctx_alloc(device);
  if (frames == nullptr) {
    return fail(ErrorCode::sink_failed, "cannot allocate vaapi frame context");
  }
  encoder.hw_frames.reset(frames);

  auto* frames_ctx = reinterpret_cast<AVHWFramesContext*>(frames->data);
  frames_ctx->format = AV_PIX_FMT_VAAPI;
  frames_ctx->sw_format = AV_PIX_FMT_NV12;
  frames_ctx->width = encoder.width;
  frames_ctx->height = encoder.height;
  frames_ctx->initial_pool_size = 8;

  const int initialised = av_hwframe_ctx_init(frames);
  if (initialised < 0) {
    return fail(ErrorCode::sink_failed, std::format("vaapi frames: {}", av_error(initialised)));
  }
  return {};
}

Result<void> Encoder::encode(const AnnotatedFrame& annotated) {
  if (!started) {
    first_frame = annotated.captured_at;
    started = true;
  }

  // Convert the annotated BGR frame into the encoder's pixel format.
  const cv::Mat image = mat_for(annotated.image);
  const std::uint8_t* source_planes[1] = {image.data};
  const int source_stride[1] = {static_cast<int>(image.step)};

  if (const int scaled = sws_scale(scaler.get(), source_planes, source_stride, 0, height,
                                   software_frame->data, software_frame->linesize);
      scaled <= 0) {
    return fail(ErrorCode::sink_failed, "colour conversion produced no rows");
  }

  // Timestamps come from capture time, not a frame counter: the camera's real
  // cadence jitters, and a counter would slowly drift against wall clock.
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      annotated.captured_at - first_frame);
  const std::int64_t pts = elapsed.count();

  AVFrame* to_encode = software_frame.get();
  if (hardware) {
    if (const int transferred = av_hwframe_transfer_data(hardware_frame.get(),
                                                         software_frame.get(), 0);
        transferred < 0) {
      return fail(ErrorCode::sink_failed, std::format("vaapi upload: {}", av_error(transferred)));
    }
    to_encode = hardware_frame.get();
  }
  to_encode->pts = pts;

  if (const int sent = avcodec_send_frame(codec.get(), to_encode); sent < 0) {
    return fail(ErrorCode::sink_failed, std::format("encode: {}", av_error(sent)));
  }

  for (;;) {
    const int received = avcodec_receive_packet(codec.get(), packet.get());
    if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
    if (received < 0) {
      return fail(ErrorCode::sink_failed, std::format("receive packet: {}", av_error(received)));
    }

    av_packet_rescale_ts(packet.get(), codec->time_base, stream->time_base);
    packet->stream_index = stream->index;

    const int written = av_interleaved_write_frame(format.get(), packet.get());
    av_packet_unref(packet.get());
    if (written < 0) {
      return fail(ErrorCode::sink_failed, std::format("write: {}", av_error(written)));
    }
  }
  return {};
}

[[nodiscard]] Result<std::shared_ptr<Encoder>> build(const SinkConfig& config, int width,
                                                     int height, int fps) {
  auto encoder = std::make_shared<Encoder>();
  encoder->width = width;
  encoder->height = height;

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

  encoder->hardware = config.hardware_encode;
  if (encoder->hardware) {
    if (const auto vaapi = init_vaapi(*encoder, config); !vaapi) {
      std::fprintf(stderr, "h264: %s; falling back to software encoding\n",
                   to_string(vaapi.error()).c_str());
      encoder->hardware = false;
      encoder->hw_frames.reset();
      encoder->hw_device.reset();
    }
  }

  const AVCodec* codec =
      avcodec_find_encoder_by_name(encoder->hardware ? "h264_vaapi" : "libx264");
  if (codec == nullptr && encoder->hardware) {
    encoder->hardware = false;
    codec = avcodec_find_encoder_by_name("libx264");
  }
  if (codec == nullptr) {
    return fail(ErrorCode::sink_failed, "no H.264 encoder available");
  }
  encoder->encoder_name = codec->name;

  AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
  if (codec_ctx == nullptr) {
    return fail(ErrorCode::sink_failed, "cannot allocate encoder");
  }
  encoder->codec.reset(codec_ctx);

  codec_ctx->width = width;
  codec_ctx->height = height;
  // Milliseconds: fine enough for 60fps and it matches the capture timestamps.
  codec_ctx->time_base = AVRational{1, 1000};
  codec_ctx->framerate = AVRational{fps, 1};
  codec_ctx->bit_rate = static_cast<std::int64_t>(config.bitrate_kbps) * 1000;
  // One keyframe per second, so a browser joining mid-stream starts quickly.
  codec_ctx->gop_size = fps;
  codec_ctx->max_b_frames = 0;  // B-frames add latency for no benefit here
  codec_ctx->pix_fmt = encoder->hardware ? AV_PIX_FMT_VAAPI : AV_PIX_FMT_YUV420P;

  if (encoder->hardware) {
    codec_ctx->hw_frames_ctx = av_buffer_ref(encoder->hw_frames.get());
  } else {
    av_opt_set(codec_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(codec_ctx->priv_data, "tune", "zerolatency", 0);
  }
  if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
    codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  if (const int opened = avcodec_open2(codec_ctx, codec, nullptr); opened < 0) {
    return fail(ErrorCode::sink_failed,
                std::format("open {}: {}", codec->name, av_error(opened)));
  }

  encoder->stream = avformat_new_stream(format, nullptr);
  if (encoder->stream == nullptr) {
    return fail(ErrorCode::sink_failed, "cannot create output stream");
  }
  encoder->stream->time_base = codec_ctx->time_base;
  if (const int copied = avcodec_parameters_from_context(encoder->stream->codecpar, codec_ctx);
      copied < 0) {
    return fail(ErrorCode::sink_failed, std::format("stream parameters: {}", av_error(copied)));
  }

  // The scaler always targets the software pixel format; the hardware path
  // uploads that to a VAAPI surface afterwards.
  const AVPixelFormat software_format =
      encoder->hardware ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
  encoder->scaler.reset(sws_getContext(width, height, AV_PIX_FMT_BGR24, width, height,
                                       software_format, SWS_BILINEAR, nullptr, nullptr,
                                       nullptr));
  if (!encoder->scaler) {
    return fail(ErrorCode::sink_failed, "cannot create colour converter");
  }

  encoder->software_frame.reset(av_frame_alloc());
  if (!encoder->software_frame) return fail(ErrorCode::sink_failed, "cannot allocate frame");
  encoder->software_frame->format = software_format;
  encoder->software_frame->width = width;
  encoder->software_frame->height = height;
  if (const int buffered = av_frame_get_buffer(encoder->software_frame.get(), 0); buffered < 0) {
    return fail(ErrorCode::sink_failed, std::format("frame buffer: {}", av_error(buffered)));
  }

  if (encoder->hardware) {
    encoder->hardware_frame.reset(av_frame_alloc());
    if (!encoder->hardware_frame) return fail(ErrorCode::sink_failed, "cannot allocate surface");
    if (const int got =
            av_hwframe_get_buffer(encoder->hw_frames.get(), encoder->hardware_frame.get(), 0);
        got < 0) {
      return fail(ErrorCode::sink_failed, std::format("vaapi surface: {}", av_error(got)));
    }
  }

  encoder->packet.reset(av_packet_alloc());
  if (!encoder->packet) return fail(ErrorCode::sink_failed, "cannot allocate packet");

  if (const int header = avformat_write_header(format, nullptr); header < 0) {
    return fail(ErrorCode::sink_failed,
                std::format("connect {}: {}", config.rtsp_url, av_error(header)));
  }
  encoder->header_written = true;

  std::fprintf(stderr, "h264: %s %dx%d @%dfps %dkbps -> %s\n", encoder->encoder_name.c_str(),
               width, height, fps, config.bitrate_kbps, config.rtsp_url.c_str());
  return encoder;
}

} // namespace

Result<FrameSink> make_h264_rtsp_sink(const SinkConfig& config, int width, int height, int fps) {
  if (config.rtsp_url.empty()) {
    return fail(ErrorCode::config_invalid, "rtsp sink needs a url");
  }
  avformat_network_init();

  auto encoder = build(config, width, height, fps);
  if (!encoder) return std::unexpected(encoder.error());

  FrameSink sink;
  sink.name = "h264";
  sink.submit = [state = *encoder](const AnnotatedFrame& annotated) -> Result<void> {
    return state->encode(annotated);
  };
  return sink;
}

} // namespace capture_eye
