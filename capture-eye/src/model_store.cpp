#include "model_store.h"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <system_error>

namespace capture_eye {
namespace {

constexpr std::string_view kUrlPrefix =
    "https://huggingface.co/onnx-community/yolo26n-ONNX/resolve/main/onnx/";

// Pinned sha256 per variant. A variant with no entry here requires
// --allow-unpinned, and the program prints the hash it observed so it can be
// added. Never accept a hash we did not expect.
struct PinnedHash {
  std::string_view variant;
  std::string_view sha256;
};
constexpr std::array kPinnedHashes{
    // yolo26n fp32, 9890454 bytes. Verified by download on 2026-08-16.
    PinnedHash{"model", "cda08d9440217e243e075ee839f40383c59b3f973e493f9f6c7452922a69436e"},
};

[[nodiscard]] std::string_view pinned_hash_for(std::string_view variant) {
  for (const auto& pin : kPinnedHashes) {
    if (pin.variant == variant) return pin.sha256;
  }
  return {};
}

// libcurl and OpenSSL hand back raw pointers with matching free functions;
// these give them destructors so no path can leak them.
struct CurlDeleter {
  void operator()(CURL* handle) const { curl_easy_cleanup(handle); }
};
using CurlHandle = std::unique_ptr<CURL, CurlDeleter>;

struct EvpDeleter {
  void operator()(EVP_MD_CTX* ctx) const { EVP_MD_CTX_free(ctx); }
};
using EvpContext = std::unique_ptr<EVP_MD_CTX, EvpDeleter>;

struct FileDeleter {
  void operator()(std::FILE* file) const { std::fclose(file); }
};
using FilePtr = std::unique_ptr<std::FILE, FileDeleter>;

std::size_t write_to_file(char* data, std::size_t size, std::size_t count, void* userdata) {
  auto* file = static_cast<std::FILE*>(userdata);
  return std::fwrite(data, size, count, file);
}

// Carried through CURLOPT_XFERINFODATA rather than kept in a static, so two
// concurrent downloads could never share a counter.
struct Progress {
  curl_off_t last_decade = -1;
};

int report_progress(void* userdata, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
  auto* progress = static_cast<Progress*>(userdata);
  // The redirect hop reports a complete transfer of a zero-length body; only
  // the real payload is worth a progress line.
  if (total < 1024 * 1024) return 0;

  const curl_off_t decade = (now * 10) / total;
  if (decade == progress->last_decade) return 0;
  progress->last_decade = decade;
  std::fprintf(stderr, "\rfetching model: %3lld%% of %lld MB", static_cast<long long>(decade * 10),
               static_cast<long long>(total / (1024 * 1024)));
  std::fflush(stderr);
  return 0;
}

[[nodiscard]] std::string hex_of(const unsigned char* bytes, unsigned int length) {
  std::string out;
  out.reserve(static_cast<std::size_t>(length) * 2);
  for (unsigned int i = 0; i < length; ++i) {
    out += std::format("{:02x}", bytes[i]);
  }
  return out;
}

[[nodiscard]] Result<void> download(const std::string& url, const std::filesystem::path& dest) {
  const CurlHandle handle{curl_easy_init()};
  if (!handle) {
    return fail(ErrorCode::model_fetch_failed, "could not initialise libcurl");
  }

  const FilePtr file{std::fopen(dest.c_str(), "wb")};
  if (!file) {
    return fail(ErrorCode::model_fetch_failed,
                std::format("cannot write {}: {}", dest.string(), std::strerror(errno)));
  }

  curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
  // HuggingFace redirects to its CDN, so following redirects is required.
  curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(handle.get(), CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, 30L);
  // No total timeout: the model is tens of MB and a slow link is not an error.
  // A stalled one is, so fail if throughput stays under 1 KB/s for 60s.
  curl_easy_setopt(handle.get(), CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(handle.get(), CURLOPT_LOW_SPEED_TIME, 60L);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, write_to_file);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, file.get());
  Progress progress;
  curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, report_progress);
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &progress);
  curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, "capture-eye/0.1");

  const CURLcode code = curl_easy_perform(handle.get());
  std::fprintf(stderr, "\n");
  if (code != CURLE_OK) {
    std::error_code ignored;
    std::filesystem::remove(dest, ignored);
    return fail(ErrorCode::model_fetch_failed,
                std::format("{}: {}", url, curl_easy_strerror(code)));
  }
  return {};
}

} // namespace

