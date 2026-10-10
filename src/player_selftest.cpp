#include "player_selftest.h"

#include <cstdio>

#include <QCoreApplication>
#include <QTimer>
#include <QWidget>

#include "playerwidget.h"

bool playerSelfTest()
{
    QWidget host;
    host.setWindowTitle(QStringLiteral("nevertube selftest-player"));
    host.resize(640, 360);
    host.setAttribute(Qt::WA_DeleteOnClose, false);

    auto *player = new PlayerWidget(&host);
    player->setGeometry(0, 0, 640, 360);
    host.show();

    bool ok = false;
    const int timeoutMs = 1500;
    QTimer timeout;
    timeout.setSingleShot(true);

    auto check = [&]() {
#ifdef NEVERTUBE_NO_MPV
        // x86 build without mpv (Phase 5): the protected core (mpv's hwdec=auto)
        // does not live here — the engine is FfmpegBackend and its real gate is
        // --selftest-ffmpeg. What does apply is verified: that the widget's
        // GL surface is ready (initializeGL) and the backend is alive. The
        // redraw is event-driven (frameReady per engine frame), with no
        // poll timer.
        if (!player->renderContextReady()) {
            std::fprintf(stderr,
                         "SELFTEST FAIL: contexto de render no creado\n");
            return false;
        }
        std::fprintf(stderr,
                     "SELFTEST OK (backend ffmpeg, redibujado por eventos)\n");
#else
        if (player->hwdec() != QStringLiteral("auto")) {
            std::fprintf(stderr,
                         "SELFTEST FAIL: hwdec esperado 'auto', obtenido '%s'\n",
                         player->hwdec().toUtf8().constData());
            return false;
        }
        // initializeGL creates the render context AND installs the update callback
        // (mpv_render_context_create + set_update_callback go together in
        // MpvBackend::initGL): renderContextReady() true after showing the
        // window proves the event-driven redraw path is active.
        if (!player->renderContextReady()) {
            std::fprintf(stderr,
                         "SELFTEST FAIL: contexto de render no creado\n");
            return false;
        }
        std::fprintf(
            stderr, "SELFTEST OK (hwdec=auto, redibujado por eventos)\n");
#endif
        return true;
    };

    QObject::connect(&timeout, &QTimer::timeout, &host, [&]() {
        ok = check();
        QCoreApplication::exit(ok ? 0 : 1);
    });

    timeout.start(timeoutMs);
    QCoreApplication::exec();
    return ok;
}