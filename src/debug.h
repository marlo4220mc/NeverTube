#ifndef NT_DEBUG_H
#define NT_DEBUG_H

#include <cstdio>
#include <cstdlib>

#include <QByteArray>
#include <QDateTime>

namespace nt {

// Debug logging gated by the NEVERTUBE_DEBUG environment variable.
// Off by default so it does not pollute stderr in the player's 33 ms loop.
// Re-enable without recompiling: NEVERTUBE_DEBUG=1 ./nevertube
// Mimics a variadic macro: nt::debug("E2E phase=%s\n", s).
inline bool debugEnabled()
{
    static const bool enabled =
        qEnvironmentVariableIsSet("NEVERTUBE_DEBUG") &&
        qEnvironmentVariable("NEVERTUBE_DEBUG") != QLatin1String("0");
    return enabled;
}

// Monotonic timestamp in ms since the first NT_DEBUG of the session. It is
// printed in front of each line (e.g. "+1234 ms") to measure startup, buffering
// and micro-pause timings without relying on external clocks.
inline long long debugMs()
{
    static const qint64 start = QDateTime::currentMSecsSinceEpoch();
    return QDateTime::currentMSecsSinceEpoch() - start;
}

} // namespace nt

#define NT_DEBUG(...)                                            \
    do {                                                         \
        if (::nt::debugEnabled())                                \
            std::fprintf(stderr, "+%lld ms ", ::nt::debugMs()),  \
                std::fprintf(stderr, __VA_ARGS__);               \
    } while (0)

#endif
