#include "hwprobe.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#ifndef NEVERTUBE_NO_MPV
#include <mpv/client.h>
#endif

#include <clocale>

#include "debug.h"

// The whole clip-probing machinery uses mpv: it only exists in builds with
// mpv. In the x86 build without mpv (NEVERTUBE_NO_MPV, Phase 5) the codec
// profile is decided by platform (DXVA2 -> h264-hw as the cap) and the
// reactive ladder of streamDecodeInfoReady covers machines without HW; here
// only the pure helpers (hwprobe::clipVerdict / lowerProfile) below remain.
#ifndef NEVERTUBE_NO_MPV
namespace {

// Deliberate order: if AV1 has HW, it is the best codec and there is no need
// to test further; VP9 is only accepted if AV1 fell back to software (same
// semantics as the previous ladder); H.264 HW confirms the legacy profile;
// H.264 software is the proof that there is no HW at all (H.264 is decoded
// in HW by every GPU with a video decoder).
struct ClipSpec
{
    const char *resource;
    const char *file;
    const char *profile;
};

const ClipSpec kClips[] = {
    { ":/clips/av1.mkv", "av1.mkv", "av1-hw" },
    { ":/clips/vp9.webm", "vp9.webm", "vp9-hw" },
    { ":/clips/h264.mp4", "h264.mp4", "h264-hw" },
};
constexpr int kClipCount = 3;

constexpr int kPollMs = 10;
// 8 s per clip (with one retry) is plenty for a local 512x288 clip.
// On virtual machines/runners the FIRST hardware decoder init
// (DXVA/VAAPI) can take several seconds; if after the retry there is still
// no verdict, the clip stays indeterminate and it moves to the next codec.
constexpr qint64 kClipTimeoutMs = 8000;
// Grace after the first frame: the decoder verdict (log and ffmpeg
// errors) is queued 1-10 ms AFTER the first video-params. Deciding without
// waiting for it is what produced false positives (av1-hw on i5 with qsv-copy:
// the "av1_qsv: unsupported" errors arrived 5 ms after the verdict).
constexpr qint64 kGraceMs = 400;

// Codec of the current clip, e.g. "av1-hw" -> "av1".
QByteArray codecOf(int index)
{
    const QString profile = QLatin1String(kClips[index].profile);
    const int dash = profile.indexOf(QLatin1Char('-'));
    return profile.left(dash).toUtf8();
}

// Error of the codec's HW attempt in the ffmpeg/vd log: the attempt failed
// but the clip may still be decoding in software. It is the signal that
// contradicts an active hwdec-current = "<api>-copy" (i5/HD4600, av1_qsv).
bool hwAttemptFailed(const QString &prefix, const QString &text,
                     const QByteArray &codec)
{
    if (!(prefix.contains(QLatin1String("ffmpeg"))
          || prefix.contains(QLatin1String("vd"))
          || prefix.contains(QLatin1String("hwdec"))))
        return false;
    if (!text.contains(QLatin1String(codec.constData()), Qt::CaseInsensitive))
        return false;
    static const char *kErrors[] = { "error", "unsupported", "invalid",
                                     "failed", "could not",  "cannot" };
    for (const char *e : kErrors) {
        if (text.contains(QLatin1String(e), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

} // namespace
#endif // NEVERTUBE_NO_MPV

namespace hwprobe {

QString clipVerdict(bool sawHwLog, bool sawSwLog, bool sawHwFailed,
                    const QString &hwdecCurrent)
{
    if (hwdecCurrent == QLatin1String("no"))
        return QStringLiteral("SW");
    if (sawSwLog || sawHwFailed)
        return QStringLiteral("SW");
    if (sawHwLog)
        return QStringLiteral("HW");
    if (!hwdecCurrent.isEmpty())
        return QStringLiteral("HW"); // fallback: backend active, nothing against it
    return QString();
}

QString lowerProfile(const QString &profile)
{
    if (profile == QLatin1String("av1-hw"))
        return QStringLiteral("vp9-hw");
    if (profile == QLatin1String("vp9-hw"))
        return QStringLiteral("h264-hw");
    if (profile == QLatin1String("h264-hw"))
        return QStringLiteral("soft-all");
    // soft-all is the floor; an unknown profile is left untouched.
    return QString();
}

} // namespace hwprobe

#ifndef NEVERTUBE_NO_MPV

HwProbe::HwProbe(QObject *parent)
    : QObject(parent)
{
}

HwProbe::~HwProbe()
{
    if (m_timer)
        m_timer->stop();
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
}

void HwProbe::start()
{
    if (m_done || m_mpv)
        return;
    // mpv requires LC_NUMERIC=C: without this mpv_create returns NULL.
    std::setlocale(LC_NUMERIC, "C");

    if (!m_tmpDir.isValid()) {
        qWarning("hwprobe: no se pudo crear el directorio temporal");
        finish(QString());
        return;
    }
    for (int i = 0; i < kClipCount; ++i) {
        QFile in(QString::fromLatin1(kClips[i].resource));
        if (!in.open(QIODevice::ReadOnly)) {
            qWarning("hwprobe: falta el recurso %s", kClips[i].resource);
            finish(QString());
            return;
        }
        const QString out =
            m_tmpDir.filePath(QString::fromLatin1(kClips[i].file));
        QFile of(out);
        if (!of.open(QIODevice::WriteOnly)) {
            qWarning("hwprobe: no se pudo preparar %s",
                     out.toUtf8().constData());
            finish(QString());
            return;
        }
        of.write(in.readAll());
        of.close();
        m_clipPaths[i] = QDir::toNativeSeparators(out);
    }

    m_mpv = mpv_create();
    if (!m_mpv) {
        qWarning("hwprobe: mpv_create fallo");
        finish(QString());
        return;
    }
    // Headless: no window and no audio. auto-copy is what lets vo=null
    // accept the frame on any platform (vulkan-copy / d3d11va-copy /
    // vaapi-copy): the modes without -copy would require a real GPU VO.
    mpv_set_option_string(m_mpv, "vo", "null");
    mpv_set_option_string(m_mpv, "hwdec", "auto-copy");
    mpv_set_option_string(m_mpv, "audio", "no");
    mpv_set_option_string(m_mpv, "aid", "no");
    mpv_set_option_string(m_mpv, "config", "no");
    mpv_set_option_string(m_mpv, "load-scripts", "no");
    mpv_set_option_string(m_mpv, "terminal", "no");
    // vd=v (decoder verdict: "Using hardware decoding (...)" /
    // "Using software decoding") + ffmpeg=v (HW attempt errors, e.g.
    // "av1_qsv: Error ... unsupported (-3)"). The ffmpeg module was set to "no"
    // and that is why the probe did not see that the HW attempt had failed.
    mpv_set_option_string(m_mpv, "msg-level", "all=no,vd=v,ffmpeg=v");
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "force-window", "no");
    if (mpv_initialize(m_mpv) < 0) {
        qWarning("hwprobe: mpv_initialize fallo");
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        finish(QString());
        return;
    }
    // The log events carry the HW verdict (see the header comment).
    mpv_request_log_messages(m_mpv, "v");

    m_index = 0;
    m_clipRetried = false;
    m_swVerdicts = 0;
    m_clipTimer.start();
    loadCurrent();

    m_timer = new QTimer(this);
    m_timer->setInterval(kPollMs);
    connect(m_timer, &QTimer::timeout, this, &HwProbe::poll);
    m_timer->start();
}

void HwProbe::loadCurrent()
{
    if (!m_mpv)
        return;
    const QByteArray p = m_clipPaths[m_index].toUtf8();
    const char *cmd[] = { "loadfile", p.constData(), "replace", nullptr };
    // New clip, new verdict: neither the previous log nor the previous decode
    // state counts for this clip.
    m_sawHwLog = false;
    m_sawSwLog = false;
    m_sawHwFailed = false;
    m_decoded = false;
    mpv_command(m_mpv, cmd);
    NT_DEBUG("E2E hwprobe load %d=%s\n", m_index, p.constData());
}

// Next codec of the ladder (or end). An "indeterminate" clip also
// advances: aborting left the i5-4590 without ever reaching H.264, its real profile.
void HwProbe::advance()
{
    if (m_done)
        return;
    if (m_index >= kClipCount - 1) {
        endOfLadder();
        return;
    }
    if (m_mpv) {
        const char *stop[] = { "stop", nullptr };
        mpv_command(m_mpv, stop);
    }
    ++m_index;
    m_clipRetried = false;
    m_clipTimer.restart();
    loadCurrent();
}

void HwProbe::endOfLadder()
{
    // H.264 without HW: if some codec gave a real decisive SW, "soft-all" is a
    // verdict (it is persisted); if everything was indeterminate, empty (do not
    // persist, retry at startup).
    if (m_swVerdicts > 0) {
        finish(QStringLiteral("soft-all"));
    } else {
        NT_DEBUG("E2E hwprobe todo indeterminado -> no persistir\n");
        finish(QString());
    }
}

void HwProbe::poll()
{
    if (!m_mpv || m_done)
        return;

    // Verdict of a clip. It requires decoded frames (m_decoded) and that the
    // grace window has passed so the decoder messages arrive (log
    // verdict and HW attempt errors). HW only if
    // the log confirms it; SW and UNKNOWN move along the ladder.
    const auto decide = [this](const QString &hwdec) {
        const QString verdict = hwprobe::clipVerdict(
            m_sawHwLog, m_sawSwLog, m_sawHwFailed, hwdec);
        NT_DEBUG("E2E hwprobe %s hwdec=%s log(hw=%d sw=%d fail=%d) -> %s\n",
                 kClips[m_index].file, hwdec.toUtf8().constData(),
                 m_sawHwLog ? 1 : 0, m_sawSwLog ? 1 : 0,
                 m_sawHwFailed ? 1 : 0, verdict.toUtf8().constData());
        if (verdict == QLatin1String("HW")) {
            finish(QString::fromLatin1(kClips[m_index].profile));
            return;
        }
        if (verdict == QLatin1String("SW"))
            ++m_swVerdicts;
        advance();
    };

    bool ended = false;
    while (true) {
        mpv_event *event = mpv_wait_event(m_mpv, 0);
        if (!event || event->event_id == MPV_EVENT_NONE)
            break;
        if (event->event_id == MPV_EVENT_END_FILE) {
            auto *end = static_cast<mpv_event_end_file *>(event->data);
            if (end && end->reason != MPV_END_FILE_REASON_QUIT
                && end->reason != MPV_END_FILE_REASON_STOP)
                ended = true;
        } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            auto *lm = static_cast<mpv_event_log_message *>(event->data);
            if (lm && lm->text) {
                const QString text = QString::fromUtf8(lm->text);
                if (text.contains(QLatin1String("Using hardware decoding")))
                    m_sawHwLog = true;
                else if (text.contains(QLatin1String("Using software decoding")))
                    m_sawSwLog = true;
                else if (hwAttemptFailed(QString::fromUtf8(lm->prefix), text,
                                         codecOf(m_index)))
                    m_sawHwFailed = true;
            }
        }
    }

    char *hwRaw = nullptr;
    const int r = mpv_get_property(m_mpv, "hwdec-current", MPV_FORMAT_STRING,
                                   &hwRaw);
    const QString hwdec = (r >= 0 && hwRaw) ? QString::fromUtf8(hwRaw)
                                            : QString();
    if (hwRaw)
        mpv_free(hwRaw);

    int64_t h = 0;
    mpv_get_property(m_mpv, "video-params/h", MPV_FORMAT_INT64, &h);
    if (h > 0 && !m_decoded) {
        m_decoded = true;
        m_decodedTimer.restart();
    }

    // 1) First decoded frame + end of grace -> decide.
    if (m_decoded && m_decodedTimer.elapsed() >= kGraceMs) {
        decide(hwdec);
        return;
    }
    // 2) The clip ended with an error WITHOUT decoding: indeterminate.
    if (ended && !m_decoded) {
        NT_DEBUG("E2E hwprobe %s decode-error -> siguiente códec\n",
                 kClips[m_index].file);
        advance();
        return;
    }
    // 3) Clip timeout (slow decoder init on VMs): one retry,
    // and if that fails too, indeterminate (moves to the next codec).
    if (m_clipTimer.elapsed() > kClipTimeoutMs) {
        if (!m_clipRetried) {
            m_clipRetried = true;
            NT_DEBUG("E2E hwprobe retry clip=%s\n", kClips[m_index].file);
            if (m_mpv) {
                const char *stop[] = { "stop", nullptr };
                mpv_command(m_mpv, stop);
            }
            m_clipTimer.restart();
            loadCurrent();
            return;
        }
        NT_DEBUG("E2E hwprobe timeout clip=%s hwdec=%s\n",
                 kClips[m_index].file, hwdec.toUtf8().constData());
        advance();
    }
}

void HwProbe::finish(const QString &profile)
{
    if (m_done)
        return;
    m_done = true;
    if (m_timer) {
        m_timer->stop();
        m_timer->deleteLater();
        m_timer = nullptr;
    }
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
    if (!profile.isEmpty())
        NT_DEBUG("E2E hwprobe profile=%s\n", profile.toUtf8().constData());
    emit finished(profile);
}

#endif // NEVERTUBE_NO_MPV