#include "ffmpeg_selftest.h"

#ifdef NEVERTUBE_HAS_FFMPEG_BACKEND

// Selftest offline del FfmpegBackend (Fases 2-4): decodifica en modo banco los
// clips embebidos (av1.mkv / vp9.webm / h264.mp4, los mismos que usa hwprobe)
// y verifica la cadena demux -> decode -> swscale:
//   - caso base por clip: EOF natural, frames>0, códec correcto, tamaño
//     (si hay VAAPI/DXVA2, el h264 sale por HW copy-back; se imprime el device
//     en el reporte, no es un assert: depende de la GPU);
//   - sync fino (Fase 3): SEEK en caliente (flush de decodificadores +
//     re-base del reloj maestro: la posición final debe superar el destino)
//     y PAUSA/reanuda (el reloj no revierte: sigue hasta EOF natural).

#include <QEventLoop>
#include <QFile>
#include <QSize>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

#include "ffmpeg_backend.h"

namespace {

struct Clip {
    const char *resource;
    const char *file;
    const char *codec; // códec esperado (avcodec_get_name)
};

const Clip kClips[] = {
    { ":/clips/av1.mkv", "av1.mkv", "av1" },
    { ":/clips/vp9.webm", "vp9.webm", "vp9" },
    { ":/clips/h264.mp4", "h264.mp4", "h264" },
};

// Extrae un clip embebido a un archivo temporal. Vacío si falla.
QString extractClip(const QTemporaryDir &tmp, const char *resource,
                    const char *file)
{
    QFile in(QString::fromLatin1(resource));
    if (!in.open(QIODevice::ReadOnly))
        return QString();
    const QString outPath = tmp.filePath(QString::fromLatin1(file));
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly))
        return QString();
    out.write(in.readAll());
    out.close();
    return outPath;
}

struct EofInfo {
    bool timedOut = false;
    PlaybackBackend::EndReason reason = PlaybackBackend::EndReason::Quit;
    double duration = -1.0;
    double lastPos = 0.0;
};

// Conecta las señales de interés del backend (una vez por escenario).
void watch(FfmpegBackend &be, QEventLoop &loop, EofInfo &info)
{
    QObject::connect(
        &be, &PlaybackBackend::endFile, &loop,
        [&](PlaybackBackend::EndReason r, const QString &) {
            info.reason = r;
            loop.quit();
        });
    QObject::connect(
        &be, &PlaybackBackend::durationUpdated, &loop,
        [&](double d) { info.duration = d; });
    QObject::connect(
        &be, &PlaybackBackend::positionUpdated, &loop,
        [&](double p) { info.lastPos = p; });
}

// Deja correr el bucle de eventos `ms` milisegundos (o hasta un endFile).
void waitMs(QEventLoop &loop, int ms)
{
    QTimer t;
    t.setSingleShot(true);
    t.setInterval(ms);
    QObject::connect(&t, &QTimer::timeout, &loop, [&] { loop.quit(); });
    t.start();
    loop.exec();
}

// Espera el EOF definitivo (o timeout): el final quita el bucle.
void waitEofPhase(QEventLoop &loop, EofInfo &info, int timeoutMs)
{
    QTimer to;
    to.setSingleShot(true);
    to.setInterval(timeoutMs);
    QObject::connect(&to, &QTimer::timeout, &loop,
                     [&] {
                         info.timedOut = true;
                         loop.quit();
                     });
    to.start();
    loop.exec();
}

void report(const char *label, bool ok, const char *extra)
{
    std::fprintf(stderr, "SELFTEST ffmpeg %-22s -> %s%s\n", label,
                 ok ? "OK" : "FAIL", extra);
}

} // namespace

