#pragma once

#include <filesystem>

#include "config.h"
#include "error.h"

namespace capture_eye {

// Reads a JSON config file into an overlay — the file layer of the
// defaults < --config FILE < flags precedence (see config.h's merge()).
// The only translation unit that includes nlohmann/json; everything else in
// the codebase stays JSON-library-agnostic.
//
// Strict: an unrecognized key at any level is a hard error, never ignored — a
// typo'd key that silently does nothing is the worst failure mode a config
// file can have. A key present with the wrong JSON type is likewise an error,
// never a silent coercion.
[[nodiscard]] Result<ConfigOverlay> load_config_file(const std::filesystem::path& path);

} // namespace capture_eye
