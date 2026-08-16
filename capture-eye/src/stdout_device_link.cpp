#include <cstdio>

#include "device_link.h"

namespace capture_eye {

DeviceLink make_stdout_device_link() {
  DeviceLink link;
  link.name = "stdout";
  link.write = [](std::string_view line) -> Result<void> {
    // The line already ends in '\n'; fwrite keeps it byte-exact so what you see
    // is exactly what the serial port would carry.
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
    return {};
  };
  link.read = []() -> Result<std::vector<std::string>> { return std::vector<std::string>{}; };
  return link;
}

} // namespace capture_eye
