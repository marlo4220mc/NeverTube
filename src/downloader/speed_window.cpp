#include "speed_window.h"

namespace dl {

void SpeedWindow::clear()
{
    m_samples.clear();
}

void SpeedWindow::add(qint64 nowMs, qint64 bytesDone)
{
    m_samples.append({nowMs, bytesDone});
    // The samples that fall out of the window go, but ONE older one is kept as
    // the base of the subtraction: without it the first sample inside the
    // window has nothing to be compared with and the speed would read 0 at
    // every tick.
    while (m_samples.size() > 2 && m_samples.at(1).ms <= nowMs - kWindowMs)
        m_samples.removeFirst();
}

qint64 SpeedWindow::bytesPerSecond(qint64 nowMs) const
{
    if (m_samples.size() < 2)
        return 0;
    const Sample &first = m_samples.first();
    const Sample &last = m_samples.last();
    const qint64 elapsed = last.ms - first.ms;
    // Under a quarter of a second the number is noise (and it divides by a
    // very small number, which gives absurd speeds).
    if (elapsed < 250)
        return 0;
    const qint64 advanced = last.bytes - first.bytes;
    if (advanced <= 0)
        return 0;
    Q_UNUSED(nowMs);
    return advanced * 1000 / elapsed;
}

} // namespace dl
