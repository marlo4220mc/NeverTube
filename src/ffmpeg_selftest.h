#pragma once

// FfmpegBackend selftest (Phase 2). Decodes in BENCH MODE the embedded clips
// (av1/vp9/h264, the same ones as hwprobe) with NO network, NO audio sink and
// NO GL render: validates the critical path demux -> decode -> swscale and
// that the backend emits a natural EOF with the correct codec. Only available
// when the build compiled the backend (NEVERTUBE_HAS_FFMPEG_BACKEND); without
// FFmpeg it is a skip (not a failure), so that --selftest-ffmpeg does not
// break x64 builds without FFmpeg.
bool ffmpegSelfTest();