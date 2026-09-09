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
        if (player->hwdec() != QStringLiteral("auto")) {
            std::fprintf(stderr,
                         "SELFTEST FAIL: hwdec esperado 'auto', obtenido '%s'\n",
                         player->hwdec().toUtf8().constData());
            return false;
        }
        if (player->redrawIntervalMs() != 33) {
            std::fprintf(stderr,
                         "SELFTEST FAIL: timer redibujado esperado 33 ms, "
                         "obtenido %d ms\n",
                         player->redrawIntervalMs());
            return false;
        }
        return true;
    };

    QObject::connect(&timeout, &QTimer::timeout, &host, [&]() {
        if (check()) {
            std::fprintf(stderr, "SELFTEST OK (hwdec=auto, timer=33ms)\n");
            ok = true;
        }
        QCoreApplication::exit(ok ? 0 : 1);
    });

    timeout.start(timeoutMs);
    QCoreApplication::exec();
    return ok;
}