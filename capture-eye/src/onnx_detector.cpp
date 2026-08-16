#include "onnx_detector.h"

#include <onnxruntime_cxx_api.h>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <thread>
#include <vector>

#include "frame_mat.h"
#include "letterbox.h"
#include "yolo_output.h"

namespace capture_eye {
namespace {

// Ultralytics pads with this grey; a different value measurably shifts results.
constexpr double kPadValue = 114.0;

[[nodiscard]] std::string shape_string(const std::vector<std::int64_t>& shape) {
  std::string out = "[";
  for (std::size_t i = 0; i < shape.size(); ++i) {
    if (i > 0) out += ", ";
    out += shape[i] < 0 ? std::string{"dynamic"} : std::to_string(shape[i]);
  }
  return out + "]";
}

// A dimension matches if it is what we need or dynamic.
[[nodiscard]] bool dim_is(std::int64_t actual, std::int64_t expected) {
  return actual < 0 || actual == expected;
}

struct Session {
  // Declaration order is lifetime order: the environment must outlive the
  // session that borrows it.
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "capture-eye"};
  Ort::SessionOptions options;
  std::unique_ptr<Ort::Session> session;

  std::string input_name;
  std::array<std::string, 2> output_names;
  std::size_t logits_index = 0;  // which output is which, resolved by shape
  std::size_t boxes_index = 1;

  int input_size = 640;
  std::size_t queries = 0;
  std::size_t classes = 0;
  float threshold = 0.35f;

  // Reused across frames; nothing here is allocated in steady state.
  std::vector<float> input_tensor;
  cv::Mat canvas;    // padded 8UC3 BGR
  cv::Mat rgb;       // 8UC3 RGB
  cv::Mat floats;    // 32FC3 scaled
  std::array<cv::Mat, 3> planes;  // views onto input_tensor
  LetterboxTransform transform;
  int prepared_width = 0;
  int prepared_height = 0;

  void prepare_for(int width, int height);
  void fill_input(const Frame& frame);
};

void Session::prepare_for(int width, int height) {
  if (width == prepared_width && height == prepared_height) return;

  transform = letterbox_transform(width, height, input_size);
  canvas.create(input_size, input_size, CV_8UC3);
  canvas.setTo(cv::Scalar::all(kPadValue));
  prepared_width = width;
  prepared_height = height;
}

void Session::fill_input(const Frame& frame) {
  prepare_for(frame.width, frame.height);

  // Resize straight into the padded canvas's centre region, so the pad and the
  // resize are one pass rather than a resize followed by copyMakeBorder.
  const cv::Rect roi{transform.pad_x, transform.pad_y, transform.scaled_width,
                     transform.scaled_height};
  cv::Mat destination = canvas(roi);
  cv::resize(mat_for(frame), destination, destination.size(), 0, 0, cv::INTER_LINEAR);

  // Frames are BGR; the model was exported expecting RGB.
  cv::cvtColor(canvas, rgb, cv::COLOR_BGR2RGB);
  rgb.convertTo(floats, CV_32FC3, 1.0 / 255.0);

  // Split writes the three channel planes directly into the tensor buffer, so
  // the HWC-to-CHW transpose costs no extra copy.
  cv::split(floats, planes.data());
}

} // namespace

Result<std::string> describe_model_io(const std::filesystem::path& model) {
  try {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "capture-eye-inspect"};
    Ort::SessionOptions options;
    Ort::Session session{env, model.c_str(), options};
    Ort::AllocatorWithDefaultOptions allocator;

    std::string out = std::format("{}\n", model.string());
    for (std::size_t i = 0; i < session.GetInputCount(); ++i) {
      const auto name = session.GetInputNameAllocated(i, allocator);
      const auto shape = session.GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
      out += std::format("  input  {}: {} {}\n", i, name.get(), shape_string(shape));
    }
    for (std::size_t i = 0; i < session.GetOutputCount(); ++i) {
      const auto name = session.GetOutputNameAllocated(i, allocator);
      const auto shape = session.GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
      out += std::format("  output {}: {} {}\n", i, name.get(), shape_string(shape));
    }
    return out;
  } catch (const Ort::Exception& error) {
    return fail(ErrorCode::model_load_failed, error.what());
  }
}

