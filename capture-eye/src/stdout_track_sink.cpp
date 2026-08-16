#include <cstdio>

#include "track_sink.h"

namespace capture_eye {

TrackSink make_stdout_track_sink() {
  TrackSink sink;
  sink.name = "stdout";
  sink.write = [](std::string_view line) -> Result<void> {
    // The line already ends in '\n'; fwrite keeps it byte-exact so what you see
    // is exactly what the serial port would carry.
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
    return {};
  };
  return sink;
}

} // namespace capture_eye
