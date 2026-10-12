#pragma once

extern "C" {
#include <libavformat/avformat.h>
}

#include <chrono>
#include <functional>
#include <memory>

#include "audio_ring.h"
#include "config.h"
#include "error.h"

namespace capture_eye {

struct Encoder;

// The microphone, encoded to Opus once and handed to every consumer that wants
// it — the RTSP stream and the clip sink. Only one process (and one open) may
// hold the ALSA device, so this is the single reader; consumers subscribe
// rather than each opening the mic.
//
// Captures through libavdevice's ALSA input at a fixed 48kHz — Opus's native
// rate, so nothing resamples here. A device that cannot do 48kHz is a startup
// error; a plughw: device has ALSA convert instead.
class AudioSource {
public:
  using Subscriber = std::function<void(const AudioPacket&)>;

  // Opens the capture device and the encoder. Nothing is read until start().
  [[nodiscard]] static Result<std::shared_ptr<AudioSource>> open(const AudioConfig& config);

  AudioSource(const AudioSource&) = delete;
  AudioSource& operator=(const AudioSource&) = delete;
  ~AudioSource();  // stops and joins the reader thread, then drops subscribers

  // Adds an Opus stream matching this source's encoder to `muxer`. Must run
  // before the muxer's header is written.
  [[nodiscard]] Result<AVStream*> add_stream(AVFormatContext* muxer) const;

  // Every encoded packet is passed to `subscriber` on the reader thread, so it
  // must be quick. Subscribe before start(); the list is not locked. A
  // subscriber keeps whatever it captures alive until this source is
  // destroyed, which is what lets it safely outlive the sink that added it.
  void subscribe(Subscriber subscriber);

  void start();

private:
  struct Impl;
  explicit AudioSource(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// Writes one packet into `stream` of `muxer`, timed relative to `epoch` (the
// instant that stream's pts 0 refers to). A packet captured before the epoch
// belongs to no part of this output and is skipped.
[[nodiscard]] Result<void> mux_audio_packet(Encoder& muxer, const AVStream* stream,
                                            const AudioPacket& packet,
                                            std::chrono::steady_clock::time_point epoch);

} // namespace capture_eye
