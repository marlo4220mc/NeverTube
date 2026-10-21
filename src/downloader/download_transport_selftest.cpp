#include "download_transport_selftest.h"

#include "download_transport.h"

#include <cstdio>

bool downloadTransportSelfTest()
{
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        std::printf("  DOWNLOAD-TRANSPORT %s: %s\n", cond ? "OK" : "FAIL",
                     what);
        if (!cond)
            ok = false;
    };

    // --- the rules of the discard of a slow fragment ---
    //
    // The CDN serves SOME of the requests at a crawl (a few KB/s). Those
    // hold a connection of the pool for the whole request and the speed of
    // the download falls (measured: it started at 45 MB/s and settled at
    // 20). They are discarded: the fragment is asked again.
    //
    // The rule is NOT "no byte in N seconds" (that killed the fragments
    // that were arriving fine but with a gap): it is "not even the X% of
    // the expected bytes in N seconds".
    check(DownloadTransport::stallTimeoutMs() > 0,
          "el descarte de un fragmento lento tiene un plazo");
    check(DownloadTransport::stallTimeoutMs() <= 15000,
          "el plazo es corto (10-15 s): un fragmento a crawl no puede retener "
          "una conexion mucho tiempo");
    check(DownloadTransport::stallMinRatio() > 0.0
              && DownloadTransport::stallMinRatio() <= 0.25,
          "el minimo es una fraccion del tamano esperado (5-25%): un "
          "fragmento que avanza un poco NO se descarta");

    // --- the rest of the transport ---
    check(!DownloadTransport::userAgent().isEmpty(),
          "el transporte tiene un user agent");
    check(DownloadTransport::defaultConcurrency() >= 4
              && DownloadTransport::defaultConcurrency() <= 16,
          "las conexiones por defecto estan entre 4 y 16");

    std::printf("DOWNLOAD-TRANSPORT SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}
