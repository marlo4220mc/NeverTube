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
#include "ffmpeg_selftest.h"
#include "ui/video_list_view.h"
#include "ui/thumbnail_cache.h"
#include "ui/pages/search_page.h"
#include "ui/theme.h"
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

    // Escalera de descenso de perfil (hwprobe::lowerProfile), regresión pura.
    struct LadderCase
    {
        const char *in;
        const char *want;
    };
    const LadderCase ladder[] = {
        { "av1-hw", "vp9-hw" },
        { "vp9-hw", "h264-hw" },
        { "h264-hw", "soft-all" },
        { "soft-all", "" },    // suelo: no procede
        { "", "" },            // sin perfil: no procede
        { "desconocido", "" }, // perfil raro: no se toca
    };
    for (const LadderCase &c : ladder) {
        const QString got = hwprobe::lowerProfile(QString::fromUtf8(c.in));
        if (got != QLatin1String(c.want)) {
            std::fprintf(stderr,
                         "SELFTEST hwprobe ladder FAIL in=%s want='%s' got='%s'\n",
                         c.in, c.want, got.toUtf8().constData());
            return false;
        }
    }
    std::fprintf(stderr, "SELFTEST hwprobe ladder -> ok (6 casos)\n");
    return true;
}

// Tope de columnas de las rejillas del Home (VideoListView::clampGridColumns).
// El ajuste "Tamaño de las miniaturas" pide 4/5/6 columnas fijas y la rejilla es
// dimensionada por el ancho, así que en ventanas estrechas hay que recortar: con
// 6 columnas a 700 px la celda mide 113 px y el título de 2 líneas + el canal se
// recortan. Es una función pura, sin widget ni red: se testea aquí.
static bool gridGeometrySelfTest()
{
    struct Case
    {
        int wanted;
        int vw;
        int want;
        const char *why;
    };
    const Case cases[] = {
        // Ancho de la máquina de pruebas (portátil 1024x600): 6 columnas
        // caben (celda de 147 px), que es justo lo que busca "Pequeñas".
        { 6, 903, 6, "6 pedidas caben a 903 px (celda 147)" },
        { 5, 903, 5, "5 pedidas a 903 px" },
        { 4, 903, 4, "4 pedidas a 903 px (celda 222)" },
        // Ventana estrecha: 6 columnas darían 113 px -> se baja a 5 (136 px).
        { 6, 700, 5, "6 recortadas a 5 a 700 px (celda 136)" },
        { 6, 600, 4, "6 recortadas a 4 a 600 px (celda 147)" },
        // Muy estrecha: nunca menos de 3 columnas.
        { 6, 380, 3, "suelo de 3 columnas a 380 px" },
        { 4, 380, 3, "4 recortadas al suelo de 3" },
        // "Grandes" nunca sube de 4 aunque la ventana sea enorme.
        { 4, 1850, 4, "4 pedidas a 1850 px (no se estira a 8)" },
        // Ancho aún desconocido (layout inicial): no se recorta; el
        // resizeEvent del Home lo ajustará cuando lo haya.
        { 6, 0, 6, "sin ancho no se recorta" },
        { 6, -1, 6, "ancho negativo no se recorta" },
        // Entradas degeneradas: nunca 0 columnas.
        { 0, 903, 1, "0 pedidas -> 1" },
        { -3, 903, 1, "negativo pedido -> 1" },
    };
    for (const Case &c : cases) {
        const int got = VideoListView::clampGridColumns(c.wanted, c.vw);
        if (got != c.want) {
            std::fprintf(stderr,
                         "SELFTEST grid FAIL wanted=%d vw=%d want=%d got=%d (%s)\n",
                         c.wanted, c.vw, c.want, got, c.why);
            return false;
        }
    }
    // El ancho mínimo por defecto (kMinCellW) es la razón del tope: por debajo
    // la celda no cabe el bloque de texto completo.
    if (VideoListView::kMinCellW < 100) {
        std::fprintf(stderr, "SELFTEST grid FAIL kMinCellW=%d (<100)\n",
                     VideoListView::kMinCellW);
        return false;
    }

    // Texto de la celda derivado del ancho. Antes eran 13 px y 34 px FIJOS con la
    // miniatura creciendo al estirar la ventana, que es lo que dejado el bloque
    // descoyente en pantalla grande. Se comprueba la tabla de tamaños, que el
    // interlineado quepa la fuente, que el bloque guardado sea el que ocupa, que
    // a celda de 140 px siga midiendo lo que medía antes (68 px: nada cambia en
    // ventana estrecha) y que la curva sea monótona.
    struct TxtCase
    {
        int cellW;
        int px;
        const char *why;
    };
    const TxtCase txts[] = {
        { 0, 13, "celda sin ancho -> 13 px" },
        { 100, 13, "celda diminuta se queda en 13 px (tope inferior)" },
        { 140, 13, "140 px -> 13 px" },
        { 175, 14, "175 px (5 columnas a 903 px de viewport)" },
        { 220, 15, "220 px (4 columnas a 903 px)" },
        { 260, 16, "260 px" },
        { 300, 17, "300 px" },
        { 340, 18, "340 px -> 18 px" },
        { 370, 18, "370 px (5 columnas a pantalla grande) -> tope superior" },
        { 900, 18, "celda enorme -> tope superior" },
    };
    for (const TxtCase &t : txts) {
        const int got = VideoListView::gridTitlePx(t.cellW);
        if (got != t.px) {
            std::fprintf(stderr,
                         "SELFTEST grid FAIL gridTitlePx(%d)=%d want=%d (%s)\n",
                         t.cellW, got, t.px, t.why);
            return false;
        }
        const int lh = VideoListView::gridTitleLineH(t.cellW);
        if (lh < got) {
            std::fprintf(stderr,
                         "SELFTEST grid FAIL lineH(%d)=%d < px=%d: la línea no "
                         "cabe la fuente\n",
                         t.cellW, lh, got);
            return false;
        }
        const int bh = VideoListView::gridTextBlockH(t.cellW);
        if (bh != VideoListView::kGridTitleLines * lh + 34) {
            std::fprintf(stderr,
                         "SELFTEST grid FAIL blockH(%d)=%d incoherente con "
                         "lineH=%d\n",
                         t.cellW, bh, lh);
            return false;
        }
    }
    // El bloque reservado tiene que ser exactamente el que ocupa la pintura:
    // kGridTitleLines líneas de título + 14 px de hueco + 20 px de pie. A celda
    // de 140 px son 3*17+34 = 85 px (antes 2*17+34 = 68 px y solo el 3% de los
    // títulos se veía entero). Si esta suma no cuadra, la celda solapa con la
    // línea de canal o deja hueco.
    if (VideoListView::gridTextBlockH(140)
        != VideoListView::kGridTitleLines * VideoListView::gridTitleLineH(140) + 34) {
        std::fprintf(stderr,
                     "SELFTEST grid FAIL blockH(140)=%d, want=%d\n",
                     VideoListView::gridTextBlockH(140),
                     VideoListView::kGridTitleLines
                         * VideoListView::gridTitleLineH(140) + 34);
        return false;
    }
    // Monótona creciente y con suelo en todo el rango real de celdas.
    int prevPx = 0;
    int prevBh = 0;
    for (int cw = 100; cw <= 900; cw += 10) {
        const int px = VideoListView::gridTitlePx(cw);
        const int bh = VideoListView::gridTextBlockH(cw);
        if (px < prevPx || bh < prevBh) {
            std::fprintf(stderr,
                         "SELFTEST grid FAIL cw=%d px=%d (prev=%d) blockH=%d "
                         "(prev=%d)\n",
                         cw, px, prevPx, bh, prevBh);
            return false;
        }
        prevPx = px;
        prevBh = bh;
    }

    // La celda de la rejilla tiene que medir LO MISMO en los tres sitios que la
    // usan: el sizeHint que lleva cada item del modelo, gridSize() (del que la
    // página saca la altura de la sección) y el rect que acaba pintando
    // QListView. En IconMode manda el sizeHint del MODELO, así que cuando
    // divergían las filas ocupaban una cosa y la sección se dimensionaba con
    // otra: con 19 vídeos (4 filas) la última quedaba entera fuera de la vista,
    // y con ella la línea de canal del vídeo. Aquí se monta un VideoListView de
    // verdad y se comprueba que no se solapa y que la altura de sección alcanza
    // para todas las filas. Sin red: el ThumbnailCache no pide nada hasta que
    // se le pinta una miniatura.
    {
        ThumbnailCache thumbs(QStringLiteral("selftest_grid"), 8 * 1024 * 1024);
        VideoListView v(&thumbs);
        v.setGridColumns(5);
        QVector<VideoInfo> vids(19);
        for (int i = 0; i < vids.size(); ++i) {
            vids[i].id = QStringLiteral("id%1").arg(i);
            vids[i].title = QStringLiteral("Titulo de prueba numero %1").arg(i);
        }
        v.resize(903, 4 * 400);
        v.show();
        v.showVideos(vids, VideoListView::GridMode);
        QCoreApplication::processEvents();
        const QSize cell = v.gridSize();
        if (cell.width() <= 0 || cell.height() <= 0) {
            std::fprintf(stderr,
                         "SELFTEST grid FAIL gridSize vacio (%dx%d) con la "
                         "ventana ya dimensionada\n",
                         cell.width(), cell.height());
            return false;
        }
        // El sizeHint que ve la vista (SizeHintRole), que en IconMode es el que
        // manda por encima de gridSize().
        const QSize hint = v.model()->index(0, 0).data(Qt::SizeHintRole).toSize();
        if (hint != cell) {
            std::fprintf(stderr,
                         "SELFTEST grid FAIL sizeHint del item (%dx%d) != "
                         "gridSize (%dx%d): la celda mide dos cosas\n",
                         hint.width(), hint.height(), cell.width(), cell.height());
            return false;
        }
        // Y el rect real de la primera celda, que es lo que tiene que coincidir
        // con gridSize() ahora que los tres van al mismo cálculo.
        if (v.gridItemRect(0).size() != cell) {
            std::fprintf(stderr,
                         "SELFTEST grid FAIL rect real de la celda %dx%d != "
                         "gridSize %dx%d\n",
                         v.gridItemRect(0).width(), v.gridItemRect(0).height(),
                         cell.width(), cell.height());
            return false;
        }
        // La altura que el Home le da a la sección tiene que alcanzar para las
        // 4 filas: si no, la última no se ve.
        const int cols = v.gridColumns();
        const int rows = (int(vids.size()) + cols - 1) / cols;
        const int sectionH = rows * cell.height();
        const QRect lastRect = v.gridItemRect(int(vids.size()) - 1);
        if (lastRect.bottom() + 1 > sectionH) {
            std::fprintf(stderr,
                         "SELFTEST grid FAIL la ultima fila se sale de la "
                         "seccion: rect hasta %d, seccion %d (celda %dx%d, %d "
                         "filas)\n",
                         lastRect.bottom(), sectionH, cell.width(), cell.height(),
                         rows);
            return false;
        }
        // Y las filas contiguas no pueden pisarse (el título de una celda no
        // puede caer encima de la línea de canal de la de encima).
        const QRect prevRect = v.gridItemRect(cols - 1);
        if (lastRect.top() < prevRect.bottom()) {
            std::fprintf(stderr,
                         "SELFTEST grid FAIL las filas se solapan: fila 1 hasta "
                         "%d, fila 2 empieza en %d\n",
                         prevRect.bottom(), lastRect.top());
            return false;
        }
        std::fprintf(stderr,
                     "SELFTEST grid -> celda %dx%d, %d filas, seccion %d, "
                     "ultima fila hasta %d (con sizeHint %dx%d): cabe\n",
                     cell.width(), cell.height(), rows, sectionH,
                     lastRect.bottom(), hint.width(), hint.height());
    }

    std::fprintf(stderr,
                 "SELFTEST grid -> ok (%d columnas, %d textos, kMinCellW=%d)\n",
                 int(sizeof(cases) / sizeof(cases[0])),
                 int(sizeof(txts) / sizeof(txts[0])), VideoListView::kMinCellW);
    return true;
}

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    ensureConsoleAttached();
#endif
#ifdef Q_OS_WIN
#ifndef NEVERTUBE_NO_MPV
    // Experimento ANGLE (Windows x64, OPT-IN con NEVERTUBE_ANGLE=1): forzar
    // el contexto GL del widget a ANGLE (OpenGL ES sobre D3D11) para que mpv
    // pueda decodificar con zero-copy. Con GL de escritorio (el default en la máquina de
    // pruebas — mpv loguea "Detected desktop OpenGL") mpv NO puede importar texturas D3D11
    // y se ve forzado a d3d11va-copy: el copy-back VRAM->RAM->textura GL es el
    // coste del CPU al 100% en la máquina de pruebas. Con contexto ANGLE, mpv (hwdec=auto,
    // invariante protegida intacta) elige solo el hwdec d3d11-egl: decodifica
    // en D3D11 y el frame viaja por EGLStream a la textura del widget sin
    // copiar a CPU. mpv carga LIBEGL.DLL con la búsqueda de LoadLibrary, así
    // que usa el MISMO ANGLE que carga Qt (mismo directorio) y toma el display
    // con eglGetCurrentDisplay() en paintGL: no hay que pasarle handles.
    // Solo Windows y solo donde hay mpv (el build x86/FfmpegBackend sale por
    // NEVERTUBE_NO_MPV; su render GLES no está preparado para ANGLE). Respetar
    // un QT_OPENGL que el usuario ya haya fijado.
    if (qEnvironmentVariableIsSet("NEVERTUBE_ANGLE")
        && !qEnvironmentVariableIsSet("QT_OPENGL")) {
        qputenv("QT_OPENGL", QByteArrayLiteral("angle"));
        std::fprintf(stderr,
                     "ANGLE: QT_OPENGL=angle (contexto Qt -> ANGLE/D3D11) por "
                     "NEVERTUBE_ANGLE=1; mpv debera elegir hwdec d3d11-egl "
                     "(zero-copy)\n");
    }
