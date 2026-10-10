#include "ffmpeg_selftest.h"

// <cstdio> is needed in BOTH branches of the guard: the selftest body uses
// std::snprintf/std::fprintf and the skip branch (#else) uses std::fprintf. It
// was inside the #ifdef and MSVC (unlike libstdc++) does not include it
// transitively -> C2039 in builds without FFmpeg (e.g. Windows x64).
#include <cstdio>

#ifdef NEVERTUBE_HAS_FFMPEG_BACKEND

// Offline self-test of the FfmpegBackend (Phases 2-4): decodes in bench mode the
// embedded clips (av1.mkv / vp9.webm / h264.mp4, the same ones hwprobe uses)
// and checks the demux -> decode -> swscale chain:
//   - base case per clip: natural EOF, frames>0, correct codec, size
//     (with VAAPI/DXVA2, h264 comes out through HW copy-back; the device is
//     printed in the report, it is not an assert: it depends on the GPU);
//   - fine sync (Phase 3): hot SEEK (decoder flush + master clock
//     re-base: the final position must go past the target)
//     and PAUSE/resume (the clock does not rewind: it goes on to a natural EOF).

#include <QEventLoop>
#include <QFile>
#include <QSize>
#include <QTemporaryDir>
#include <QTimer>

#include <cstring>

#include "ffmpeg_backend.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

// Can this build decode AV1 in SOFTWARE? FFmpeg 9's NATIVE `av1` decoder does
// NOT decode in software: with no active av1 hwaccel it aborts the whole clip
// with ENOSYS ("Your platform doesn't support hardware accelerated
// AV1 decoding", av1dec.c). The vendored i686 package brings neither libdav1d
// nor libaom (sw) nor an av1 hwaccel -> AV1 is impossible on x86, and the x86
// app never asks for it (h264-hw profile): the clip is skipped instead of
// failing. Where there IS a sw AV1 decoder (Linux/x64 with libdav1d/libaom) it
// is checked like the rest. (A build without sw but with a usable av1 hwaccel
// would fall outside this heuristic; distro packages ship libdav1d anyway.)
bool av1SoftwareDecodable()
{
    return avcodec_find_decoder_by_name("libdav1d") != nullptr ||
           avcodec_find_decoder_by_name("libaom-av1") != nullptr;
}

struct Clip {
    const char *resource;
    const char *file;
    const char *codec; // expected codec (avcodec_get_name)
};

const Clip kClips[] = {
    { ":/clips/av1.mkv", "av1.mkv", "av1" },
    { ":/clips/vp9.webm", "vp9.webm", "vp9" },
    { ":/clips/h264.mp4", "h264.mp4", "h264" },
};

// Extracts an embedded clip to a temporary file. Empty on failure.
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

// Connects the backend's signals of interest (once per scenario).
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

// Runs the event loop for `ms` milliseconds (or until an endFile).
void waitMs(QEventLoop &loop, int ms)
{
    QTimer t;
    t.setSingleShot(true);
    t.setInterval(ms);
    QObject::connect(&t, &QTimer::timeout, &loop, [&] { loop.quit(); });
    t.start();
    loop.exec();
}

// Waits for the final EOF (or timeout): the end quits the loop.
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

    // --- base case per clip (demux->decode->swscale chain) ---
    for (const Clip &clip : kClips) {
        // AV1 in builds without a software decoder (i686 package): impossible
        // to decode (see av1SoftwareDecodable). On x86 the app does not use it.
        if (std::strcmp(clip.codec, "av1") == 0 && !av1SoftwareDecodable()) {
            std::fprintf(stderr,
                         "SELFTEST ffmpeg %-22s -> skip (sin decoder AV1 de "
                         "software en este build)\n",
                         clip.file);
            continue;
        }
        const QString outPath = extractClip(tmp, clip.resource, clip.file);
        if (outPath.isEmpty()) {
            std::fprintf(stderr, "SELFTEST ffmpeg -> falta %s\n",
                         clip.resource);
            ++failures;
            continue;
        }

        FfmpegBackend be;
        be.setBenchMode(true); // no audio sink, no render, just count
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

    // --- fine sync (Phase 3) on h264.mp4 ---
    const QString h264 = extractClip(tmp, kClips[2].resource, kClips[2].file);
    if (h264.isEmpty()) {
        std::fprintf(stderr, "SELFTEST ffmpeg -> falta %s\n",
                     kClips[2].resource);
        ++failures;
    } else {
        // Hot SEEK: seek to 50% and check that it ends with a natural EOF
        // having advanced past the target.
        {
            FfmpegBackend be;
            be.setBenchMode(true);
            QEventLoop loop;
            EofInfo info;
            watch(be, loop, info);
            be.playUrl(h264, 0.0, QString());
            waitMs(loop, 400); // let the clip duration arrive
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

        // PAUSE/resume: freeze ~350 ms halfway and check that playback ends
        // with a natural EOF (the clock does not rewind).
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

// Build without FFmpeg (e.g. x64 Linux without libav): skip, not a failure.
bool ffmpegSelfTest()
{
    std::fprintf(stderr,
                 "SELFTEST ffmpeg -> skip (FFmpeg no compilado en este build)\n");
    return true;
}

#endif