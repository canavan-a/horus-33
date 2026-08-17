#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
}

#include <cstdint>
#include <chrono>
#include <memory>
#include <string>

#include "config.h"
#include "error.h"
#include "frame_sink.h"

struct SwsContext;  // libswscale — only h264_encoder.cpp needs the real type

namespace capture_eye {

// Shared between h264_sink.cpp (RTSP egress) and clip_sink.cpp (MP4 files) —
// both encode the same annotated BGR24 frames to H.264 with the same
// codec/scaler/VAAPI setup, and only differ in the muxer they write into.
// Split into these pieces so neither duplicates the libav RAII/VAAPI dance.

[[nodiscard]] std::string av_error(int code);

struct FormatDeleter {
  void operator()(AVFormatContext* ctx) const;
};
struct CodecDeleter {
  void operator()(AVCodecContext* ctx) const;
};
struct FrameDeleter {
  void operator()(AVFrame* frame) const;
};
struct PacketDeleter {
  void operator()(AVPacket* packet) const;
};
struct SwsDeleter {
  void operator()(SwsContext* ctx) const;
};
struct BufferDeleter {
  void operator()(AVBufferRef* ref) const;
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
  bool needs_avio_close = false;  // true for a file muxer we opened ourselves
  int width = 0;
  int height = 0;
  std::string encoder_name;

  std::chrono::steady_clock::time_point first_frame{};
  bool started = false;

  ~Encoder();

  [[nodiscard]] Result<void> encode(const AnnotatedFrame& annotated);
};

// Sets up hardware encoding. Returns an error rather than aborting so the caller
// can fall back to software.
[[nodiscard]] Result<void> init_vaapi(Encoder& encoder, const std::filesystem::path& vaapi_device);

// Everything transport-agnostic: codec selection/open, the scaler, the frame
// buffers, VAAPI upload surfaces. Leaves `encoder.format`/`encoder.stream`/
// header-writing to the caller, since that is the one part that differs
// between an RTSP connection and an MP4 file.
[[nodiscard]] Result<void> configure_codec(Encoder& encoder, int width, int height, int fps,
                                           int bitrate_kbps, bool hardware_encode,
                                           const std::filesystem::path& vaapi_device);

} // namespace capture_eye
