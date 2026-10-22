#pragma once

// The ARRIVAL phase of a download: before the first fragment can be asked,
// the app needs two things of each track:
//
//   - the INIT segment (the header of the file), and
//   - the INDEX (the sidx), which says where every fragment starts.
//
// Those are four requests (two per track). The fragments may only start when
// ALL of them have answered, because the ranges of a track are not known
// until its index arrives.
//
// WHY this class exists (the bug it fixes): the counter of those requests
// was decremented in TWO places and only ONE of them checked it. So when the
// LAST answer to arrive was an INIT (and not an index), the counter went
// from 1 to 0 with nobody noticing and the fragments never started: the
// download stayed STILL for ever and only moved after a pause/resume (the
// user saw it as "the first download does not start").
//
// The gate has ONE rule: the counter is only touched in ONE place, so the
// order of the answers does not matter.
namespace dl {

class ArrivalGate
{
public:
    // A request of the arrival phase is PLANNED (not necessarily launched:
    // with one connection the second request of a track waits for the
    // first one). The counter counts the PLANNED ones.
    void add() { ++m_pending; }

    // One request of the arrival phase has answered.
    void done()
    {
        if (m_pending > 0)
            --m_pending;
    }

    // A planned request is given up for good (the track does not exist): it
    // does not wait for anything.
    void discard() { done(); }

    // How many are still in flight.
    int pending() const { return m_pending; }

    // Whether the phase is over. It returns TRUE exactly ONCE: the first
    // time the last request has answered. That is the moment the fragments
    // may start.
    bool takeFinished()
    {
        if (m_pending > 0)
            return false;
        if (m_finished)
            return false;   // it already finished: do not start twice
        m_finished = true;
        return true;
    }

    // Whether the phase is over (the same answer as the last takeFinished).
    bool isDone() const { return m_finished; }

    void reset()
    {
        m_pending = 0;
        m_finished = false;
    }

private:
    int m_pending = 0;
    bool m_finished = false;
};

} // namespace dl
