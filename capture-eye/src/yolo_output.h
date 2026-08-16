#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "detection.h"
#include "letterbox.h"

namespace capture_eye {

// Parses YOLO26's two-tensor output into source-frame detections.
//
// The interface was verified against the real model file rather than assumed:
//   logits     [1, queries, classes]  RAW scores, sigmoid required
//   pred_boxes [1, queries, 4]        centre/size normalised to the input canvas
//
// The head is NMS-free (one-to-one, TopK in the graph), so there is no
// suppression to do — every query is already a distinct object.
//
// Only one class column is read. For person detection that is 300 sigmoid
// evaluations per frame instead of 300x80, because a query's score for "person"
// does not depend on its scores for the other 79 classes.
//
// Queries are NOT assumed to be score-sorted: all of them are scanned.
[[nodiscard]] std::vector<Detection> parse_yolo_output(std::span<const float> logits,
                                                       std::span<const float> boxes,
                                                       std::size_t queries, std::size_t classes,
                                                       const LetterboxTransform& transform,
                                                       float confidence_threshold,
                                                       int class_id);

} // namespace capture_eye