#endif // !NEVERTUBE_NO_MPV
#endif // Q_OS_WIN
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

    if (args.contains(QLatin1String("--selftest-ffmpeg"))) {
        // Fase 2: FfmpegBackend offline (clips embebidos, modo banco). Sin
        // FFmpeg en el build es un skip, no un fallo.
        return ffmpegSelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-hwprobe"))) {
        // Offline: la tabla de decisión con los casos REALES observados
        // (i5/HD4600 con fallo av1_qsv, GMA 950 software, GPU con HW) no
        // depende de la GPU de este equipo; es una regresión CI determinista.
        if (!hwprobeVerdictSelfTests())
            return 1;
#ifndef NEVERTUBE_NO_MPV
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
#else
        // Build x86 sin mpv (Fase 5): el sondeo vivo usa mpv y no existe en
        // este build. La tabla de decisión ya corrió arriba; el gate de HW
        // real del x86 es --selftest-ffmpeg (y la escalera reactiva cubre a
        // las máquinas sin HW sin necesidad de HwProbe).
        std::fprintf(stderr,
                     "SELFTEST hwprobe -> (sin mpv en este build: tabla de "
                     "decisión OK, sondeo vivo no disponible)\n");
        return 0;
#endif
    }

    if (args.contains(QLatin1String("--selftest-home"))) {
        return homeBrowseSelfTest() ? 0 : 1;
    }

    // Tope de columnas de las rejillas (función pura, sin red ni GPU): vale en
    // el CI, donde no se puede comprobar el hardware ni reproducir.
    if (args.contains(QLatin1String("--selftest-grid"))) {
        return gridGeometrySelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-theme"))) {
        // QSS puro: ningun selector declarado dos veces. Un duplicado no avisa y
        // el ultimo gana, que es como se colo un #sectionTitle de 22px por
        // detras del de 16px y crecieron los titulos de seccion de todas las
        // paginas. Offline y determinista: vale en el CI.
        const bool ok = nt::themeSelfTest();
        std::fprintf(stderr, "SELFTEST theme -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
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

    if (args.contains(QLatin1String("--selftest-suggest-parse"))) {
        // Solo el parser del desplegable: sin red y sin GPU, asi que este si
        // puede ir en el smoke del CI (a diferencia del selftest completo).
        const bool ok = suggestParseSelfTest();
        std::fprintf(stderr, "SELFTEST suggest-parse -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-suggest-panel"))) {
        // El desplegable en si (offline): construye una SearchPage y comprueba
        // los dos grupos, el filtrado del historial y el descarte de respuestas
        // tardias, sin pedir nada a la red.
        const bool ok = searchSuggestPanelSelfTestRun();
        std::fprintf(stderr, "SELFTEST suggest-panel -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-suggest-gestures"))) {
        // Gestos del desplegable en la jerarquia real (offline): la pagina
        // dentro de un QStackedWidget, con QSS puesto, y cada gesto entregado
        // por la ruta que usa la plataforma.
        const bool ok = searchSuggestGesturesSelfTestRun();
        std::fprintf(stderr, "SELFTEST suggest-gestures -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-suggest"))) {
        // Parser offline + sonda con red del endpoint que usa el type-ahead.
        return suggestSelfTest() ? 0 : 1;
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