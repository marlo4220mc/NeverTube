#pragma once

#include <QVector>

namespace dl {

// The speed of a download over a WINDOW of the last seconds, not the average
// since it started.
//
// The average does not answer the question the person is asking ("is it going
// well NOW?"): a download that was fast and then got stuck keeps showing a
// high number for minutes, and after a resume it counts the bytes of the
// previous session (which were not moved in this one).
class SpeedWindow
{
public:
    // The window. 10 s is long enough for a page of fragments to be measured
    // and short enough for a stall to show up while it is happening.
    static constexpr qint64 kWindowMs = 10000;

    void clear();
    // Adds the TOTAL of bytes downloaded at that moment (the window only keeps
    // the samples of the last kWindowMs).
    void add(qint64 nowMs, qint64 bytesDone);
    // Bytes per second over the window (0 when there is not enough data, or
    // when nothing advanced in it).
    qint64 bytesPerSecond(qint64 nowMs) const;
    // 0 when there is no measurement yet.
    int sampleCount() const { return m_samples.size(); }

private:
    struct Sample
    {
        qint64 ms = 0;
        qint64 bytes = 0;
    };
    QVector<Sample> m_samples;
};

} // namespace dl
