#include "frame_pool.h"

namespace capture_eye {

FramePool::FramePool(std::size_t count, int width, int height) {
  const std::size_t size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
  slabs_.resize(count);
  free_.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    slabs_[i].bytes.resize(size);
    slabs_[i].frame.width = width;
    slabs_[i].frame.height = height;
    slabs_[i].frame.format = PixelFormat::bgr24;
    slabs_[i].frame.bytes = std::span{slabs_[i].bytes};
    free_.push_back(static_cast<std::uint32_t>(i));
  }
}

std::optional<FrameLease> FramePool::acquire() {
  const std::scoped_lock lock{mutex_};
  if (free_.empty()) return std::nullopt;
  const std::uint32_t index = free_.back();
  free_.pop_back();
  return FrameLease{this, index};
}

void FramePool::release(std::uint32_t index) {
  const std::scoped_lock lock{mutex_};
  free_.push_back(index);
}

const Frame& FrameLease::frame() const { return pool_->slabs_[index_].frame; }

Frame& FrameLease::frame() { return pool_->slabs_[index_].frame; }

void FrameLease::release() {
  if (pool_ != nullptr) {
    pool_->release(index_);
    pool_ = nullptr;
  }
}

} // namespace capture_eye
