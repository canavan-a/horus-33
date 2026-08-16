#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "config.h"
#include "error.h"

namespace capture_eye {

// Where a model comes from and how we know we got the right one.
struct ModelSpec {
  std::string variant;
  std::string url;
  std::string sha256;  // empty = unpinned; only usable with allow_unpinned
};

// Resolves a ModelConfig to a concrete spec, filling in the default URL and the
// pinned hash for known variants. Pure — no network, no filesystem.
[[nodiscard]] Result<ModelSpec> resolve_spec(const ModelConfig& config);

// The cache location for a spec. Pure given cache_root; the hash prefix in the
// filename means a changed pin lands in a different file rather than colliding.
[[nodiscard]] std::filesystem::path cache_path(const std::filesystem::path& cache_root,
                                               const ModelSpec& spec);

// $XDG_CACHE_HOME/capture-eye, falling back to $HOME/.cache/capture-eye.
[[nodiscard]] Result<std::filesystem::path> default_cache_root();

// Ensures a verified model file exists locally and returns its path.
//
// Downloads only if the cache is cold. Writes to a .part sibling and renames
// into place after the hash checks out, so an interrupted download can never be
// mistaken for a good model on the next run.
[[nodiscard]] Result<std::filesystem::path> ensure_model(const ModelConfig& config,
                                                         const std::filesystem::path& cache_root);

// Lowercase hex sha256 of a file's contents.
[[nodiscard]] Result<std::string> sha256_file(const std::filesystem::path& path);

} // namespace capture_eye
