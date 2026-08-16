#include "control_relay.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace capture_eye {
namespace {

using Json = nlohmann::json;

// Disjoint from the pipeline's own outgoing track seq (inference_loop starts
// that counter at 1 and it grows by at most one per frame). If the two ever
// collided, an ack meant for the pipeline's own track could be misrouted to a
// relay client, or vice versa.
//
// Must also stay comfortably inside int32 range: the firmware reads seq as
// `doc["seq"] | 0` (ArduinoJson, main.cpp), a signed-int default. A value at
// or above 2^31 fails that conversion and silently comes back as 0 instead —
// confirmed live against real hardware, where 0x8000'0000 round-tripped as a
// bare `{"t":"ack","seq":0}` that this relay could not route to anyone.
constexpr std::uint32_t kRelaySeqBase = 0x4000'0000u;
constexpr std::uint32_t kRelaySeqMax = 0x7fff'0000u;  // wrap well before INT32_MAX
constexpr std::size_t kOutboundCapacity = 64;
constexpr std::size_t kPendingCap = 256;
constexpr int kAcceptPollMs = 200;

} // namespace

struct ControlRelay::Impl {
  // A client's data (socket, outbound queue, liveness) is deliberately not
  // bundled with its jthreads: the reader/writer threads each hold a
  // shared_ptr<Client> for the duration of their run, and if the jthreads
  // were members of Client too, the last reference could be dropped from
  // *inside* one of those threads — making its own destructor try to join
  // itself. Thread handles live in Impl's maps instead (below), always
  // joined from a thread that is never one of the two being joined (the
  // accept thread, or whichever thread destroys the relay).
  struct Client {
    std::uint64_t id = 0;
    int fd = -1;

    std::mutex out_mutex;
    std::condition_variable_any out_cv;
    std::deque<std::string> out_queue;
    std::atomic<bool> alive{true};

    // Safety net in case a Client ever outlives explicit disconnect()
    // handling (e.g. a transient shared_ptr copy on some call stack).
    // Idempotent with disconnect()'s own shutdown+notify.
    ~Client() {
      if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
      out_cv.notify_all();
    }
  };

  std::size_t max_clients;
  SerialQueue* to_device;
  int listen_fd = -1;
  std::filesystem::path socket_path;

  std::mutex clients_mutex;
  std::unordered_map<std::uint64_t, std::shared_ptr<Client>> clients;
  std::unordered_map<std::uint64_t, std::jthread> reader_threads;
  std::unordered_map<std::uint64_t, std::jthread> writer_threads;
  std::vector<std::uint64_t> pending_reap;  // disconnected; threads not yet joined
  std::atomic<std::uint64_t> next_client_id{1};

  std::mutex pending_mutex;
  std::unordered_map<std::uint32_t, std::pair<std::uint64_t, std::uint32_t>> pending;
  std::uint32_t next_seq = kRelaySeqBase;

  std::jthread accept_thread;

  ~Impl() {
    if (listen_fd >= 0) {
      ::close(listen_fd);
      listen_fd = -1;
    }

    std::vector<std::shared_ptr<Client>> live;
    {
      const std::scoped_lock lock{clients_mutex};
      live.reserve(clients.size());
      for (auto& [id, client] : clients) live.push_back(client);
      clients.clear();
    }
    // Unblock every reader (stuck in recv()) and writer (waiting on out_cv)
    // before joining — otherwise join() below waits forever.
    for (auto& client : live) {
      client->alive.store(false, std::memory_order_relaxed);
      if (client->fd >= 0) ::shutdown(client->fd, SHUT_RDWR);
      client->out_cv.notify_all();
    }
    live.clear();

    // Move the thread handles out from under the lock, then join without
    // holding it. reader_loop's own cleanup calls disconnect(), which needs
    // clients_mutex to erase itself — joining while still holding the lock
    // here would deadlock against exactly that, and did: capture-eye hung on
    // shutdown with a relay client attached until this was found and fixed.
    std::unordered_map<std::uint64_t, std::jthread> readers;
    std::unordered_map<std::uint64_t, std::jthread> writers;
    {
      const std::scoped_lock lock{clients_mutex};
      readers.swap(reader_threads);
      writers.swap(writer_threads);
    }
    for (auto& [id, thread] : readers) thread.join();
    for (auto& [id, thread] : writers) thread.join();

    std::filesystem::remove(socket_path);
  }

  // Joins whatever finished threads a disconnect() left behind. Only ever
  // called from accept_loop, so this is never a self-join either.
  //
  // Handles are moved out from under clients_mutex before join() runs, same
  // as ~Impl() — never call join() while holding a lock a client thread's own
  // cleanup might still need (see ~Impl()'s comment for the deadlock this
  // caused once already).
  void reap() {
    std::vector<std::uint64_t> ids;
    std::vector<std::jthread> to_join;
    {
      const std::scoped_lock lock{clients_mutex};
      ids.swap(pending_reap);
      for (const auto id : ids) {
        if (const auto it = reader_threads.find(id); it != reader_threads.end()) {
          to_join.push_back(std::move(it->second));
          reader_threads.erase(it);
        }
        if (const auto it = writer_threads.find(id); it != writer_threads.end()) {
          to_join.push_back(std::move(it->second));
          writer_threads.erase(it);
        }
      }
    }
    for (auto& thread : to_join) thread.join();
  }

