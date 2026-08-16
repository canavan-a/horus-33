#include "v4l2_device.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <thread>
#include <utility>

namespace capture_eye {
namespace {

// ioctl returns EINTR on any signal; retrying is the documented contract.
int retry_ioctl(int fd, unsigned long request, void* argument) {
  int result = 0;
  do {
    result = ::ioctl(fd, request, argument);
  } while (result == -1 && errno == EINTR);
  return result;
}

[[nodiscard]] std::string errno_message() { return std::strerror(errno); }

// A device just released by another process (or by capture-eye's own
// previous run, restarted quickly by systemd's Restart=always or a dev
// script) can answer STREAMON with EPROTO or EBUSY for a brief moment while
// the UVC driver settles — confirmed by hand: closing and immediately
// reopening the same camera reliably reproduced "VIDIOC_STREAMON: Protocol
// error", and the same open succeeded a couple of seconds later with no
// other change. Retrying a config error would hide a real problem; retrying
// this specific transient is what "camera_stream_failed" should mean.
constexpr int kStreamOnRetries = 10;
constexpr auto kStreamOnRetryDelay = std::chrono::milliseconds{200};

[[nodiscard]] int streamon_with_retry(int fd, v4l2_buf_type* type) {
  for (int attempt = 0; attempt < kStreamOnRetries; ++attempt) {
    const int result = retry_ioctl(fd, VIDIOC_STREAMON, type);
    if (result == 0) return 0;
    if (errno != EPROTO && errno != EBUSY) return result;  // a real error; don't mask it
    if (attempt + 1 < kStreamOnRetries) std::this_thread::sleep_for(kStreamOnRetryDelay);
  }
  return -1;
}

} // namespace

Result<std::vector<FormatInfo>> enumerate_formats(const std::filesystem::path& device) {
  const int fd = ::open(device.c_str(), O_RDWR);
  if (fd < 0) {
    return fail(ErrorCode::camera_open_failed,
                std::format("{}: {}", device.string(), errno_message()));
  }

  std::vector<FormatInfo> formats;
  for (std::uint32_t index = 0;; ++index) {
    v4l2_fmtdesc description{};
    description.index = index;
    description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (retry_ioctl(fd, VIDIOC_ENUM_FMT, &description) < 0) break;

    FormatInfo format;
    format.fourcc = description.pixelformat;
    format.description = reinterpret_cast<const char*>(description.description);

    for (std::uint32_t size_index = 0;; ++size_index) {
      v4l2_frmsizeenum size{};
      size.index = size_index;
      size.pixel_format = description.pixelformat;
      if (retry_ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) < 0) break;
      if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE) continue;

      FrameSizeInfo info;
      info.width = static_cast<int>(size.discrete.width);
      info.height = static_cast<int>(size.discrete.height);

      for (std::uint32_t rate_index = 0;; ++rate_index) {
        v4l2_frmivalenum interval{};
        interval.index = rate_index;
        interval.pixel_format = description.pixelformat;
        interval.width = size.discrete.width;
        interval.height = size.discrete.height;
        if (retry_ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) < 0) break;
        if (interval.type != V4L2_FRMIVAL_TYPE_DISCRETE) continue;
        if (interval.discrete.numerator == 0) continue;
        info.fps_options.push_back(
            static_cast<int>(interval.discrete.denominator / interval.discrete.numerator));
      }
      format.sizes.push_back(std::move(info));
    }
    formats.push_back(std::move(format));
  }

  ::close(fd);
  return formats;
}

