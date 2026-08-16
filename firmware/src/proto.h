// Wire protocol: one JSON object per line over USB CDC. See docs/protocol.md.
#pragma once

#include <ArduinoJson.h>

namespace proto {

constexpr int VERSION = 1;
constexpr size_t MAX_LINE = 512;

// Message type tags.
constexpr const char *T_DESCRIBE = "describe";
constexpr const char *T_SET = "set";
constexpr const char *T_PING = "ping";
constexpr const char *T_TRACK = "track";
constexpr const char *T_HELLO = "hello";
constexpr const char *T_DESCRIPTOR = "descriptor";
constexpr const char *T_STATE = "state";
constexpr const char *T_ACK = "ack";
constexpr const char *T_ERR = "err";

// Field type names, matching the host's expectations.
constexpr const char *F_NUMBER = "number";
constexpr const char *F_COLOR = "color";
constexpr const char *F_ENUM = "enum";
constexpr const char *F_BOOL = "bool";

// begin sets up the serial port and the transmit mutex. Call once from setup().
void begin(unsigned long baud);

// send serialises a document as one line. Safe to call from any task: writes are
// serialised behind a mutex so two tasks cannot interleave halves of a line.
void send(const JsonDocument &doc);

// Convenience senders for the small fixed-shape replies.
void sendHello(const char *device, const char *fw);
void sendAck(uint32_t seq);
void sendErr(uint32_t seq, const char *msg);

// Format a color as "#rrggbb" into out (needs 8 bytes).
void formatColor(uint8_t r, uint8_t g, uint8_t b, char *out);

// Parse "#rrggbb" or "rrggbb". Returns false and leaves outputs untouched on
// malformed input.
bool parseColor(const char *s, uint8_t &r, uint8_t &g, uint8_t &b);

} // namespace proto