Result<ModelSpec> resolve_spec(const ModelConfig& config) {
  if (config.variant.empty()) {
    return fail(ErrorCode::config_invalid, "--model-variant must not be empty");
  }

  ModelSpec spec;
  spec.variant = config.variant;
  spec.url = config.url.empty() ? std::format("{}{}.onnx", kUrlPrefix, config.variant) : config.url;
  spec.sha256 = config.sha256.empty() ? std::string{pinned_hash_for(config.variant)} : config.sha256;

  if (spec.sha256.empty() && !config.allow_unpinned) {
    return fail(ErrorCode::config_invalid,
                std::format("no pinned sha256 for variant '{}'; pass --model-sha or "
                            "--allow-unpinned",
                            config.variant));
  }
  return spec;
}

std::filesystem::path cache_path(const std::filesystem::path& cache_root, const ModelSpec& spec) {
  const std::string_view short_hash =
      std::string_view{spec.sha256}.substr(0, std::min<std::size_t>(8, spec.sha256.size()));
  const std::string name =
      short_hash.empty() ? std::format("yolo26n-{}.onnx", spec.variant)
                         : std::format("yolo26n-{}-{}.onnx", spec.variant, short_hash);
  return cache_root / "models" / name;
}

Result<std::filesystem::path> default_cache_root() {
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && *xdg != '\0') {
    return std::filesystem::path{xdg} / "capture-eye";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    return std::filesystem::path{home} / ".cache" / "capture-eye";
  }
  return fail(ErrorCode::config_invalid, "neither XDG_CACHE_HOME nor HOME is set");
}

Result<std::string> sha256_file(const std::filesystem::path& path) {
  const FilePtr file{std::fopen(path.c_str(), "rb")};
  if (!file) {
    return fail(ErrorCode::model_fetch_failed,
                std::format("cannot read {}: {}", path.string(), std::strerror(errno)));
  }

  const EvpContext ctx{EVP_MD_CTX_new()};
  if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
    return fail(ErrorCode::model_fetch_failed, "could not initialise sha256");
  }

  std::array<unsigned char, 64 * 1024> buffer{};
  while (const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file.get())) {
    if (EVP_DigestUpdate(ctx.get(), buffer.data(), read) != 1) {
      return fail(ErrorCode::model_fetch_failed, "sha256 update failed");
    }
  }
  if (std::ferror(file.get())) {
    return fail(ErrorCode::model_fetch_failed, std::format("read error on {}", path.string()));
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &length) != 1) {
    return fail(ErrorCode::model_fetch_failed, "sha256 finalisation failed");
  }
  return hex_of(digest.data(), length);
}

Result<std::filesystem::path> ensure_model(const ModelConfig& config,
                                           const std::filesystem::path& cache_root) {
  // An explicit path is the operator's business; we neither fetch nor verify it.
  if (!config.path.empty()) {
    if (!std::filesystem::exists(config.path)) {
      return fail(ErrorCode::model_load_failed,
                  std::format("{}: no such file", config.path.string()));
    }
    return config.path;
  }

  const auto spec = resolve_spec(config);
  if (!spec) return std::unexpected(spec.error());

  const auto destination = cache_path(cache_root, *spec);

  if (std::filesystem::exists(destination)) {
    if (spec->sha256.empty()) {
      return destination;  // unpinned: presence is all we can check
    }
    const auto actual = sha256_file(destination);
    if (!actual) return std::unexpected(actual.error());
    if (*actual == spec->sha256) {
      return destination;
    }
    // A cached file that no longer matches is corrupt, not a new version — the
    // hash is part of its name.
    std::fprintf(stderr, "cached model %s failed verification, refetching\n",
                 destination.string().c_str());
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);
  }

  if (config.offline) {
    return fail(ErrorCode::model_fetch_failed,
                std::format("--offline and no cached model at {}", destination.string()));
  }

  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  if (ec) {
    return fail(ErrorCode::model_fetch_failed,
                std::format("cannot create {}: {}", destination.parent_path().string(),
                            ec.message()));
  }

  // Download to a sibling and rename only after verification, so a partial or
  // corrupt transfer is never visible as a usable model.
  auto partial = destination;
  partial += ".part";
  if (const auto downloaded = download(spec->url, partial); !downloaded) {
    return std::unexpected(downloaded.error());
  }

  const auto actual = sha256_file(partial);
  if (!actual) {
    std::filesystem::remove(partial, ec);
    return std::unexpected(actual.error());
  }

  if (spec->sha256.empty()) {
    std::fprintf(stderr, "model sha256 (unpinned): %s\n", actual->c_str());
  } else if (*actual != spec->sha256) {
    std::filesystem::remove(partial, ec);
    return fail(ErrorCode::model_hash_mismatch,
                std::format("{}: expected {}, got {}", spec->url, spec->sha256, *actual));
  }

  std::filesystem::rename(partial, destination, ec);
  if (ec) {
    return fail(ErrorCode::model_fetch_failed,
                std::format("cannot install {}: {}", destination.string(), ec.message()));
  }
  return destination;
}

} // namespace capture_eye
