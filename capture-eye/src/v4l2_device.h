#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "config.h"
#include "error.h"

namespace capture_eye {

struct FrameSizeInfo {
  int width = 0;
  int height = 0;
  std::vector<int> fps_options;
};

struct FormatInfo {
  std::uint32_t fourcc = 0;
  std::string description;
  std::vector<FrameSizeInfo> sizes;
};

// What the driver says it can do, for --list-formats.
[[nodiscard]] Result<std::vector<FormatInfo>> enumerate_formats(
    const std::filesystem::path& device);

// What the driver actually granted, which is not necessarily what we asked for.
struct GrantedFormat {
  std::uint32_t fourcc = 0;
  int width = 0;
  int height = 0;
  int fps = 0;
};

// A streaming V4L2 capture device using mmap'd buffers.
//
// Raw ioctls rather than cv::VideoCapture because the driver's reply to
// VIDIOC_S_FMT is the only honest answer about what we are actually capturing.
// OpenCV renegotiates silently, which is how you end up running at 5fps and
// blaming the model.
class V4l2Device {
public:
  [[nodiscard]] static Result<V4l2Device> open(const CaptureConfig& config);

  V4l2Device(const V4l2Device&) = delete;
  V4l2Device& operator=(const V4l2Device&) = delete;
  V4l2Device(V4l2Device&& other) noexcept;
  V4l2Device& operator=(V4l2Device&& other) noexcept;
  ~V4l2Device();

  // A dequeued buffer. Requeues itself on destruction — the driver only has a
  // handful, and holding one starves capture.
  class Buffer {
  public:
    Buffer() = default;
    Buffer(V4l2Device* device, std::uint32_t index, std::span<const std::byte> data)
        : device_{device}, index_{index}, data_{data} {}

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    ~Buffer();

    [[nodiscard]] std::span<const std::byte> data() const { return data_; }

  private:
    V4l2Device* device_ = nullptr;  // non-owning
    std::uint32_t index_ = 0;
    std::span<const std::byte> data_;
  };

  // Waits for the next frame. nullopt means the timeout elapsed, which is not
  // an error — the caller decides whether a quiet camera matters.
  [[nodiscard]] Result<std::optional<Buffer>> next(std::chrono::milliseconds timeout);

  [[nodiscard]] const GrantedFormat& granted() const { return granted_; }

private:
  friend class Buffer;

  V4l2Device() = default;
  void close_device();
  void requeue(std::uint32_t index);

  int fd_ = -1;
  GrantedFormat granted_;
  std::vector<std::span<std::byte>> buffers_;  // mmap'd regions we own
};

} // namespace capture_eye
