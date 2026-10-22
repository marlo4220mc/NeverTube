#include "download_transport_selftest.h"

#include "arrival_gate.h"
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

    // --- the ARRIVAL phase: the fragments start no matter the order ---
    //
    // The bug this covers: the counter of the arrival requests (the init
    // and the index of each track) was decremented in two places and only
    // ONE of them checked it. When the LAST answer to arrive was an INIT
    // (and not an index) the counter went from 1 to 0 with nobody
    // noticing, so the fragments never started: the download stayed still
    // for ever and only moved after a pause/resume.
    {
        // The order that used to break it: both indexes first, then the
        // inits, with the init of the video being the LAST one.
        dl::ArrivalGate g;
        g.add();   // init del video
        g.add();   // indice del video
        g.add();   // init del audio
        g.add();   // indice del audio
        int started = 0;
        g.done(); started += g.takeFinished() ? 1 : 0;   // indice del video
        g.done(); started += g.takeFinished() ? 1 : 0;   // indice del audio
        g.done(); started += g.takeFinished() ? 1 : 0;   // init del audio
        g.done(); started += g.takeFinished() ? 1 : 0;   // init del video
        check(started == 1,
              "la llegada termina aunque la ultima respuesta sea un init "
              "(el orden que rompia la descarga)");
        check(g.isDone(), "la puerta sabe que la llegada termino");
        check(!g.takeFinished(),
              "la llegada avisa UNA sola vez (no hay arranque doble)");
    }
    {
        // The other order: everything in the natural order.
        dl::ArrivalGate g;
        g.add();
        g.add();
        g.add();
        g.add();
        int started = 0;
        for (int i = 0; i < 4; ++i) {
            g.done();
            started += g.takeFinished() ? 1 : 0;
        }
        check(started == 1, "la llegada termina en el orden natural");
    }
    {
        // The counter goes by the number of requests.
        dl::ArrivalGate g;
        g.add();
        g.add();
        check(g.pending() == 2, "el contador va por el numero de peticiones");
        check(!g.takeFinished(), "con dos peticiones en vuelo NO ha terminado");
        g.done();
        check(g.pending() == 1, "una respuesta descuenta UNA");
        check(!g.takeFinished(), "todavia queda una");
        g.done();
        check(g.pending() == 0, "el contador llega a cero");
        check(g.takeFinished(), "la llegada termino");
        check(g.isDone(), "la puerta sabe que la llegada termino");
    }
    {
        // The gate starts clean for every download.
        dl::ArrivalGate g;
        g.add();
        g.add();
        g.reset();
        check(g.pending() == 0 && !g.isDone(),
              "la puerta se reinicia para cada descarga");
        g.add();
        g.add();
        check(!g.takeFinished(), "despues del reset hay que esperar de nuevo");
    }
    {
        // A planned request that is given up (the track does not exist).
        dl::ArrivalGate g;
        g.add();
        g.add();
        g.discard();
        check(g.pending() == 1,
              "una peticion abandonada no espera a nadie");
    }

    std::printf("DOWNLOAD-TRANSPORT SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}
