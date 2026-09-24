#include "playback_backend.h"

// Fábrica del motor de reproducción. Un único punto que decide qué
// PlaybackBackend se instancia, sin tocar el resto de la app:
//   - x64 (Linux y Windows 64): MpvBackend SIEMPRE (pieza protegida) — la
//     integración mpv debe mantenerse como está;
//   - x86 de Windows (Win7, cuando el build active FFmpeg):
//     FfmpegBackend (sin libmpv, más ligero);
//   - NEVERTUBE_BACKEND=mpv|ffmpeg fuerza el motor (desarrollo/verificación:
//     probar el FfmpegBackend en esta máquina Linux con DISPLAY=:0).
//
// La x64 con NEVERTUBE_BACKEND=ffmpeg es SOLO para desarrollo: el runtime
// por defecto de la x64 nunca pierde el mpv.

#include "mpv_backend.h"

#ifdef NEVERTUBE_HAS_FFMPEG_BACKEND
#include "ffmpeg_backend.h"
#endif

PlaybackBackend *createPlaybackBackend(QObject *parent)
{
    const QByteArray forced = qgetenv("NEVERTUBE_BACKEND");
    if (forced == "mpv")
        return new MpvBackend(parent);
#ifdef NEVERTUBE_HAS_FFMPEG_BACKEND
    if (forced == "ffmpeg")
        return new FfmpegBackend(parent);
    // x86 de Windows: el motor barato es el único (el build x86 no enlaza mpv).
#if defined(Q_OS_WIN) && !defined(_WIN64)
    return new FfmpegBackend(parent);
#endif
#endif
    return new MpvBackend(parent);
}