Result<V4l2Device> V4l2Device::open(const CaptureConfig& config) {
  V4l2Device device;

  device.fd_ = ::open(config.device.c_str(), O_RDWR);
  if (device.fd_ < 0) {
    return fail(ErrorCode::camera_open_failed,
                std::format("{}: {}", config.device.string(), errno_message()));
  }

  v4l2_capability capability{};
  if (retry_ioctl(device.fd_, VIDIOC_QUERYCAP, &capability) < 0) {
    return fail(ErrorCode::camera_open_failed,
                std::format("{}: not a v4l2 device: {}", config.device.string(), errno_message()));
  }
  if ((capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
    return fail(ErrorCode::camera_open_failed,
                std::format("{}: does not support video capture", config.device.string()));
  }
  if ((capability.capabilities & V4L2_CAP_STREAMING) == 0) {
    return fail(ErrorCode::camera_open_failed,
                std::format("{}: does not support streaming i/o", config.device.string()));
  }

  v4l2_format format{};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = static_cast<__u32>(config.width);
  format.fmt.pix.height = static_cast<__u32>(config.height);
  format.fmt.pix.pixelformat = config.fourcc;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  if (retry_ioctl(device.fd_, VIDIOC_S_FMT, &format) < 0) {
    return fail(ErrorCode::camera_format_rejected,
                std::format("VIDIOC_S_FMT: {}", errno_message()));
  }

  // The driver rewrote `format` with what it actually granted. Believing the
  // request instead of the reply is the classic way to silently capture at the
  // wrong size or format.
  device.granted_.fourcc = format.fmt.pix.pixelformat;
  device.granted_.width = static_cast<int>(format.fmt.pix.width);
  device.granted_.height = static_cast<int>(format.fmt.pix.height);

  if (config.strict_format) {
    if (device.granted_.fourcc != config.fourcc) {
      return fail(ErrorCode::camera_format_rejected,
                  std::format("asked for {}, driver granted {} (pass --loose-format to accept)",
                              fourcc_string(config.fourcc),
                              fourcc_string(device.granted_.fourcc)));
    }
    if (device.granted_.width != config.width || device.granted_.height != config.height) {
      return fail(ErrorCode::camera_format_rejected,
                  std::format("asked for {}x{}, driver granted {}x{} (pass --loose-format)",
                              config.width, config.height, device.granted_.width,
                              device.granted_.height));
    }
  }

  v4l2_streamparm parameters{};
  parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  parameters.parm.capture.timeperframe.numerator = 1;
  parameters.parm.capture.timeperframe.denominator = static_cast<__u32>(config.fps);
  if (retry_ioctl(device.fd_, VIDIOC_S_PARM, &parameters) < 0) {
    return fail(ErrorCode::camera_format_rejected, std::format("VIDIOC_S_PARM: {}", errno_message()));
  }
  const auto& granted_interval = parameters.parm.capture.timeperframe;
  device.granted_.fps =
      granted_interval.numerator == 0
          ? config.fps
          : static_cast<int>(granted_interval.denominator / granted_interval.numerator);

  if (config.strict_format && device.granted_.fps != config.fps) {
    return fail(ErrorCode::camera_format_rejected,
                std::format("asked for {}fps, driver granted {}fps (pass --loose-format)",
                            config.fps, device.granted_.fps));
  }

  v4l2_requestbuffers request{};
  request.count = static_cast<__u32>(config.buffer_count);
  request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  request.memory = V4L2_MEMORY_MMAP;
  if (retry_ioctl(device.fd_, VIDIOC_REQBUFS, &request) < 0) {
    return fail(ErrorCode::camera_stream_failed, std::format("VIDIOC_REQBUFS: {}", errno_message()));
  }
  if (request.count < 2) {
    return fail(ErrorCode::camera_stream_failed,
                std::format("driver granted only {} buffers", request.count));
  }

  for (__u32 index = 0; index < request.count; ++index) {
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    if (retry_ioctl(device.fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
      return fail(ErrorCode::camera_stream_failed,
                  std::format("VIDIOC_QUERYBUF: {}", errno_message()));
    }

    void* mapped = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, device.fd_,
                          static_cast<off_t>(buffer.m.offset));
    if (mapped == MAP_FAILED) {
      return fail(ErrorCode::camera_stream_failed, std::format("mmap: {}", errno_message()));
    }
    device.buffers_.emplace_back(static_cast<std::byte*>(mapped), buffer.length);

    if (retry_ioctl(device.fd_, VIDIOC_QBUF, &buffer) < 0) {
      return fail(ErrorCode::camera_stream_failed, std::format("VIDIOC_QBUF: {}", errno_message()));
    }
  }

  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (streamon_with_retry(device.fd_, &type) < 0) {
    return fail(ErrorCode::camera_stream_failed,
                std::format("VIDIOC_STREAMON: {}", errno_message()));
  }

  return device;
}

V4l2Device::V4l2Device(V4l2Device&& other) noexcept
    : fd_{std::exchange(other.fd_, -1)},
      granted_{other.granted_},
      buffers_{std::move(other.buffers_)} {
  other.buffers_.clear();
}

V4l2Device& V4l2Device::operator=(V4l2Device&& other) noexcept {
  if (this != &other) {
    close_device();
    fd_ = std::exchange(other.fd_, -1);
    granted_ = other.granted_;
    buffers_ = std::move(other.buffers_);
    other.buffers_.clear();
  }
  return *this;
}

V4l2Device::~V4l2Device() { close_device(); }

void V4l2Device::close_device() {
  if (fd_ < 0) return;

  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  retry_ioctl(fd_, VIDIOC_STREAMOFF, &type);
  for (const auto& buffer : buffers_) {
    ::munmap(buffer.data(), buffer.size());
  }
  buffers_.clear();
  ::close(fd_);
  fd_ = -1;
}

void V4l2Device::requeue(std::uint32_t index) {
  v4l2_buffer buffer{};
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_MMAP;
  buffer.index = index;
  retry_ioctl(fd_, VIDIOC_QBUF, &buffer);
}

Result<std::optional<V4l2Device::Buffer>> V4l2Device::next(std::chrono::milliseconds timeout) {
  pollfd descriptor{.fd = fd_, .events = POLLIN, .revents = 0};
  const int ready = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
  if (ready < 0) {
    if (errno == EINTR) return std::nullopt;
    return fail(ErrorCode::camera_stream_failed, std::format("poll: {}", errno_message()));
  }
  if (ready == 0) return std::nullopt;

  v4l2_buffer buffer{};
  buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buffer.memory = V4L2_MEMORY_MMAP;
  if (retry_ioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
    if (errno == EAGAIN) return std::nullopt;
    return fail(ErrorCode::camera_stream_failed, std::format("VIDIOC_DQBUF: {}", errno_message()));
  }

  const auto region = buffers_[buffer.index];
  return Buffer{this, buffer.index, region.subspan(0, buffer.bytesused)};
}

V4l2Device::Buffer::Buffer(Buffer&& other) noexcept
    : device_{std::exchange(other.device_, nullptr)}, index_{other.index_}, data_{other.data_} {}

V4l2Device::Buffer& V4l2Device::Buffer::operator=(Buffer&& other) noexcept {
  if (this != &other) {
    if (device_ != nullptr) device_->requeue(index_);
    device_ = std::exchange(other.device_, nullptr);
    index_ = other.index_;
    data_ = other.data_;
  }
  return *this;
}

V4l2Device::Buffer::~Buffer() {
  if (device_ != nullptr) device_->requeue(index_);
}

} // namespace capture_eye
