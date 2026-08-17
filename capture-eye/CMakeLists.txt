cmake_minimum_required(VERSION 3.28)
project(capture_eye LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CAPTURE_EYE_OPENVINO "Build the OpenVINO inference backend" OFF)
option(CAPTURE_EYE_WERROR "Treat warnings as errors" OFF)
option(CAPTURE_EYE_TSAN "Build with ThreadSanitizer instead of ASan/UBSan" OFF)

find_package(OpenCV REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Catch2 3 REQUIRED)
find_package(OpenSSL REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SERIALPORT REQUIRED IMPORTED_TARGET libserialport)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale)

# onnxruntime ships no CMake config in nixpkgs; the shell puts it on the
# default include/link paths.
find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h REQUIRED)
find_library(ONNXRUNTIME_LIB onnxruntime REQUIRED)

# Warnings and sanitizers are applied to everything we build, via an interface
# target rather than by repeating flags per target.
add_library(capture_eye_flags INTERFACE)
target_compile_options(capture_eye_flags INTERFACE -Wall -Wextra -Wpedantic)
if(CAPTURE_EYE_WERROR)
  target_compile_options(capture_eye_flags INTERFACE -Werror)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  if(CAPTURE_EYE_TSAN)
    target_compile_options(capture_eye_flags INTERFACE -fsanitize=thread)
    target_link_options(capture_eye_flags INTERFACE -fsanitize=thread)
  else()
    target_compile_options(capture_eye_flags INTERFACE -fsanitize=address,undefined)
    target_link_options(capture_eye_flags INTERFACE -fsanitize=address,undefined)
  endif()
endif()

# ---- core: pure logic + data, no camera, no serial, no model ----
# Everything here must be unit-testable on a machine with no hardware.
add_library(capture_eye_core STATIC
  src/error.cpp
  src/args.cpp
  src/config.cpp
  src/frame_pool.cpp
  src/track_message.cpp
  src/target_selector.cpp
  src/emission_policy.cpp
  src/clip_policy.cpp
  src/yolo_output.cpp
  src/metrics.cpp
)
target_include_directories(capture_eye_core PUBLIC src)
target_link_libraries(capture_eye_core PUBLIC capture_eye_flags)

# ---- edges: things that talk to the outside world ----
add_library(capture_eye_io STATIC
  src/model_store.cpp
  src/config_file.cpp
  src/v4l2_device.cpp
  src/frame_decoder.cpp
  src/fake_detector.cpp
  src/overlay.cpp
  src/preview_sink.cpp
  src/snapshot_sink.cpp
  src/h264_encoder.cpp
  src/h264_sink.cpp
  src/clip_sink.cpp
  src/clip_admin.cpp
  src/pipeline.cpp
  src/onnx_detector.cpp
  src/serial_port.cpp
  src/stdout_device_link.cpp
  src/control_relay.cpp
)
target_include_directories(capture_eye_io PUBLIC src PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
target_link_libraries(capture_eye_io
  PUBLIC capture_eye_core ${OpenCV_LIBS}
  PRIVATE OpenSSL::Crypto PkgConfig::CURL PkgConfig::SERIALPORT PkgConfig::FFMPEG
          ${ONNXRUNTIME_LIB} nlohmann_json::nlohmann_json
)

# ---- executable ----
add_executable(capture-eye src/main.cpp)
target_link_libraries(capture-eye PRIVATE capture_eye_core capture_eye_io)

if(CAPTURE_EYE_OPENVINO)
  find_package(OpenVINO REQUIRED)
  target_link_libraries(capture-eye PRIVATE openvino::runtime)
  target_compile_definitions(capture-eye PRIVATE CAPTURE_EYE_OPENVINO=1)
endif()

install(TARGETS capture-eye RUNTIME DESTINATION bin)

# ---- tests ----
enable_testing()
add_executable(capture-eye-tests
  tests/args_test.cpp
  tests/config_test.cpp
  tests/config_file_test.cpp
  tests/serial_queue_test.cpp
  tests/model_store_test.cpp
  tests/primitives_test.cpp
  tests/track_message_test.cpp
  tests/target_selector_test.cpp
  tests/emission_policy_test.cpp
  tests/letterbox_test.cpp
  tests/frame_mat_test.cpp
  tests/clip_policy_test.cpp
  tests/error_test.cpp
  tests/yolo_output_test.cpp
)
target_link_libraries(capture-eye-tests
  PRIVATE capture_eye_core capture_eye_io Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(capture-eye-tests)
