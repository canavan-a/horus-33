#pragma once

#include "config.h"
#include "error.h"
#include "frame_sink.h"

namespace capture_eye {

// Encodes annotated frames to H.264 and publishes them over RTSP.
//
// The far end is MediaMTX, which re-serves the stream as WebRTC/WHEP, RTSP and
// LL-HLS. Keeping the transport in a media server rather than in this process
// means the eventual move to Media-over-QUIC is a server config change, not a
// C++ rewrite.
//
// Encoding happens synchronously inside submit. At 720p that is a few
// milliseconds, comfortably inside a frame interval, and it means the sink never
// retains a frame past the call — which is exactly the contract the overlay's
// scratch buffer requires.
//
// With audio.device set, the same stream also carries the microphone as Opus
// (see AudioTrack). A mic that fails to open is logged and the stream goes
// out video-only: losing audio should never cost the video or the tracking.
[[nodiscard]] Result<FrameSink> make_h264_rtsp_sink(const SinkConfig& config,
                                                    const AudioConfig& audio, int width,
                                                    int height, int fps);

} // namespace capture_eye
