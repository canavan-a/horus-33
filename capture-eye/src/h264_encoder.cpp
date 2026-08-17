#include "h264_encoder.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <format>

#include "frame_mat.h"

namespace capture_eye {

std::string av_error(int code) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(code, buffer, sizeof(buffer));
  return buffer;
}

void FormatDeleter::operator()(AVFormatContext* ctx) const {
  if (ctx == nullptr) return;
  avformat_free_context(ctx);
}
void CodecDeleter::operator()(AVCodecContext* ctx) const { avcodec_free_context(&ctx); }
void FrameDeleter::operator()(AVFrame* frame) const { av_frame_free(&frame); }
void PacketDeleter::operator()(AVPacket* packet) const { av_packet_free(&packet); }
void SwsDeleter::operator()(SwsContext* ctx) const { sws_freeContext(ctx); }
void BufferDeleter::operator()(AVBufferRef* ref) const { av_buffer_unref(&ref); }

Encoder::~Encoder() {
  // A trailer is what makes the recording valid for anything that stores it;
  // skipping it on shutdown leaves the far end (or the file) truncated.
  if (header_written && format) av_write_trailer(format.get());
  if (format && needs_avio_close && format->pb != nullptr) {
    avio_closep(&format->pb);
  }
}

Result<void> init_vaapi(Encoder& encoder, const std::filesystem::path& vaapi_device) {
  AVBufferRef* device = nullptr;
  const int opened = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI,
                                            vaapi_device.c_str(), nullptr, 0);
  if (opened < 0) {
    return fail(ErrorCode::sink_failed,
                std::format("vaapi {}: {}", vaapi_device.string(), av_error(opened)));
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

    // Writes the compressed packet through libav's own AVIOContext, i.e. an
    // actual write() to disk (or the network socket) right here — nothing
    // above this call buffers more than the one frame in flight.
    const int written = av_interleaved_write_frame(format.get(), packet.get());
    av_packet_unref(packet.get());
    if (written < 0) {
      return fail(ErrorCode::sink_failed, std::format("write: {}", av_error(written)));
    }
  }
  return {};
}

Result<void> configure_codec(Encoder& encoder, int width, int height, int fps, int bitrate_kbps,
                             bool hardware_encode, const std::filesystem::path& vaapi_device) {
  encoder.width = width;
  encoder.height = height;

  encoder.hardware = hardware_encode;
  if (encoder.hardware) {
    if (const auto vaapi = init_vaapi(encoder, vaapi_device); !vaapi) {
      std::fprintf(stderr, "h264: %s; falling back to software encoding\n",
                   to_string(vaapi.error()).c_str());
      encoder.hardware = false;
      encoder.hw_frames.reset();
      encoder.hw_device.reset();
    }
  }

  const AVCodec* codec = avcodec_find_encoder_by_name(encoder.hardware ? "h264_vaapi" : "libx264");
  if (codec == nullptr && encoder.hardware) {
    encoder.hardware = false;
    codec = avcodec_find_encoder_by_name("libx264");
  }
  if (codec == nullptr) {
    return fail(ErrorCode::sink_failed, "no H.264 encoder available");
  }
  encoder.encoder_name = codec->name;

  AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
  if (codec_ctx == nullptr) {
    return fail(ErrorCode::sink_failed, "cannot allocate encoder");
  }
  encoder.codec.reset(codec_ctx);

  codec_ctx->width = width;
  codec_ctx->height = height;
  // Milliseconds: fine enough for 60fps and it matches the capture timestamps.
  codec_ctx->time_base = AVRational{1, 1000};
  codec_ctx->framerate = AVRational{fps, 1};
  codec_ctx->bit_rate = static_cast<std::int64_t>(bitrate_kbps) * 1000;
  // One keyframe per second, so a viewer joining mid-stream (or seeking a
  // clip) starts quickly.
  codec_ctx->gop_size = fps;
  codec_ctx->max_b_frames = 0;  // B-frames add latency for no benefit here
  codec_ctx->pix_fmt = encoder.hardware ? AV_PIX_FMT_VAAPI : AV_PIX_FMT_YUV420P;

  if (encoder.hardware) {
    codec_ctx->hw_frames_ctx = av_buffer_ref(encoder.hw_frames.get());
  } else {
    av_opt_set(codec_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(codec_ctx->priv_data, "tune", "zerolatency", 0);
  }
  if (encoder.format && (encoder.format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
    codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  if (const int opened = avcodec_open2(codec_ctx, codec, nullptr); opened < 0) {
    return fail(ErrorCode::sink_failed, std::format("open {}: {}", codec->name, av_error(opened)));
  }

  encoder.stream = avformat_new_stream(encoder.format.get(), nullptr);
  if (encoder.stream == nullptr) {
    return fail(ErrorCode::sink_failed, "cannot create output stream");
  }
  encoder.stream->time_base = codec_ctx->time_base;
  if (const int copied = avcodec_parameters_from_context(encoder.stream->codecpar, codec_ctx);
      copied < 0) {
    return fail(ErrorCode::sink_failed, std::format("stream parameters: {}", av_error(copied)));
  }

  // The scaler always targets the software pixel format; the hardware path
  // uploads that to a VAAPI surface afterwards.
  const AVPixelFormat software_format = encoder.hardware ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
  encoder.scaler.reset(sws_getContext(width, height, AV_PIX_FMT_BGR24, width, height,
                                      software_format, SWS_BILINEAR, nullptr, nullptr, nullptr));
  if (!encoder.scaler) {
    return fail(ErrorCode::sink_failed, "cannot create colour converter");
  }

  encoder.software_frame.reset(av_frame_alloc());
  if (!encoder.software_frame) return fail(ErrorCode::sink_failed, "cannot allocate frame");
  encoder.software_frame->format = software_format;
  encoder.software_frame->width = width;
  encoder.software_frame->height = height;
  if (const int buffered = av_frame_get_buffer(encoder.software_frame.get(), 0); buffered < 0) {
    return fail(ErrorCode::sink_failed, std::format("frame buffer: {}", av_error(buffered)));
  }

  if (encoder.hardware) {
    encoder.hardware_frame.reset(av_frame_alloc());
    if (!encoder.hardware_frame) return fail(ErrorCode::sink_failed, "cannot allocate surface");
    if (const int got =
            av_hwframe_get_buffer(encoder.hw_frames.get(), encoder.hardware_frame.get(), 0);
        got < 0) {
      return fail(ErrorCode::sink_failed, std::format("vaapi surface: {}", av_error(got)));
    }
  }

  encoder.packet.reset(av_packet_alloc());
  if (!encoder.packet) return fail(ErrorCode::sink_failed, "cannot allocate packet");
  return {};
}

} // namespace capture_eye
