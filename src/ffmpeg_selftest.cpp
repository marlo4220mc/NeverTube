#include "ffmpeg_selftest.h"

#ifdef NEVERTUBE_HAS_FFMPEG_BACKEND

// Selftest offline del FfmpegBackend (Fase 2): decodifica en modo banco los
// clips embebidos (av1.mkv / vp9.webm / h264.mp4, los mismos que usa hwprobe)
// y verifica que la cadena demux -> decode -> swscale funciona de verdad:
// EOF natural, frames decodificados, códec correcto y tamaño de vídeo.

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

} // namespace

bool ffmpegSelfTest()
{
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "SELFTEST ffmpeg -> no se pudo crear tmpdir\n");
        return false;
    }

    int failures = 0;
    for (const Clip &clip : kClips) {
        // Extraer el clip (los recursos de hwprobe) a un archivo temporal.
        QFile in(QString::fromLatin1(clip.resource));
        if (!in.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "SELFTEST ffmpeg -> falta %s\n",
                         clip.resource);
            ++failures;
            continue;
        }
        const QString outPath = tmp.filePath(QString::fromLatin1(clip.file));
        QFile out(outPath);
        if (!out.open(QIODevice::WriteOnly)) {
            std::fprintf(stderr, "SELFTEST ffmpeg -> no se pudo escribir %s\n",
                         clip.file);
            ++failures;
            continue;
        }
        out.write(in.readAll());
        out.close();

        FfmpegBackend be;
        be.setBenchMode(true); // sin sink de audio, sin render, solo contar

        QEventLoop loop;
        PlaybackBackend::EndReason reason = PlaybackBackend::EndReason::Quit;
        QString err;
        bool done = false;
        bool timedOut = false;
        QObject::connect(
            &be, &PlaybackBackend::endFile, &loop,
            [&](PlaybackBackend::EndReason r, const QString &msg) {
                reason = r;
                err = msg;
                done = true;
                loop.quit();
            });
        QTimer to;
        to.setSingleShot(true);
        to.setInterval(15000);
        QObject::connect(
            &to, &QTimer::timeout, &loop,
            [&]() {
                timedOut = true;
                done = true;
                loop.quit();
            });

        be.playUrl(outPath, 0.0, QString());
        to.start();
        loop.exec();

        const int frames = be.framesDecoded();
        const QSize size = be.videoSize();
        const int maxH = be.probeMaxVideoHeight();
        const bool ok = !timedOut && err.isEmpty() &&
                        reason == PlaybackBackend::EndReason::Eof && frames > 0 &&
                        be.videoCodec() == QLatin1String(clip.codec) &&
                        size.width() > 0 && size.height() > 0 && maxH > 0;
        std::fprintf(stderr,
                     "SELFTEST ffmpeg %-10s reason=%s frames=%d size=%dx%d "
                     "codec=%s maxH=%d -> %s%s\n",
                     clip.file, timedOut ? "TIMEOUT"
                                         : (reason == PlaybackBackend::EndReason::Eof
                                                ? "EOF"
                                                : "ERROR"),
                     frames, size.width(), size.height(),
                     be.videoCodec().toUtf8().constData(), maxH,
                     ok ? "OK" : "FAIL",
                     err.isEmpty() ? "" : err.toUtf8().constData());
        be.shutdown();
        if (!ok)
            ++failures;
    }

    std::fprintf(stderr, "SELFTEST ffmpeg -> %s\n",
                 failures == 0 ? "OK" : "FAIL");
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