bool ffmpegSelfTest()
{
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "SELFTEST ffmpeg -> no se pudo crear tmpdir\n");
        return false;
    }

    int failures = 0;

    // --- caso base por clip (cadena demux->decode->swscale) ---
    for (const Clip &clip : kClips) {
        const QString outPath = extractClip(tmp, clip.resource, clip.file);
        if (outPath.isEmpty()) {
            std::fprintf(stderr, "SELFTEST ffmpeg -> falta %s\n",
                         clip.resource);
            ++failures;
            continue;
        }

        FfmpegBackend be;
        be.setBenchMode(true); // sin sink de audio, sin render, solo contar
        QEventLoop loop;
        EofInfo info;
        watch(be, loop, info);
        be.playUrl(outPath, 0.0, QString());
        waitEofPhase(loop, info, 15000);

        const int frames = be.framesDecoded();
        const QSize size = be.videoSize();
        const int maxH = be.probeMaxVideoHeight();
        const bool ok = !info.timedOut &&
                        info.reason == PlaybackBackend::EndReason::Eof &&
                        frames > 0 &&
                        be.videoCodec() == QLatin1String(clip.codec) &&
                        size.width() > 0 && size.height() > 0 && maxH > 0;
        char buf[96];
        std::snprintf(buf, sizeof buf, " (hw=%s)",
                      be.hwdecCurrent().toUtf8().constData());
        report(clip.file, ok, buf);
        if (!ok)
            ++failures;
        be.shutdown();
    }

    // --- sync fino (Fase 3) sobre h264.mp4 ---
    const QString h264 = extractClip(tmp, kClips[2].resource, kClips[2].file);
    if (h264.isEmpty()) {
        std::fprintf(stderr, "SELFTEST ffmpeg -> falta %s\n",
                     kClips[2].resource);
        ++failures;
    } else {
        // SEEK en caliente: buscar al 50% y verificar que se termina con EOF
        // natural habiendo avanzado más allá del destino.
        {
            FfmpegBackend be;
            be.setBenchMode(true);
            QEventLoop loop;
            EofInfo info;
            watch(be, loop, info);
            be.playUrl(h264, 0.0, QString());
            waitMs(loop, 400); // deja que llegue la duración del clip
            const double target =
                info.duration > 0 ? info.duration / 2 : 0.4;
            be.seek(target);
            waitEofPhase(loop, info, 10000);
            const bool ok = !info.timedOut &&
                            info.reason == PlaybackBackend::EndReason::Eof &&
                            be.framesDecoded() > 0 &&
                            info.lastPos >= target - 0.3;
            char buf[96];
            std::snprintf(buf, sizeof buf,
                          " (target=%.2f pos=%.2f frames=%d)", target,
                          info.lastPos, be.framesDecoded());
            report("seek en caliente", ok, buf);
            be.shutdown();
            if (!ok)
                ++failures;
        }

        // PAUSA/reanuda: congelar ~350 ms a mitad y verificar que la
        // reproducción termina con EOF natural (el reloj no revierte).
        {
            FfmpegBackend be;
            be.setBenchMode(true);
            QEventLoop loop;
            EofInfo info;
            watch(be, loop, info);
            be.playUrl(h264, 0.0, QString());
            QTimer pauseT;
            pauseT.setSingleShot(true);
            pauseT.setInterval(250);
            QObject::connect(&pauseT, &QTimer::timeout, &be,
                             [&] { be.setPaused(true); });
            QTimer resumeT;
            resumeT.setSingleShot(true);
            resumeT.setInterval(600);
            QObject::connect(&resumeT, &QTimer::timeout, &be,
                             [&] { be.setPaused(false); });
            pauseT.start();
            resumeT.start();
            waitEofPhase(loop, info, 10000);
            const bool ok = !info.timedOut &&
                            info.reason == PlaybackBackend::EndReason::Eof &&
                            be.framesDecoded() > 0 &&
                            be.videoSize().width() > 0;
            char buf[80];
            std::snprintf(buf, sizeof buf, " (frames=%d)", be.framesDecoded());
            report("pausa/reanuda", ok, buf);
            be.shutdown();
            if (!ok)
                ++failures;
        }
    }

    report("ffmpeg (global)", failures == 0, "");
    return failures == 0;
}

#else // !NEVERTUBE_HAS_FFMPEG_BACKEND

// Build sin FFmpeg (p. ej. x64 Linux sin libav): skip, no fallo.
bool ffmpegSelfTest()
{
    std::fprintf(stderr,
                 "SELFTEST ffmpeg -> skip (FFmpeg no compilado en este build)\n");
    return true;
}

#endif