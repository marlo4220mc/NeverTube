#ifndef HWPROBE_H
#define HWPROBE_H

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

struct mpv_handle;

class QTimer;

namespace hwprobe {

// Verdict for a clip after the first decoded frame and the end of the
// grace window. Deciding BEFORE the decoder speaks up is the
// origin of the HW false positives (e.g. av1-hw on i5/HD4600 QSV: the
// probe saw hwdec-current=qsv-copy and the "av1_qsv: unsupported" errors +
// the fall back to libdav1d arrived 5 ms after the verdict).
//
//   - "HW": mpv confirmed hardware ("Using hardware decoding") and NOTHING
//     contradicts it (neither a fallback to software nor a failed HW attempt
//     nor hwdec=no). The backend active in hwdec-current with no signals
//     against is a fallback for GPUs whose log did not arrive (same behavior
//     as before, but with the grace already consumed).
//   - "SW": mpv confirmed software ("Using software decoding", a HW attempt of
//     the codec that failed in ffmpeg, or hwdec-current == "no").
//   - "":   undetermined; the caller continues the ladder without concluding.
QString clipVerdict(bool sawHwLog, bool sawSwLog, bool sawHwFailed,
                    const QString &hwdecCurrent);

// Profile descent ladder (degradation by playback). Returns the profile
// immediately below the given one, or empty if it does not apply.
//
// It is used ONLY with codec/HW evidence: the active profile promises hardware
// for the codec being played and mpv decodes in SOFTWARE. The network cannot
// produce that signal (a 403, an expired visitorData or a cache stall do not
// change hwdec-current), so the ladder NEVER degrades because of the network.
//   av1-hw -> vp9-hw -> h264-hw -> soft-all -> (empty: it is the floor)
QString lowerProfile(const QString &profile);

} // namespace hwprobe

// The probing machinery uses mpv (headless clips): the class only exists in
// builds with mpv. In the x86 build (NEVERTUBE_NO_MPV) the profile is decided
// per platform (DXVA2 -> h264-hw) without HwProbe; without this guard, AUTOMOC
// generates the Q_OBJECT with the poll() slot and linking fails (LNK2019: poll
// undefined).
#ifndef NEVERTUBE_NO_MPV

// Probing of hardware decoding capabilities at startup. Plays in
// headless (vo=null, hwdec=auto-copy) one micro-clip per codec in order
// AV1 -> VP9 -> H.264 from resources/clips.
//
// The HW verdict comes from the mpv LOG (vd and ffmpeg modules), not from
// hwdec-current: mpv emits "Using hardware decoding (<api>)" /
// "Using software decoding" and, when a HW attempt fails (e.g. av1_qsv on
// HD 4600), the errors are seen in ffmpeg. hwdec-current remains only as a
// fallback. Rule (the same for any GPU):
//   - first codec with a "HW" verdict -> "av1-hw" / "vp9-hw" / "h264-hw"
//   - all "SW"            -> "soft-all" (persisted: it is a real verdict)
//   - no reliable verdict -> empty profile (transient soft-all without
//     persisting; the probe is retried on the next startup)
// An "undetermined" result does NOT abort: it keeps testing the following
// codecs (the i5-4590 reaches H.264 this way, which it does decode in HW).
class HwProbe : public QObject
{
    Q_OBJECT

public:
    explicit HwProbe(QObject *parent = nullptr);
    ~HwProbe() override;

    void start();

signals:
    // Detected profile: "av1-hw" | "vp9-hw" | "h264-hw" | "soft-all".
    // Empty if it could not be determined reliably: the caller keeps the
    // conservative default without persisting and the probe is retried at
    // startup.
    void finished(const QString &profileName);

private slots:
    void poll();

private:
    void loadCurrent();
    void advance();
    void endOfLadder();
    void finish(const QString &profile);

    mpv_handle *m_mpv = nullptr;
    QTimer *m_timer = nullptr;
    QTemporaryDir m_tmpDir;
    QElapsedTimer m_clipTimer;
    QString m_clipPaths[3];
    int m_index = 0;
    bool m_done = false;
    // Verdict for the current clip (reset on each loadCurrent). The decoder
    // log can be queued 1-10 ms AFTER the first video-params; that is why the
    // grace (m_decoded + m_decodedTimer) is awaited before concluding.
    bool m_sawHwLog = false;
    bool m_sawSwLog = false;
    bool m_sawHwFailed = false; // the codec's HW attempt failed (ffmpeg)
    bool m_decoded = false;     // first frame of the current clip
    QElapsedTimer m_decodedTimer;
    // Number of clips with a decisive SW verdict: upon reaching h264 without
    // HW, if there was some conclusive SW, soft-all is persisted (real
    // verdict); if everything was undetermined, empty (without persisting).
    int m_swVerdicts = 0;
    // One retry per clip if the timeout expires (slow hw init in VMs).
    bool m_clipRetried = false;
};

#endif // NEVERTUBE_NO_MPV

#endif