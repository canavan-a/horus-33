#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace capture_eye {

enum class PixelFormat { bgr24 };

// Metadata for one captured image. The pixels live in a pool slab; `bytes` is a
// non-owning view of them and is valid only while the owning lease is alive.
struct Frame {
  std::uint64_t seq = 0;
  std::chrono::steady_clock::time_point captured_at{};
  int width = 0;
  int height = 0;
  PixelFormat format = PixelFormat::bgr24;
  std::span<std::byte> bytes;

  [[nodiscard]] std::size_t stride() const { return static_cast<std::size_t>(width) * 3; }
};

} // namespace capture_eye
