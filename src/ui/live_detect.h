#ifndef NT_LIVE_DETECT_H
#define NT_LIVE_DETECT_H

#include <QtGlobal>
#include <QDateTime>

namespace nt {

// Detection of "live" (a medium without a final duration) for the player bar
// (overlay and mini player, one detector per surface). A
// YouTube live stream has no final duration: mpv reports 0 (pure HLS live)
// or the "up to now" duration, which grows with each incoming segment and
// never settles. A VOD materializes its duration at startup (no
// change or just one) and then keeps it CONSTANT, so the duration that
// ONLY GROWS is the signal and the STATE is decided with evidence, not with
// a single tick:
//
//   - duration that GROWS by more than kChangeS between ticks (dur>0 on both) TWO
//     times (kLockChanges): a VOD does not grow twice; a live stream grows
//     with each segment, indefinitely. Only RISES count: a
//     decrease or a duration collapse (a VOD on reload resets its own:
//     e.g. 17659.5 -> 0 -> 1213.6 — it is the SAME finite video materializing
//     again) is never a live-stream signal. The branch
//     "dur=0 with advanced position" that flagged normal slow-start videos
//     is not used either: it is impossible for a VOD to reach 2 rises, so no
//     normal video is flagged.
//
// Automatic unlock (safety net): if in live mode a
// STABLE duration arrives (>0, no significant rises during kStableUnlockMs),
// it returns to the normal bar. A real live stream KEEPS growing the duration
// with the segments, so it never unlocks by itself; a video wrongly flagged
// is released after ~15 s and recovers the seek.
//
// Note: the synthetic AV1 HLS (finite fMP4 with a VOD+ENDLIST playlist) is NOT
// fed here — by construction it is not a live stream and the backend reports
// unstable durations (rises and resets) while it loads the BYTERANGE
// ranges. The surface excludes it with currentCanBeLive()==false.
class LiveDetector {
public:
    static constexpr double kChangeS = 0.5;
    static constexpr int kLockChanges = 2;
    static constexpr qint64 kStableUnlockMs = 15000;

    // New video: detection starts from scratch (the duration is received anew
    // and the previous video's state is not carried over).
    void reset()
    {
        m_changes = 0;
        m_live = false;
        m_lastDur = -1.0;
        m_lastChangeMs = 0;
    }

    bool isLive() const { return m_live; }

    // IMMEDIATE entry through the authoritative signal (videoDetails.isLive of
    // YouTube's player response, when the surface knows it). Avoids waiting
    // for the kLockChanges duration rises, and covers PURE live streams
    // (dur=0 constant), which by definition never produce rises. The exit
    // remains governed by the stability unlock: the last-change clock
    // is armed from NOW, so a live stream that ends (the duration settles and
    // stops growing) returns to the normal bar just as without this function.
    void forceLive()
    {
        m_live = true;
        m_lastChangeMs = nowMs();
    }

    // Feed on each position tick (~1/s). Returns the medium state
    // (true = live) after evaluating the tick.
    bool feed(double duration)
    {
        const qint64 now = nowMs();
        const double prevDur = m_lastDur;
        if (duration > 0)
            m_lastDur = duration;

        if (m_live) {
            // Already live: refresh the last-change clock and unlock
            // if the duration stabilizes (false positive -> normal bar).
            if (prevDur > 0 && duration > prevDur + kChangeS) {
                m_lastChangeMs = now;
            } else if (duration > 0 && now - m_lastChangeMs >= kStableUnlockMs) {
                m_live = false;
            }
            return m_live;
        }

        // Only RISES of the duration count. A decrease is not a signal: a
        // VOD on reload collapses/resets its own (17659.5 -> 0 -> 1213.6) and a
        // live stream never goes down — it only grows it with each segment.
        if (prevDur > 0 && duration > prevDur + kChangeS) {
            ++m_changes;
            m_lastChangeMs = now;
            if (m_changes >= kLockChanges)
                m_live = true;
        }
        return m_live;
    }

private:
    static qint64 nowMs()
    {
        static const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        return QDateTime::currentMSecsSinceEpoch() - t0;
    }

    int m_changes = 0;
    bool m_live = false;
    double m_lastDur = -1.0;
    qint64 m_lastChangeMs = 0;
};

} // namespace nt

#endif // NT_LIVE_DETECT_H