  void accept_loop(std::stop_token token) {
    while (!token.stop_requested()) {
      reap();

      pollfd pfd{.fd = listen_fd, .events = POLLIN, .revents = 0};
      const int ready = ::poll(&pfd, 1, kAcceptPollMs);
      if (ready <= 0) continue;

      const int fd = ::accept(listen_fd, nullptr, nullptr);
      if (fd < 0) continue;

      const std::scoped_lock lock{clients_mutex};
      if (clients.size() >= max_clients) {
        ::close(fd);
        continue;
      }

      auto client = std::make_shared<Client>();
      client->id = next_client_id.fetch_add(1, std::memory_order_relaxed);
      client->fd = fd;
      const auto id = client->id;
      reader_threads.emplace(id, std::jthread{[this, client](std::stop_token t) { reader_loop(client, t); }});
      writer_threads.emplace(id, std::jthread{[this, client](std::stop_token t) { writer_loop(client, t); }});
      clients.emplace(id, std::move(client));
    }
  }

  void reader_loop(const std::shared_ptr<Client>& client, std::stop_token /*token*/) {
    std::string pending_line;
    std::array<char, 512> buffer{};
    for (;;) {
      const auto received = ::recv(client->fd, buffer.data(), buffer.size(), 0);
      if (received <= 0) break;  // EOF, error, or shutdown() from disconnect()

      pending_line.append(buffer.data(), static_cast<std::size_t>(received));
      std::size_t start = 0;
      for (std::size_t i = 0; i < pending_line.size(); ++i) {
        if (pending_line[i] != '\n') continue;
        auto line = pending_line.substr(start, i - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        start = i + 1;
        if (line.empty()) continue;
        handle_client_line(client, std::move(line));
      }
      pending_line.erase(0, start);
      if (pending_line.size() > 4096) pending_line.clear();
    }
    disconnect(client);
  }

  void writer_loop(const std::shared_ptr<Client>& client, std::stop_token token) {
    while (client->alive.load(std::memory_order_relaxed)) {
      std::string line;
      {
        std::unique_lock lock{client->out_mutex};
        const bool woke_with_work = client->out_cv.wait(
            lock, token, [&] { return !client->out_queue.empty() || !client->alive.load(); });
        // false means the jthread's own stop token fired before the predicate
        // was ever satisfied — without this check the loop would spin on a
        // token that stays stopped forever but never sets alive=false.
        if (!woke_with_work) break;
        if (client->out_queue.empty()) break;  // alive flipped false, nothing left to flush
        line = std::move(client->out_queue.front());
        client->out_queue.pop_front();
      }
      if (::send(client->fd, line.data(), line.size(), MSG_NOSIGNAL) < 0) break;
    }
  }

  // The relay's one piece of protocol knowledge: reassign seq on the way in
  // so two clients starting at 1 cannot collide, and remember who to route
  // the reply to. Anything that isn't valid JSON, or has no numeric seq, is
  // forwarded byte-for-byte — capture-eye stays a dumb pipe for those.
  void handle_client_line(const std::shared_ptr<Client>& client, std::string line) {
    Json parsed;
    bool has_seq = false;
    std::uint32_t original_seq = 0;
    try {
      parsed = Json::parse(line);
      if (parsed.is_object() && parsed.contains("seq") && parsed["seq"].is_number_integer()) {
        has_seq = true;
        original_seq = parsed["seq"].template get<std::uint32_t>();
      }
    } catch (const Json::parse_error&) {
      // Not JSON at all — pass it through unmodified; the device will reject
      // it with its own err, same as if the relay were not here.
    }

    std::string outgoing = line;
    if (has_seq && original_seq != 0) {
      const std::uint32_t rewritten = next_seq++;
      if (next_seq >= kRelaySeqMax) next_seq = kRelaySeqBase;  // stays clear of int32 overflow
      {
        const std::scoped_lock lock{pending_mutex};
        if (pending.size() >= kPendingCap) pending.erase(pending.begin());
        pending.emplace(rewritten, std::pair{client->id, original_seq});
      }
      parsed["seq"] = rewritten;
      outgoing = parsed.dump();
    }
    outgoing.push_back('\n');

    if (!to_device->push_command(std::move(outgoing))) {
      std::fprintf(stderr, "control relay: client %llu overran the command queue; disconnecting\n",
                   static_cast<unsigned long long>(client->id));
      disconnect(client);
    }
  }

  void enqueue_to_client(const std::shared_ptr<Client>& client, std::string line) {
    bool overflowed = false;
    {
      std::scoped_lock lock{client->out_mutex};
      if (client->out_queue.size() >= kOutboundCapacity) {
        overflowed = true;
      } else {
        client->out_queue.push_back(std::move(line));
      }
    }
    if (overflowed) {
      // A slow client must never stall the pipeline; disconnecting it is the
      // whole point of a per-client bounded queue instead of an unbounded one.
      std::fprintf(stderr, "control relay: client %llu fell behind; disconnecting\n",
                   static_cast<unsigned long long>(client->id));
      disconnect(client);
      return;
    }
    client->out_cv.notify_all();
  }

  void broadcast(const std::string& line) {
    std::vector<std::shared_ptr<Client>> targets;
    {
      const std::scoped_lock lock{clients_mutex};
      targets.reserve(clients.size());
      for (auto& [id, client] : clients) targets.push_back(client);
    }
    for (auto& client : targets) enqueue_to_client(client, line);
  }

  // Idempotent and safe to call from any thread: the reader's own cleanup
  // calls this after a natural EOF (fd already dead), and the device thread
  // calls it when a client overflows its outbound queue (fd still healthy,
  // so it must be shut down here or the reader would block in recv()
  // forever and the writer would block on a cv nobody notifies). Does not
  // join the client's threads itself — that happens later, via reap() or
  // ~Impl, on a thread that is never the client's own.
  void disconnect(const std::shared_ptr<Client>& client) {
    client->alive.store(false, std::memory_order_relaxed);
    if (client->fd >= 0) ::shutdown(client->fd, SHUT_RDWR);
    client->out_cv.notify_all();

    const std::scoped_lock lock{clients_mutex};
    if (clients.erase(client->id) > 0) pending_reap.push_back(client->id);
  }
};

Result<std::unique_ptr<ControlRelay>> ControlRelay::create(const std::filesystem::path& socket_path,
                                                             std::size_t max_clients,
                                                             SerialQueue& to_device) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return fail(ErrorCode::config_invalid,
                std::format("control socket: {}: {}", socket_path.string(), std::strerror(errno)));
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const std::string path_str = socket_path.string();
  if (path_str.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return fail(ErrorCode::config_invalid,
                std::format("control socket: path too long: {}", path_str));
  }
  std::strncpy(addr.sun_path, path_str.c_str(), sizeof(addr.sun_path) - 1);

