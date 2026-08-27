#include "openvino_detector.h"

#include <openvino/openvino.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "yolo_input.h"
#include "yolo_output.h"

namespace capture_eye {
namespace {

[[nodiscard]] std::string shape_string(const ov::Shape& shape) {
  std::string out = "[";
  for (std::size_t i = 0; i < shape.size(); ++i) {
    if (i > 0) out += ", ";
    out += std::to_string(shape[i]);
  }
  return out + "]";
}

struct Session {
  explicit Session(int input_size_) : input{input_size_}, input_size{input_size_} {}

  // Declaration order is lifetime order, as with ORT: core owns the plugin
  // registry the compiled model and its request borrow.
  ov::Core core;
  ov::CompiledModel compiled;
  ov::InferRequest request;

  std::size_t logits_index = 0;  // which output is which, resolved by shape
  std::size_t boxes_index = 1;

  YoloInput input;
  int input_size = 640;
  std::size_t queries = 0;
  std::size_t classes = 0;
  float threshold = 0.35f;
};

} // namespace

Result<Detector> make_openvino_detector(const InferenceConfig& config,
                                        const std::filesystem::path& model) {
  auto state = std::make_shared<Session>(config.input_size);
  state->threshold = config.conf_threshold;

  try {
    // Match the ONNX backend's thread budget rather than letting OpenVINO take
    // every core: the capture thread has to keep up with the camera, and a
    // benchmark between the two backends is only meaningful at equal budgets.
    state->compiled = state->core.compile_model(
        model.string(), "CPU",
        ov::inference_num_threads(config.intra_op_threads),
        ov::num_streams(config.inter_op_threads));
    state->request = state->compiled.create_infer_request();

    const auto inputs = state->compiled.inputs();
    const auto outputs = state->compiled.outputs();
    if (inputs.size() != 1) {
      return fail(ErrorCode::model_shape_unexpected,
                  std::format("expected 1 input, found {}", inputs.size()));
    }
    if (outputs.size() != 2) {
      return fail(ErrorCode::model_shape_unexpected,
                  std::format("expected 2 outputs (logits, boxes), found {}", outputs.size()));
    }

    const ov::Shape input_shape = inputs[0].get_shape();
    if (input_shape.size() != 4 || input_shape[1] != 3 ||
        input_shape[2] != static_cast<std::size_t>(config.input_size) ||
        input_shape[3] != static_cast<std::size_t>(config.input_size)) {
      return fail(ErrorCode::model_shape_unexpected,
                  std::format("input is {}, expected [batch, 3, {}, {}]",
                              shape_string(input_shape), config.input_size, config.input_size));
    }

    // Resolve outputs by shape, not by position — same reasoning as the ONNX
    // backend: a re-export that reorders them must not silently swap meanings.
    for (std::size_t i = 0; i < 2; ++i) {
      const ov::Shape shape = outputs[i].get_shape();
      if (shape.size() != 3) {
        return fail(ErrorCode::model_shape_unexpected,
                    std::format("output {} is {}, expected 3 dimensions", i, shape_string(shape)));
      }
      if (shape[2] == 4) {
        state->boxes_index = i;
      } else {
        state->logits_index = i;
        state->classes = shape[2];
      }
      state->queries = shape[1];
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
  } catch (const ov::Exception& error) {
    return fail(ErrorCode::model_load_failed, error.what());
  }

  Detector detector;
  detector.name = "openvino";
  detector.detect = [state](const Frame& frame) -> Result<std::vector<Detection>> {
    try {
      state->input.fill(frame);

      // Wraps the preprocessing buffer rather than copying into OpenVINO's own:
      // the buffer outlives the call, and infer() is synchronous.
      const ov::Shape shape{1, 3, static_cast<std::size_t>(state->input_size),
                            static_cast<std::size_t>(state->input_size)};
      state->request.set_input_tensor(
          ov::Tensor{ov::element::f32, shape, state->input.tensor().data()});
      state->request.infer();

      const ov::Tensor logits_tensor = state->request.get_output_tensor(state->logits_index);
      const ov::Tensor boxes_tensor = state->request.get_output_tensor(state->boxes_index);

      return parse_yolo_output(
          std::span{logits_tensor.data<const float>(), state->queries * state->classes},
          std::span{boxes_tensor.data<const float>(), state->queries * 4}, state->queries,
          state->classes, state->input.transform(), state->threshold, kPersonClass);
    } catch (const ov::Exception& error) {
      // OpenVINO throws; the exception stops here and becomes a Result like
      // everything else in the pipeline.
      return fail(ErrorCode::inference_failed, error.what());
    }
  };
  return detector;
}

} // namespace capture_eye
