#include "frame_decoder.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <format>

namespace capture_eye {
namespace {

const std::uint32_t kMjpg = fourcc_of("MJPG");
const std::uint32_t kJpeg = fourcc_of("JPEG");
const std::uint32_t kYuyv = fourcc_of("YUYV");

[[nodiscard]] cv::Mat mat_view(Frame& frame) {
  return cv::Mat{frame.height, frame.width, CV_8UC3, frame.bytes.data()};
}

} // namespace

Result<FrameDecoder> FrameDecoder::create(std::uint32_t fourcc, int source_width,
                                          int source_height, int decode_scale) {
  if (fourcc != kMjpg && fourcc != kJpeg && fourcc != kYuyv) {
    return fail(ErrorCode::decode_failed,
                std::format("unsupported pixel format {}", fourcc_string(fourcc)));
  }
  if (fourcc == kYuyv && decode_scale != 1) {
    // Reduced decode is a JPEG feature; scaling YUYV would be a separate resize
    // and is not worth pretending to support.
    return fail(ErrorCode::decode_failed, "--decode-scale only applies to MJPG");
  }

  FrameDecoder decoder;
  decoder.fourcc_ = fourcc;
  decoder.source_width_ = source_width;
  decoder.source_height_ = source_height;
  decoder.decode_scale_ = decode_scale;
  decoder.output_width_ = source_width / decode_scale;
  decoder.output_height_ = source_height / decode_scale;
  return decoder;
}

Result<void> FrameDecoder::decode(std::span<const std::byte> encoded, Frame& destination) const {
  if (destination.width != output_width_ || destination.height != output_height_) {
    return fail(ErrorCode::decode_failed,
                std::format("destination is {}x{}, decoder produces {}x{}", destination.width,
                            destination.height, output_width_, output_height_));
  }

  cv::Mat output = mat_view(destination);

  if (fourcc_ == kYuyv) {
    const std::size_t expected =
        static_cast<std::size_t>(source_width_) * static_cast<std::size_t>(source_height_) * 2;
    if (encoded.size() < expected) {
      return fail(ErrorCode::decode_failed,
                  std::format("short YUYV frame: {} of {} bytes", encoded.size(), expected));
    }
    const cv::Mat source{source_height_, source_width_, CV_8UC2,
                         const_cast<std::byte*>(encoded.data())};
    cv::cvtColor(source, output, cv::COLOR_YUV2BGR_YUYV);
    return {};
  }

  // MJPG. IMREAD_REDUCED_* decodes at a fraction of full size for roughly a
  // fraction of the cost, which is the cheapest fps lever available.
  int flags = cv::IMREAD_COLOR;
  if (decode_scale_ == 2) flags = cv::IMREAD_REDUCED_COLOR_2;
  if (decode_scale_ == 4) flags = cv::IMREAD_REDUCED_COLOR_4;

  const cv::Mat compressed{1, static_cast<int>(encoded.size()), CV_8UC1,
                           const_cast<std::byte*>(encoded.data())};
  // The 3-argument overload decodes into `output` without allocating, provided
  // the size and type already match.
  cv::imdecode(compressed, flags, &output);
  if (output.empty()) {
    return fail(ErrorCode::decode_failed, "jpeg decode produced no image");
  }
  if (reinterpret_cast<const std::byte*>(output.data) != destination.bytes.data()) {
    return fail(ErrorCode::decode_failed,
                std::format("decoder reallocated: got {}x{}, expected {}x{}", output.cols,
                            output.rows, output_width_, output_height_));
  }
  return {};
}

} // namespace capture_eye