Result<Detector> make_onnx_detector(const InferenceConfig& config,
                                    const std::filesystem::path& model) {
  auto state = std::make_shared<Session>();
  state->input_size = config.input_size;
  state->threshold = config.conf_threshold;

  try {
    // ORT's intra-op pool spin-waits between operators by default. On a busy
    // machine that starves the capture thread, and the camera starts dropping
    // frames for reasons that look nothing like an inference problem.
    state->options.SetIntraOpNumThreads(config.intra_op_threads);
    state->options.SetInterOpNumThreads(config.inter_op_threads);
    state->options.SetExecutionMode(ORT_SEQUENTIAL);
    state->options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    state->options.AddConfigEntry("session.intra_op.allow_spinning", "0");

    state->session = std::make_unique<Ort::Session>(state->env, model.c_str(), state->options);

    Ort::AllocatorWithDefaultOptions allocator;

    if (state->session->GetInputCount() != 1) {
      return fail(ErrorCode::model_shape_unexpected,
                  std::format("expected 1 input, found {}", state->session->GetInputCount()));
    }
    if (state->session->GetOutputCount() != 2) {
      return fail(ErrorCode::model_shape_unexpected,
                  std::format("expected 2 outputs (logits, boxes), found {}",
                              state->session->GetOutputCount()));
    }

    state->input_name = state->session->GetInputNameAllocated(0, allocator).get();
    const auto input_shape =
        state->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    if (input_shape.size() != 4 || !dim_is(input_shape[1], 3) ||
        !dim_is(input_shape[2], config.input_size) || !dim_is(input_shape[3], config.input_size)) {
      return fail(ErrorCode::model_shape_unexpected,
                  std::format("input is {}, expected [batch, 3, {}, {}]",
                              shape_string(input_shape), config.input_size, config.input_size));
    }

    // Resolve outputs by shape rather than by position: the last dimension of 4
    // identifies boxes, and anything else is the class scores. Depending on
    // export order would be a silent failure if a future export reorders them.
    for (std::size_t i = 0; i < 2; ++i) {
      state->output_names[i] = state->session->GetOutputNameAllocated(i, allocator).get();
      const auto shape =
          state->session->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
      if (shape.size() != 3) {
        return fail(ErrorCode::model_shape_unexpected,
                    std::format("output {} is {}, expected 3 dimensions", i, shape_string(shape)));
      }
      if (shape[2] == 4) {
        state->boxes_index = i;
      } else {
        state->logits_index = i;
        state->classes = static_cast<std::size_t>(shape[2]);
      }
      state->queries = static_cast<std::size_t>(shape[1]);
    }

    if (state->boxes_index == state->logits_index) {
      return fail(ErrorCode::model_shape_unexpected,
                  "could not tell the boxes output from the scores output");
    }
    if (state->classes <= static_cast<std::size_t>(kPersonClass)) {
      return fail(ErrorCode::model_shape_unexpected,
                  std::format("model scores {} classes, which does not include class {}",
                              state->classes, kPersonClass));
    }

    const std::size_t elements = static_cast<std::size_t>(config.input_size) *
                                 static_cast<std::size_t>(config.input_size) * 3;
    state->input_tensor.resize(elements);
    const std::size_t plane = static_cast<std::size_t>(config.input_size) *
                              static_cast<std::size_t>(config.input_size);
    for (std::size_t c = 0; c < 3; ++c) {
      state->planes[c] = cv::Mat{config.input_size, config.input_size, CV_32FC1,
                                 state->input_tensor.data() + c * plane};
    }
  } catch (const Ort::Exception& error) {
    return fail(ErrorCode::model_load_failed, error.what());
  }

  Detector detector;
  detector.name = "onnx";
  detector.detect = [state](const Frame& frame) -> Result<std::vector<Detection>> {
    try {
      state->fill_input(frame);

      const auto memory =
          Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
      const std::array<std::int64_t, 4> shape{1, 3, state->input_size, state->input_size};
      auto input = Ort::Value::CreateTensor<float>(memory, state->input_tensor.data(),
                                                   state->input_tensor.size(), shape.data(),
                                                   shape.size());

      const std::array<const char*, 1> input_names{state->input_name.c_str()};
      const std::array<const char*, 2> output_names{state->output_names[0].c_str(),
                                                    state->output_names[1].c_str()};

      auto outputs = state->session->Run(Ort::RunOptions{nullptr}, input_names.data(), &input, 1,
                                         output_names.data(), output_names.size());

      const float* logits = outputs[state->logits_index].GetTensorData<float>();
      const float* boxes = outputs[state->boxes_index].GetTensorData<float>();

      return parse_yolo_output(
          std::span{logits, state->queries * state->classes},
          std::span{boxes, state->queries * 4}, state->queries, state->classes, state->transform,
          state->threshold, kPersonClass);
    } catch (const Ort::Exception& error) {
      // ORT is the one dependency that throws; the exception stops here and
      // becomes a Result like everything else in the pipeline.
      return fail(ErrorCode::inference_failed, error.what());
    }
  };
  return detector;
}

} // namespace capture_eye
