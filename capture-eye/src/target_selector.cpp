#include "target_selector.h"

#include <algorithm>

namespace capture_eye {
namespace {

[[nodiscard]] const Detection* best_by(std::span<const Detection> people, auto&& score) {
  const Detection* best = nullptr;
  float best_score = 0;
  for (const auto& person : people) {
    const float value = score(person);
    if (best == nullptr || value > best_score) {
      best = &person;
      best_score = value;
    }
  }
  return best;
}

} // namespace

std::optional<Detection> TargetSelector::select(std::span<const Detection> people) {
  if (people.empty()) return std::nullopt;

  const Detection* chosen = nullptr;

  switch (policy) {
    case TargetPolicy::most_confident:
      chosen = best_by(people, [](const Detection& d) { return d.confidence; });
      break;

    case TargetPolicy::largest_area:
      chosen = best_by(people, [](const Detection& d) { return d.box.area(); });
      break;

    case TargetPolicy::nearest_center:
      // Boxes are in pixels, so "nearest centre" is relative to the widest box
      // we can see; comparing negated distance keeps best_by's "larger wins".
      chosen = best_by(people, [](const Detection& d) {
        return -(d.box.center_x() * d.box.center_x() + d.box.center_y() * d.box.center_y());
      });
      break;

    case TargetPolicy::sticky_largest: {
      if (previous.has_value()) {
        // Keep following the same person while any detection still overlaps the
        // box we were tracking.
        const Detection* continued = best_by(
            people, [this](const Detection& d) { return iou(d.box, *previous); });
        if (continued != nullptr && iou(continued->box, *previous) >= lock_iou) {
          chosen = continued;
        }
      }
      if (chosen == nullptr) {
        // No lock, or the locked target vanished: largest box is the best proxy
        // for nearest subject.
        chosen = best_by(people, [](const Detection& d) { return d.box.area(); });
      }
      break;
    }
  }

  if (chosen == nullptr) return std::nullopt;
  previous = chosen->box;
  return *chosen;
}

} // namespace capture_eye