  // A stale socket file from a previous crash must not block a fresh bind.
  std::filesystem::remove(socket_path);

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    const int saved_errno = errno;
    ::close(fd);
    return fail(ErrorCode::config_invalid,
                std::format("control socket: bind {}: {}", path_str, std::strerror(saved_errno)));
  }
  if (::listen(fd, 16) != 0) {
    const int saved_errno = errno;
    ::close(fd);
    std::filesystem::remove(socket_path);
    return fail(ErrorCode::config_invalid,
                std::format("control socket: listen {}: {}", path_str, std::strerror(saved_errno)));
  }

  auto impl = std::make_unique<Impl>();
  impl->listen_fd = fd;
  impl->max_clients = max_clients;
  impl->to_device = &to_device;
  impl->socket_path = socket_path;
  impl->accept_thread = std::jthread{[raw = impl.get()](std::stop_token t) { raw->accept_loop(t); }};

  std::fprintf(stderr, "control relay: listening on %s\n", path_str.c_str());
  return std::unique_ptr<ControlRelay>{new ControlRelay{std::move(impl)}};
}

ControlRelay::ControlRelay(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
ControlRelay::~ControlRelay() = default;

void ControlRelay::dispatch_incoming(const std::string& line) {
  Json parsed;
  bool has_seq = false;
  std::uint32_t seq = 0;
  try {
    parsed = Json::parse(line);
    if (parsed.is_object() && parsed.contains("seq") && parsed["seq"].is_number_integer()) {
      has_seq = true;
      seq = parsed["seq"].template get<std::uint32_t>();
    }
  } catch (const Json::parse_error&) {
    // Malformed line from the device — broadcast it as-is; there is nothing
    // more specific to route it to.
  }

  if (has_seq) {
    std::optional<std::pair<std::uint64_t, std::uint32_t>> match;
    {
      const std::scoped_lock lock{impl_->pending_mutex};
      if (const auto it = impl_->pending.find(seq); it != impl_->pending.end()) {
        match = it->second;
        impl_->pending.erase(it);
      }
    }
    if (match.has_value()) {
      parsed["seq"] = match->second;
      std::string rewritten = parsed.dump();
      rewritten.push_back('\n');

      std::shared_ptr<Impl::Client> target;
      {
        const std::scoped_lock lock{impl_->clients_mutex};
        if (const auto it = impl_->clients.find(match->first); it != impl_->clients.end()) {
          target = it->second;
        }
      }
      if (target) impl_->enqueue_to_client(target, std::move(rewritten));
      return;
    }
  }

  // hello, descriptor, state, and anything with no matching pending seq
  // (including the pipeline's own track acks, which live in a disjoint seq
  // range and so never match) go to everyone.
  std::string with_newline = line;
  with_newline.push_back('\n');
  impl_->broadcast(with_newline);
}

std::size_t ControlRelay::client_count() const {
  const std::scoped_lock lock{impl_->clients_mutex};
  return impl_->clients.size();
}

} // namespace capture_eye
