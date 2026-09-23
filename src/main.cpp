#include "mainwindow.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QSslSocket>
#include <QStyleFactory>
#include <QTimer>

#include <cstdio>

#include "db/repositories.h"
#include "hwprobe.h"
#include "player_selftest.h"
#include "yt.h"
#include "yt_innertube.h"

#ifdef Q_OS_WIN
#include <windows.h>
namespace {
// La app está marcada WIN32_EXECUTABLE_TARGET (sin consola propia). Para que
// los selftests impriman y el CI/la máquina de pruebas vean los logs, nos pegamos a la
// consola del proceso padre (shell del CI) o, en doble clic con
// NEVERTUBE_DEBUG=1 (depuración explícita), creamos una consola propia. En un
// lanzamiento normal con doble clic NO se abre ninguna ventana de terminal.
// Núcleo del reproductor no se toca: es arranque de la app.
void ensureConsoleAttached()
{
    // SI CI/redirección ya nos dio un stdout válido (pipe heredada del runner vía
    // Start-Process -RedirectStandardOutput), NO tocamos nada: el runner captura
    // esa pipe. Adjuntar/AllocConsole + freopen(CONOUT$) robaría el output.
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE)
        return;
    if (GetConsoleWindow())
        return; // ya tenemos (o hay) consola
    bool ok = false;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) { // shell/CI: hereda la consola del padre
        ok = true;
    } else if (qEnvironmentVariableIsSet("NEVERTUBE_DEBUG")) {
        // Doble clic SIN consola madre: la app normal no abre terminal. Solo una
        // sesión de depuración explícita (NEVERTUBE_DEBUG=1) abre consola propia.
        ok = (AllocConsole() != FALSE);
    }
    if (ok) {
        FILE *f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        freopen_s(&f, "CONIN$", "r", stdin);
    }
}
} // namespace
#endif // Q_OS_WIN

// SELFTEST TLS con handshake REAL. El viejo --selftest-ssl solo cargaba las
// DLLs de OpenSSL (supportsSsl): un backend que carga pero aborta en el
// primer uso real (p. ej. "OpenSSL internal error, assertion failed:
// lock != NULL", conocido en Windows con Qt 5.15.2 + OpenSSL 1.1.1 en una
// máquina x86 de 32 bits) pasaba desapercibido en CI. Este selftest abre una
// conexión TLS real contra hosts públicos y comprueba que el engine completa
// el handshake; si OpenSSL aborta internamente, el proceso muere con abort()
// y el CI lo marca rojo con el mensaje del assert en el log.
static bool tlsHandshakeSelfTest(QString *outDetail)
{
    const bool supported = QSslSocket::supportsSsl();
    std::fprintf(stderr, "SELFTEST tls: supportsSsl=%s build=%s runtime=%s\n",
                 supported ? "yes" : "no",
                 QSslSocket::sslLibraryBuildVersionString().toUtf8().constData(),
                 QSslSocket::sslLibraryVersionString().toUtf8().constData());
    if (!supported) {
        *outDetail =
            QStringLiteral("backend TLS no disponible (DLLs OpenSSL no cargan)");
        return false;
    }

    const char *const hosts[] = {
        "www.youtube.com",
        "www.google.com",
        "example.com",
    };
    for (const char *hostC : hosts) {
        const QString host = QString::fromUtf8(hostC);
        QSslSocket socket;
        socket.setPeerVerifyMode(QSslSocket::VerifyNone); // testea el engine, no certs
        socket.ignoreSslErrors();
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        bool encrypted = false;
        QString err;
        QObject::connect(&socket, &QSslSocket::encrypted, &loop,
                         [&]() {
                             encrypted = true;
                             loop.quit();
                         });
        QObject::connect(&socket,
                         QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::errorOccurred),
                         &loop, [&](QAbstractSocket::SocketError) {
                             err = socket.errorString();
                             loop.quit();
                         });
        QObject::connect(&timer, &QTimer::timeout, &loop,
                         [&]() {
                             err = QStringLiteral("timeout 15s");
                             loop.quit();
                         });
        std::fprintf(stderr, "SELFTEST tls: handshake %s ...\n", hostC);
        timer.start(15000);
        socket.connectToHostEncrypted(host, 443);
        loop.exec();
        if (encrypted) {
            std::fprintf(stderr, "SELFTEST tls: %s -> ENCRYPTED OK\n", hostC);
            *outDetail = QString::fromUtf8(hostC);
            return true;
        }
        std::fprintf(stderr, "SELFTEST tls: %s -> %s\n", hostC,
                     err.isEmpty() ? "sin encrypted ni error"
                                   : err.toUtf8().constData());
    }
    *outDetail =
        QStringLiteral("ningún host completó el handshake TLS");
    return false;
}

