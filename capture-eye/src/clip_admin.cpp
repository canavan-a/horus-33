#include "clip_admin.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace capture_eye {
namespace {

using Json = nlohmann::json;
constexpr int kAcceptPollMs = 200;

Json status_json(const ClipRuntime::Status& status) {
  return Json{{"ok", true}, {"enabled", status.enabled}, {"recording", status.recording}};
}

// One connection at a time is handled synchronously in its own thread; there
// is no shared dispatch state to protect beyond ClipRuntime's own atomics/
// mutex, so unlike ControlRelay there is no per-client outbound queue or
// writer thread — a request always gets exactly one response, written before
// the next read.
void handle_connection(int fd, const std::shared_ptr<ClipRuntime>& runtime) {
  std::string pending;
  std::array<char, 512> buffer{};
  for (;;) {
    const auto received = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (received <= 0) break;
    pending.append(buffer.data(), static_cast<std::size_t>(received));

    std::size_t start = 0;
    for (std::size_t i = 0; i < pending.size(); ++i) {
      if (pending[i] != '\n') continue;
      auto line = pending.substr(start, i - start);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      start = i + 1;
      if (line.empty()) continue;

      Json response;
      try {
        const Json request = Json::parse(line);
        const std::string cmd = request.value("cmd", "");
        if (cmd == "set_enabled") {
          runtime->set_enabled(request.value("enabled", true));
          response = status_json(runtime->status());
        } else if (cmd == "status") {
          response = status_json(runtime->status());
        } else {
          response = Json{{"ok", false}, {"error", std::format("unknown cmd '{}'", cmd)}};
        }
      } catch (const Json::parse_error& e) {
        response = Json{{"ok", false}, {"error", std::format("invalid JSON: {}", e.what())}};
      }

      std::string out = response.dump();
      out.push_back('\n');
      if (::send(fd, out.data(), out.size(), MSG_NOSIGNAL) < 0) {
        ::close(fd);
        return;
      }
    }
    pending.erase(0, start);
    if (pending.size() > 4096) pending.clear();
  }
  ::close(fd);
}

} // namespace

struct ClipAdmin::Impl {
  int listen_fd = -1;
  std::filesystem::path socket_path;
  std::shared_ptr<ClipRuntime> runtime;
  std::jthread accept_thread;

  std::mutex clients_mutex;
  std::vector<std::jthread> clients;

  ~Impl() {
    if (listen_fd >= 0) {
      ::close(listen_fd);
      listen_fd = -1;
    }
    accept_thread = {};  // request_stop + join via jthread dtor

    std::vector<std::jthread> to_join;
    {
      const std::scoped_lock lock{clients_mutex};
      to_join.swap(clients);
    }
    for (auto& thread : to_join) thread.join();

    std::filesystem::remove(socket_path);
  }

  void accept_loop(std::stop_token token) {
    while (!token.stop_requested()) {
      pollfd pfd{.fd = listen_fd, .events = POLLIN, .revents = 0};
      const int ready = ::poll(&pfd, 1, kAcceptPollMs);
      if (ready <= 0) continue;

      const int fd = ::accept(listen_fd, nullptr, nullptr);
      if (fd < 0) continue;

      const std::scoped_lock lock{clients_mutex};
      // Reap finished connections opportunistically rather than growing
      // unbounded — admin connections are short-lived request/response
      // sessions, so this rarely accumulates more than one or two.
      std::erase_if(clients, [](const std::jthread& t) { return !t.joinable(); });
      clients.emplace_back([fd, runtime = runtime](std::stop_token) { handle_connection(fd, runtime); });
    }
  }
};

Result<std::unique_ptr<ClipAdmin>> ClipAdmin::create(const std::filesystem::path& socket_path,
                                                      std::shared_ptr<ClipRuntime> runtime) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return fail(ErrorCode::config_invalid,
                std::format("clip admin socket: {}: {}", socket_path.string(), std::strerror(errno)));
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const std::string path_str = socket_path.string();
  if (path_str.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return fail(ErrorCode::config_invalid,
                std::format("clip admin socket: path too long: {}", path_str));
  }
  std::strncpy(addr.sun_path, path_str.c_str(), sizeof(addr.sun_path) - 1);

  std::filesystem::remove(socket_path);

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    const int saved_errno = errno;
    ::close(fd);
    return fail(ErrorCode::config_invalid,
                std::format("clip admin socket: bind {}: {}", path_str, std::strerror(saved_errno)));
  }
  if (::listen(fd, 8) != 0) {
    const int saved_errno = errno;
    ::close(fd);
    std::filesystem::remove(socket_path);
    return fail(ErrorCode::config_invalid,
                std::format("clip admin socket: listen {}: {}", path_str, std::strerror(saved_errno)));
  }

  auto impl = std::make_unique<Impl>();
  impl->listen_fd = fd;
  impl->socket_path = socket_path;
  impl->runtime = std::move(runtime);
  impl->accept_thread = std::jthread{[raw = impl.get()](std::stop_token t) { raw->accept_loop(t); }};

  std::fprintf(stderr, "clip admin: listening on %s\n", path_str.c_str());
  return std::unique_ptr<ClipAdmin>{new ClipAdmin{std::move(impl)}};
}

ClipAdmin::ClipAdmin(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
ClipAdmin::~ClipAdmin() = default;

} // namespace capture_eye
