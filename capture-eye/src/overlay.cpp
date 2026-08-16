#include "overlay.h"

#include <opencv2/imgproc.hpp>

#include <format>
#include <vector>

namespace capture_eye {
namespace {

const cv::Scalar kOther{180, 180, 180};    // grey: seen but not followed
const cv::Scalar kSelected{80, 220, 80};   // green: steering the gimbal
const cv::Scalar kReticle{200, 200, 200};
const cv::Scalar kText{240, 240, 240};
const cv::Scalar kShadow{20, 20, 20};

// Past this the boxes are old enough that their position is visibly wrong, and
// the readout says so rather than quietly showing stale data.
constexpr std::chrono::milliseconds kStaleAfter{250};

[[nodiscard]] cv::Rect rect_of(const BoxF& box) {
  return cv::Rect{cv::Point{static_cast<int>(box.x1), static_cast<int>(box.y1)},
                  cv::Point{static_cast<int>(box.x2), static_cast<int>(box.y2)}};
}

// Dark outline under light text, so the readout stays legible over any scene.
void draw_text(cv::Mat& image, const std::string& text, cv::Point at, double scale,
               const cv::Scalar& colour) {
  cv::putText(image, text, at, cv::FONT_HERSHEY_SIMPLEX, scale, kShadow, 3, cv::LINE_AA);
  cv::putText(image, text, at, cv::FONT_HERSHEY_SIMPLEX, scale, colour, 1, cv::LINE_AA);
}

} // namespace

void draw_detections(cv::Mat& image, std::span<const Detection> people,
                     const std::optional<Detection>& selected) {
  for (const auto& person : people) {
    // Identity by box equality rather than by overlap: the selected detection is
    // one of these, carried through from the selector itself.
    const bool is_selected = selected.has_value() &&
                             selected->box.x1 == person.box.x1 &&
                             selected->box.y1 == person.box.y1 &&
                             selected->box.x2 == person.box.x2 &&
                             selected->box.y2 == person.box.y2;
    if (is_selected) continue;  // drawn last, on top

    cv::rectangle(image, rect_of(person.box), kOther, 1);
    draw_text(image, std::format("{:.2f}", person.confidence),
              cv::Point{static_cast<int>(person.box.x1), static_cast<int>(person.box.y1) - 6},
              0.45, kOther);
  }

  if (!selected.has_value()) return;

  cv::rectangle(image, rect_of(selected->box), kSelected, 2);
  draw_text(image, std::format("target {:.2f}", selected->confidence),
            cv::Point{static_cast<int>(selected->box.x1), static_cast<int>(selected->box.y1) - 6},
            0.5, kSelected);

  const cv::Point centre{static_cast<int>(selected->box.center_x()),
                         static_cast<int>(selected->box.center_y())};
  cv::drawMarker(image, centre, kSelected, cv::MARKER_CROSS, 22, 2);
}

void draw_hud(cv::Mat& image, const HudInfo& info) {
  const cv::Point centre{image.cols / 2, image.rows / 2};

  // Frame centre: where the PID loop is trying to put the target's crosshair.
  cv::drawMarker(image, centre, kReticle, cv::MARKER_CROSS, 14, 1);
  cv::circle(image, centre, 30, kReticle, 1);

  // A line from the reticle to the target is the error vector the device is
  // being asked to null out.
  if (info.track.has_value() && !info.track->lost) {
    const cv::Point target{
        static_cast<int>((info.track->x + 1.0f) * 0.5f * static_cast<float>(image.cols)),
        static_cast<int>((1.0f - info.track->y) * 0.5f * static_cast<float>(image.rows))};
    cv::line(image, centre, target, kSelected, 1, cv::LINE_AA);
  }

  std::vector<std::string> lines;
  lines.push_back(std::format("cap {} fps   infer {} fps ({:.0f} ms)", info.capture_fps,
                              info.inference_fps, info.inference_ms));

  if (info.track.has_value() && !info.track->lost) {
    lines.push_back(std::format("track x{:+.3f} y{:+.3f} c{:.2f}", info.track->x, info.track->y,
                                info.track->c));
  } else {
    lines.push_back("track lost");
  }

  const bool stale = info.detection_age > kStaleAfter;
  lines.push_back(std::format("boxes {} ms old{}", info.detection_age.count(),
                              stale ? "  STALE" : ""));

  int y = 26;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const cv::Scalar colour = (i == 2 && stale) ? cv::Scalar{80, 80, 240} : kText;
    draw_text(image, lines[i], cv::Point{12, y}, 0.5, colour);
    y += 22;
  }
}

} // namespace capture_eye