// Tabla de decisión del perfil HW probada offline (independiente de la GPU de
// este equipo): reemplaza medidas los casos reales observados en máquinas que
// no tenemos aquí (i5-4590/HD4600 con fallo av1_qsv -> SW; GMA 950 -> SW;
// GPUs con HW real -> HW). Regresión determinista para CI.
static bool hwprobeVerdictSelfTests()
{
    struct Case
    {
        bool hw;
        bool sw;
        bool fail;
        const char *hwdec;
        const char *want;
    };
    const Case cases[] = {
        // GPU real: AV1 por Vulkan (p. ej. este equipo Linux).
        { true, false, false, "vulkan-copy", "HW" },
        // GPU real: D3D11.
        { true, false, false, "d3d11va-copy", "HW" },
        // Respaldo: backend activo sin señales en contra (tras la gracia).
        { false, false, false, "dxva2-copy", "HW" },
        // GMA 950: mpv confirmó software aunque haya un dispositivo WARP.
        { false, true, false, "warp", "SW" },
        // i5-4590/HD4600: el intento av1_qsv falló (unsupported) -> SW, aunque
        // hwdec-current reporte qsv-copy (el falso positivo original).
        { false, false, true, "qsv-copy", "SW" },
        // i5: tras el fallo QSV hwdec-current termina en "no" -> SW.
        { false, false, true, "no", "SW" },
        // Log de software domina aunque antes hubiera una línea HW (cambio de
        // decoder durante la gracia) -> SW.
        { true, true, false, "vulkan-copy", "SW" },
        // hwdec-current == "no" es decisivo aunque hubiera línea HW -> SW.
        { true, false, false, "no", "SW" },
        // Sin ninguna señal: indeterminado -> continuar la escalera.
        { false, false, false, "", "" },
    };
    for (const Case &c : cases) {
        const QString got =
            hwprobe::clipVerdict(c.hw, c.sw, c.fail,
                                 QString::fromUtf8(c.hwdec));
        if (got != QLatin1String(c.want)) {
            std::fprintf(stderr,
                         "SELFTEST hwprobe verify FAIL hw=%d sw=%d fail=%d "
                         "hwdec=%s want=%s got=%s\n",
                         c.hw ? 1 : 0, c.sw ? 1 : 0, c.fail ? 1 : 0, c.hwdec,
                         c.want, got.toUtf8().constData());
            return false;
        }
    }
    std::fprintf(stderr, "SELFTEST hwprobe verify -> ok (9 casos)\n");
    return true;
}

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    ensureConsoleAttached();
#endif
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(resources);
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

    if (args.contains(QLatin1String("--selftest-av1hls"))) {
        return av1HlsSelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-hwprobe"))) {
        // Offline: la tabla de decisión con los casos REALES observados
        // (i5/HD4600 con fallo av1_qsv, GMA 950 software, GPU con HW) no
        // depende de la GPU de este equipo; es una regresión CI determinista.
        if (!hwprobeVerdictSelfTests())
            return 1;
        // Diagnóstico dependiente de la GPU: imprime el perfil detectado
        // (av1-hw/vp9-hw/h264-hw/soft-all) o INDETERMINADO. No es un assert
        // fijo, sino una comprobación de que el sondeo concluye.
        HwProbe probe;
        bool decided = false;
        int rc = 1;
        QObject::connect(&probe, &HwProbe::finished, &app,
                         [&](const QString &profile) {
                             decided = true;
                             std::fprintf(stderr, "SELFTEST hwprobe -> %s\n",
                                          profile.isEmpty()
                                              ? "INDETERMINADO"
                                              : profile.toUtf8().constData());
                             rc = profile.isEmpty() ? 1 : 0;
                             app.quit();
                         });
        QTimer::singleShot(50000, &app, [&]() {
            if (!decided) {
                std::fprintf(stderr, "SELFTEST hwprobe -> TIMEOUT\n");
                app.quit();
            }
        });
        probe.start();
        app.exec();
        return rc;
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

    if (args.contains(QLatin1String("--selftest-search"))) {
        const bool ok = searchSelfTest();
        std::fprintf(stderr, "SELFTEST search -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-browse"))) {
        const bool ok = innertubeBrowseSelfTest();
        std::fprintf(stderr, "SELFTEST browse -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-tls"))) {
        QString detail;
        const bool ok = tlsHandshakeSelfTest(&detail);
        std::fprintf(stderr, "SELFTEST tls -> %s (%s)\n",
                     ok ? "OK" : "FAIL", detail.toUtf8().constData());
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-ssl"))) {
        // Backend TLS (Windows: OpenSSL 1.1 en DLLs al lado del exe). Si las
        // DLLs de OpenSSL faltan o no cargan, supportsSsl() es false y todas
        // las llamadas HTTPS fallan con "TLS initialization failed".
        const bool ok = QSslSocket::supportsSsl();
        std::fprintf(stderr, "SELFTEST ssl -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
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