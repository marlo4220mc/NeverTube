#include "playback_backend.h"

// Playback engine factory. The single point that decides which
// PlaybackBackend gets instantiated, without touching the rest of the app:
//   - Windows x86 build without mpv (NEVERTUBE_NO_MPV, Win7, Phase 5):
//     FfmpegBackend ALWAYS (no libmpv linked);
//   - everything else (Linux and Windows x64): MpvBackend ALWAYS (protected piece);
//   - NEVERTUBE_BACKEND=mpv|ffmpeg forces the engine (development/verification:
//     try the FfmpegBackend on this Linux machine with DISPLAY=:0).
//
// x64 with NEVERTUBE_BACKEND=ffmpeg is FOR development only: the default
// x64 runtime never loses mpv.

#ifndef NEVERTUBE_NO_MPV
#include "mpv_backend.h"
#endif

#ifdef NEVERTUBE_HAS_FFMPEG_BACKEND
#include "ffmpeg_backend.h"
#endif

PlaybackBackend *createPlaybackBackend(QObject *parent)
{
#ifdef NEVERTUBE_NO_MPV
    // x86 build without mpv: the only engine is the FfmpegBackend.
    return new FfmpegBackend(parent);
#else
    const QByteArray forced = qgetenv("NEVERTUBE_BACKEND");
    if (forced == "mpv")
        return new MpvBackend(parent);
#ifdef NEVERTUBE_HAS_FFMPEG_BACKEND
    if (forced == "ffmpeg")
        return new FfmpegBackend(parent);
#endif
    return new MpvBackend(parent);
#endif
}