#include <opencv2/highgui.hpp>

#include <cstdio>
#include <memory>

#include "frame_mat.h"
#include "frame_sink.h"

namespace capture_eye {
namespace {

constexpr const char* kWindowName = "capture-eye";

} // namespace

Result<FrameSink> make_preview_sink(const SinkConfig& /*config*/) {
  // HighGUI wants every call on one thread; the overlay stage is that thread.
  struct Window {
    Window() { cv::namedWindow(kWindowName, cv::WINDOW_AUTOSIZE); }
    ~Window() { cv::destroyWindow(kWindowName); }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
  };
  auto window = std::make_shared<Window>();

  FrameSink sink;
  sink.name = "preview";
  sink.submit = [window](const AnnotatedFrame& annotated) -> Result<void> {
    // HighGUI throws when there is no display; on a headless machine that must
    // be an error, not a crash.
    try {
      cv::Mat image = mat_for(annotated.image);
      cv::imshow(kWindowName, image);
      cv::waitKey(1);  // required for the window to actually repaint
    } catch (const cv::Exception& error) {
      return fail(ErrorCode::sink_failed, error.what());
    }
    return {};
  };
  return sink;
}

} // namespace capture_eye
