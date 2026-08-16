#include "model_store.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>

using namespace capture_eye;

namespace {

// A scratch directory that removes itself, so tests leave nothing behind.
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    path = std::filesystem::temp_directory_path() /
           std::format("capture-eye-test-{}", static_cast<const void*>(this));
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out{path, std::ios::binary};
  out << contents;
}

} // namespace

TEST_CASE("the default variant is pinned and resolves to the HuggingFace URL") {
  ModelConfig config;
  const auto spec = resolve_spec(config);
  REQUIRE(spec.has_value());
  CHECK(spec->url ==
        "https://huggingface.co/onnx-community/yolo26n-ONNX/resolve/main/onnx/model.onnx");
  CHECK(spec->sha256.size() == 64);
}

TEST_CASE("an unpinned variant is refused unless explicitly allowed") {
  ModelConfig config;
  config.variant = "model_int8";

  const auto refused = resolve_spec(config);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().code == ErrorCode::config_invalid);

  config.allow_unpinned = true;
  const auto allowed = resolve_spec(config);
  REQUIRE(allowed.has_value());
  CHECK(allowed->sha256.empty());
  CHECK(allowed->url.ends_with("model_int8.onnx"));

  // An explicit hash pins it just as well as a compiled-in one.
  config.allow_unpinned = false;
  config.sha256 = std::string(64, 'a');
  const auto explicit_hash = resolve_spec(config);
  REQUIRE(explicit_hash.has_value());
  CHECK(explicit_hash->sha256 == std::string(64, 'a'));
}

TEST_CASE("the cache filename carries the hash so a re-pin cannot collide") {
  const ModelSpec a{"model", "http://x", std::string(64, 'a')};
  const ModelSpec b{"model", "http://x", std::string(64, 'b')};
  CHECK(cache_path("/cache", a) != cache_path("/cache", b));
  CHECK(cache_path("/cache", a).parent_path() == std::filesystem::path{"/cache"} / "models");
}

TEST_CASE("sha256_file matches a known digest") {
  const TempDir dir;
  const auto file = dir.path / "empty";
  write_file(file, "");
  const auto digest = sha256_file(file);
  REQUIRE(digest.has_value());
  // The well-known sha256 of zero bytes.
  CHECK(*digest == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  write_file(file, "abc");
  const auto abc = sha256_file(file);
  REQUIRE(abc.has_value());
  CHECK(*abc == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256_file reports a missing file rather than returning a digest") {
  const auto digest = sha256_file("/nonexistent/model.onnx");
  REQUIRE_FALSE(digest.has_value());
}

TEST_CASE("an explicit --model path is used as-is") {
  const TempDir dir;
  const auto file = dir.path / "custom.onnx";
  write_file(file, "not really a model");

  ModelConfig config;
  config.path = file;
  const auto resolved = ensure_model(config, dir.path / "cache");
  REQUIRE(resolved.has_value());
  CHECK(*resolved == file);
}

TEST_CASE("a missing --model path fails instead of falling back to a download") {
  const TempDir dir;
  ModelConfig config;
  config.path = dir.path / "absent.onnx";
  const auto resolved = ensure_model(config, dir.path / "cache");
  REQUIRE_FALSE(resolved.has_value());
  CHECK(resolved.error().code == ErrorCode::model_load_failed);
}

TEST_CASE("a warm cache is returned without touching the network") {
  const TempDir dir;
  const auto cache_root = dir.path / "cache";

  ModelConfig config;
  config.variant = "test";
  config.url = "http://invalid.invalid/model.onnx";  // reaching this would hang or fail
  config.sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  config.offline = true;

  const auto spec = resolve_spec(config);
  REQUIRE(spec.has_value());
  write_file(cache_path(cache_root, *spec), "abc");

  const auto resolved = ensure_model(config, cache_root);
  REQUIRE(resolved.has_value());
  CHECK(*resolved == cache_path(cache_root, *spec));
}

TEST_CASE("a corrupt cached file is rejected, not loaded") {
  const TempDir dir;
  const auto cache_root = dir.path / "cache";

  ModelConfig config;
  config.variant = "test";
  config.url = "http://invalid.invalid/model.onnx";
  config.sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  config.offline = true;

  const auto spec = resolve_spec(config);
  REQUIRE(spec.has_value());
  write_file(cache_path(cache_root, *spec), "abd");  // one byte off

  // Offline, so it cannot refetch — the point is that it refuses the bad file
  // rather than handing it to the model loader.
  const auto resolved = ensure_model(config, cache_root);
  REQUIRE_FALSE(resolved.has_value());
  CHECK(resolved.error().code == ErrorCode::model_fetch_failed);
}

TEST_CASE("offline with a cold cache is an error, not a download") {
  const TempDir dir;
  ModelConfig config;
  config.offline = true;
  const auto resolved = ensure_model(config, dir.path / "cache");
  REQUIRE_FALSE(resolved.has_value());
  CHECK(resolved.error().code == ErrorCode::model_fetch_failed);
}
