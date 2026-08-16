#include "frame_mat.h"

#include <catch2/catch_test_macros.hpp>

using namespace capture_eye;

namespace {

// A 2x2 BGR Mat with a distinct color in each corner, so a flip's effect on
// pixel *position* is unambiguous rather than just "the bytes changed".
[[nodiscard]] cv::Mat corner_mat() {
  // Parens, not braces: cv::Mat{2, 2, CV_8UC3} hits the N-dimensional
  // initializer_list<int> constructor instead of (rows, cols, type) — caught
  // by the resulting at<Vec3b>() assertion failure at runtime.
  cv::Mat mat(2, 2, CV_8UC3);
  mat.at<cv::Vec3b>(0, 0) = {1, 0, 0};  // top-left
  mat.at<cv::Vec3b>(0, 1) = {2, 0, 0};  // top-right
  mat.at<cv::Vec3b>(1, 0) = {3, 0, 0};  // bottom-left
  mat.at<cv::Vec3b>(1, 1) = {4, 0, 0};  // bottom-right
  return mat;
}

} // namespace

TEST_CASE("apply_flip: neither flag set is a no-op") {
  cv::Mat mat = corner_mat();
  apply_flip(mat, false, false);
  // countNonZero rejects multi-channel input directly, so compare corners by
  // value instead of diffing the whole 3-channel Mat.
  CHECK(mat.at<cv::Vec3b>(0, 0)[0] == 1);
  CHECK(mat.at<cv::Vec3b>(0, 1)[0] == 2);
  CHECK(mat.at<cv::Vec3b>(1, 0)[0] == 3);
  CHECK(mat.at<cv::Vec3b>(1, 1)[0] == 4);
}

TEST_CASE("apply_flip: horizontal mirrors left/right, top/bottom unchanged") {
  cv::Mat mat = corner_mat();
  apply_flip(mat, /*horizontal=*/true, /*vertical=*/false);
  CHECK(mat.at<cv::Vec3b>(0, 0)[0] == 2);  // was top-right
  CHECK(mat.at<cv::Vec3b>(0, 1)[0] == 1);  // was top-left
  CHECK(mat.at<cv::Vec3b>(1, 0)[0] == 4);  // was bottom-right
  CHECK(mat.at<cv::Vec3b>(1, 1)[0] == 3);  // was bottom-left
}

TEST_CASE("apply_flip: vertical flips top/bottom, left/right unchanged") {
  cv::Mat mat = corner_mat();
  apply_flip(mat, /*horizontal=*/false, /*vertical=*/true);
  CHECK(mat.at<cv::Vec3b>(0, 0)[0] == 3);  // was bottom-left
  CHECK(mat.at<cv::Vec3b>(0, 1)[0] == 4);  // was bottom-right
  CHECK(mat.at<cv::Vec3b>(1, 0)[0] == 1);  // was top-left
  CHECK(mat.at<cv::Vec3b>(1, 1)[0] == 2);  // was top-right
}

TEST_CASE("apply_flip: both flags is a 180-degree rotation") {
  cv::Mat mat = corner_mat();
  apply_flip(mat, /*horizontal=*/true, /*vertical=*/true);
  CHECK(mat.at<cv::Vec3b>(0, 0)[0] == 4);  // was bottom-right
  CHECK(mat.at<cv::Vec3b>(0, 1)[0] == 3);  // was bottom-left
  CHECK(mat.at<cv::Vec3b>(1, 0)[0] == 2);  // was top-right
  CHECK(mat.at<cv::Vec3b>(1, 1)[0] == 1);  // was top-left
}
