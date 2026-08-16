#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "frame.h"

namespace capture_eye {

class FramePool;

// Move-only RAII handle to one pooled frame buffer. Returns the slab to the
// pool on destruction, so no path can leak a buffer.
class FrameLease {
public:
  FrameLease() = default;
  FrameLease(FramePool* pool, std::uint32_t index) : pool_{pool}, index_{index} {}

  FrameLease(const FrameLease&) = delete;
  FrameLease& operator=(const FrameLease&) = delete;

  FrameLease(FrameLease&& other) noexcept
      : pool_{std::exchange(other.pool_, nullptr)}, index_{other.index_} {}

  FrameLease& operator=(FrameLease&& other) noexcept {
    if (this != &other) {
      release();
      pool_ = std::exchange(other.pool_, nullptr);
      index_ = other.index_;
    }
    return *this;
  }

  ~FrameLease() { release(); }

  [[nodiscard]] bool valid() const { return pool_ != nullptr; }
  [[nodiscard]] const Frame& frame() const;
  [[nodiscard]] Frame& frame();

private:
  void release();

  FramePool* pool_ = nullptr;  // non-owning
  std::uint32_t index_ = 0;
};

// Two stages read the same immutable pixels, so the lease is genuinely shared.
// One small control block per frame, versus copying several megabytes.
using FrameRef = std::shared_ptr<const FrameLease>;

// Fixed set of preallocated buffers. Nothing allocates after construction — a
// 1080p BGR frame is 6 MB and allocating one per frame at 60fps is not viable.
class FramePool {
public:
  FramePool(std::size_t count, int width, int height);

  // Never blocks. Empty means every buffer is still in flight, and the caller
  // should drop the frame rather than stall the camera.
  [[nodiscard]] std::optional<FrameLease> acquire();

  [[nodiscard]] std::size_t capacity() const { return slabs_.size(); }
  [[nodiscard]] std::size_t available() const {
    const std::scoped_lock lock{mutex_};
    return free_.size();
  }

private:
  friend class FrameLease;

  struct Slab {
    std::vector<std::byte> bytes;
    Frame frame;
  };

  void release(std::uint32_t index);

  mutable std::mutex mutex_;
  std::vector<Slab> slabs_;
  std::vector<std::uint32_t> free_;
};

} // namespace capture_eye
