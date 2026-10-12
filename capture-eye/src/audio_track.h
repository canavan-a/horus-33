#pragma once

extern "C" {
#include <libavformat/avformat.h>
}

#include <chrono>
#include <functional>
#include <memory>

#include "config.h"
#include "error.h"

namespace capture_eye {

// Microphone → Opus, written as a second stream into a muxer the video
// encoder already owns (the RTSP sink's), so a WebRTC viewer gets audio and
// video in one session with one clock rather than two streams to line up.
//
// Captures through libavdevice's ALSA input at a fixed 48kHz — Opus's native
// rate, so nothing resamples here. A device that cannot do 48kHz is a startup
// error; a plughw: device has ALSA convert instead.
class AudioTrack {
public:
  using Writer = std::function<Result<void>(AVPacket*)>;

  // Opens the capture device and the encoder and adds the audio stream to
  // `muxer`. Must run before the muxer's header is written. Nothing is read
  // until start().
  [[nodiscard]] static Result<std::unique_ptr<AudioTrack>> open(const AudioConfig& config,
                                                                AVFormatContext* muxer);

  AudioTrack(const AudioTrack&) = delete;
  AudioTrack& operator=(const AudioTrack&) = delete;
  ~AudioTrack();  // stops and joins the reader thread

  // Starts the reader thread. `epoch` is the instant the video stream's pts 0
  // refers to; audio timestamps are measured from the same instant, which is
  // the whole of the A/V sync. Every encoded packet goes to `write`, from the
  // reader thread.
  void start(std::chrono::steady_clock::time_point epoch, Writer write);

private:
  struct Impl;
  explicit AudioTrack(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace capture_eye
