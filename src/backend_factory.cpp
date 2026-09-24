#include "playback_backend.h"

// Fábrica del motor de reproducción. Un único punto que decide qué
// PlaybackBackend se instancia, sin tocar el resto de la app:
//   - build x86 de Windows sin mpv (NEVERTUBE_NO_MPV, Win7, Fase 5):
//     FfmpegBackend SIEMPRE (no hay libmpv enlazado);
//   - resto (Linux y Windows x64): MpvBackend SIEMPRE (pieza protegida);
//   - NEVERTUBE_BACKEND=mpv|ffmpeg fuerza el motor (desarrollo/verificación:
//     probar el FfmpegBackend en esta máquina Linux con DISPLAY=:0).
//
// La x64 con NEVERTUBE_BACKEND=ffmpeg es SOLO para desarrollo: el runtime
// por defecto de la x64 nunca pierde el mpv.

#ifndef NEVERTUBE_NO_MPV
#include "mpv_backend.h"
#endif

#ifdef NEVERTUBE_HAS_FFMPEG_BACKEND
#include "ffmpeg_backend.h"
#endif

PlaybackBackend *createPlaybackBackend(QObject *parent)
{
#ifdef NEVERTUBE_NO_MPV
    // Build x86 sin mpv: el único motor es el FfmpegBackend.
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