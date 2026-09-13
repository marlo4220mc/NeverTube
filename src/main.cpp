#include "mainwindow.h"

#include <QApplication>
#include <QDir>
#include <QStyleFactory>
#include <QTimer>

#include <cstdio>

#include "db/repositories.h"
#include "player_selftest.h"
#include "yt.h"
#include "yt_innertube.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("nevertube"));
    app.setOrganizationName(QStringLiteral("nevertube"));
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    const QStringList args = app.arguments();
    if (args.contains(QLatin1String("--selftest-db"))) {
        const QString dir = QDir::temp().absoluteFilePath(
            QStringLiteral("nevertube_selftest"));
        QDir().mkpath(dir);
        const QString path =
            dir + QStringLiteral("/mini_tube.db");
        const bool ok = databaseSelfTest(path);
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-discover"))) {
        const QString dir = QDir::temp().absoluteFilePath(
            QStringLiteral("nevertube_selftest"));
        QDir().mkpath(dir);
        const QString path =
            dir + QStringLiteral("/discover.db");
        const bool ok = discoverSelfTest(path);
        std::fprintf(stderr, "SELFTEST discover → %s\n",
                     ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-player"))) {
        return playerSelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-innertube"))) {
        return innertubeSelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-home"))) {
        return homeBrowseSelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-feeds"))) {
        const bool proto = feedsProtocolSelfTest();
        const bool chain = feedsChainSelfTest();
        std::fprintf(stderr, "SELFTEST feeds proto=%s cadena=%s → %s\n",
                     proto ? "OK" : "FAIL", chain ? "OK" : "FAIL",
                     (proto && chain) ? "SELFTEST OK" : "SELFTEST FAIL");
        return (proto && chain) ? 0 : 1;
    }

    MainWindow window;
    window.show();

    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args.at(i);
        if (arg == QLatin1String("--search") && i + 1 < args.size()) {
            const QString query = args.at(++i);
            QTimer::singleShot(400, &window, [&window, query]() {
                window.searchFor(query);
            });
        } else if (arg == QLatin1String("--channel") && i + 1 < args.size()) {
            const QString channelId = args.at(++i);
            QTimer::singleShot(500, &window, [&window, channelId]() {
                window.openChannel(channelId);
            });
        } else if (arg == QLatin1String("--library")) {
            QTimer::singleShot(500, &window, [&window]() {
                window.openLibrary();
            });
        } else if (arg == QLatin1String("--playlist") && i + 1 < args.size()) {
            const QString playlistId = args.at(++i);
            QTimer::singleShot(500, &window, [&window, playlistId]() {
                window.openPlaylist(playlistId,
                                    QStringLiteral("Mi playlist de demo"));
            });
        } else if (arg.startsWith(QLatin1String("http"))) {
            const QString url = arg;
            QTimer::singleShot(500, &window, [&window, url]() {
                window.playVideo(url);
            });
        }
    }

    return app.exec();
}