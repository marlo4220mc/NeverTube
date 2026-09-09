#ifndef NT_DEBUG_H
#define NT_DEBUG_H

#include <cstdio>
#include <cstdlib>

#include <QByteArray>

namespace nt {

// Logging de depuración controlado por la variable de entorno NEVERTUBE_DEBUG.
// Desactivado por defecto para no ensuciar stderr en el bucle de 33 ms del
// reproductor. Reactivar sin recompilar: NEVERTUBE_DEBUG=1 ./nevertube_qt2
// Imita una macro variádica: nt::debug("E2E phase=%s\n", s).
inline bool debugEnabled()
{
    static const bool enabled =
        qEnvironmentVariableIsSet("NEVERTUBE_DEBUG") &&
        qEnvironmentVariable("NEVERTUBE_DEBUG") != QLatin1String("0");
    return enabled;
}

} // namespace nt

#define NT_DEBUG(...)                                            \
    do {                                                         \
        if (::nt::debugEnabled())                                \
            std::fprintf(stderr, __VA_ARGS__);                   \
    } while (0)

#endif
