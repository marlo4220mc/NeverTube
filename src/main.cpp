#include "mainwindow.h"
#include "downloader/download_dialog.h"
#include "downloader/download_settings.h"
#include "ui/pages/settings_page.h"

#include <QApplication>
#include <QMenu>
#include <QPushButton>
#include <QDir>
#include <QEventLoop>
#include <QGuiApplication>
#include <QIcon>
#include <QSslSocket>
#include <QStyleFactory>
#include <QTimer>

#include <cstdio>

#include "db/repositories.h"
#include "hwprobe.h"
#include "i18n.h"
#include "player_selftest.h"
#include "ffmpeg_selftest.h"
#include "ui/video_list_view.h"
#include "ui/thumbnail_cache.h"
#include "ui/pages/search_page.h"
#include "ui/theme.h"
#include "yt.h"
#include "yt_innertube.h"
#include "downloader/download_selftest.h"
#include "downloader/download_button_selftest.h"
#include "downloader/download_resume_selftest.h"
#include "downloader/download_folder_selftest.h"
#include "downloader/download_settings_selftest.h"
#include "downloader/download_transport_selftest.h"
#include "reels_filter_selftest.h"
#include "youtube_url.h"
#include "single_instance.h"
#include "single_instance_selftest.h"
#include "video_variant_selftest.h"

#ifdef Q_OS_WIN
#include <windows.h>
namespace {
// The app is marked WIN32_EXECUTABLE_TARGET (no console of its own). So that
// the selftests print and the CI/test machine see the logs, we attach to the
// parent process's console (CI shell) or, on a double click with
// NEVERTUBE_DEBUG=1 (explicit debugging), we create a console of our own. On a
// normal double-click launch NO terminal window is opened.
// Player core is not touched: this is app startup.
void ensureConsoleAttached()
{
    // IF CI/redirection already gave us a valid stdout (pipe inherited from the runner via
    // Start-Process -RedirectStandardOutput), we do NOT touch anything: the runner captures
    // that pipe. Attach/AllocConsole + freopen(CONOUT$) would steal the output.
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE)
        return;
    if (GetConsoleWindow())
        return; // we already have (or there is) a console
    bool ok = false;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) { // shell/CI: inherit the parent's console
        ok = true;
    } else if (qEnvironmentVariableIsSet("NEVERTUBE_DEBUG")) {
        // Double click WITHOUT a parent console: the normal app does not open a terminal. Only an
        // explicit debugging session (NEVERTUBE_DEBUG=1) opens a console of its own.
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

#if defined(Q_OS_WIN) && defined(_WIN64) && !defined(NEVERTUBE_NO_MPV)
namespace {
// ANGLE is only useful if it can actually start: it needs its two DLLs next to
// the exe (windeployqt deploys them) AND the D3D shader compiler, which is a
// system DLL. Forcing QT_OPENGL=angle when one of them is missing makes Qt fail
// to create the context and the app dies with
// "no se encuentra d3dcompiler_47.dll" BEFORE it can show anything (reported on
// the Lenovo x64). Probing is cheap (three LoadLibrary) and it turns a fatal
// startup into "the app opens with the default OpenGL".
bool angleDllsAvailable(QString *missing = nullptr)
{
    static const wchar_t *const kNeeded[] = {
        L"libEGL.dll", L"libGLESv2.dll", L"d3dcompiler_47.dll",
    };
    for (const wchar_t *name : kNeeded) {
        HMODULE h = LoadLibraryW(name);
        if (!h) {
            if (missing)
                *missing = QString::fromWCharArray(name);
            return false;
        }
        FreeLibrary(h);
    }
    return true;
}
} // namespace
#endif

// TLS SELFTEST with a REAL handshake. The old --selftest-ssl only loaded the
// OpenSSL DLLs (supportsSsl): a backend that loads but aborts on the
// first real use (e.g. "OpenSSL internal error, assertion failed:
// lock != NULL", known on Windows with Qt 5.15.2 + OpenSSL 1.1.1 on a
// 32-bit x86 machine) went unnoticed in CI. This selftest opens a
// real TLS connection to public hosts and checks that the engine completes
// the handshake; if OpenSSL aborts internally, the process dies with abort()
// and the CI marks it red with the assert message in the log.
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
        socket.setPeerVerifyMode(QSslSocket::VerifyNone); // tests the engine, not certs
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

// HW profile decision table tested offline (independent of this machine's
// GPU): it re-measures the real cases observed on machines we do not
// have here (i5-4590/HD4600 with av1_qsv failure -> SW; GMA 950 -> SW;
// GPUs with real HW -> HW). Deterministic regression for CI.
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
        // Real GPU: AV1 via Vulkan (e.g. this Linux machine).
        { true, false, false, "vulkan-copy", "HW" },
        // Real GPU: D3D11.
        { true, false, false, "d3d11va-copy", "HW" },
        // Fallback: backend active with no signals against it (after the grace).
        { false, false, false, "dxva2-copy", "HW" },
        // GMA 950: mpv confirmed software even though there is a WARP device.
        { false, true, false, "warp", "SW" },
        // i5-4590/HD4600: the av1_qsv attempt failed (unsupported) -> SW, even though
        // hwdec-current reports qsv-copy (the original false positive).
        { false, false, true, "qsv-copy", "SW" },
        // i5: after the QSV failure hwdec-current ends as "no" -> SW.
        { false, false, true, "no", "SW" },
        // Software log dominates even if there was an HW line before (decoder
        // change during the grace) -> SW.
        { true, true, false, "vulkan-copy", "SW" },
        // hwdec-current == "no" is decisive even if there was an HW line -> SW.
        { true, false, false, "no", "SW" },
        // No signal at all: indeterminate -> continue the ladder.
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

    // Profile descent ladder (hwprobe::lowerProfile), pure regression.
    struct LadderCase
    {
        const char *in;
        const char *want;
    };
    const LadderCase ladder[] = {
        { "av1-hw", "vp9-hw" },
        { "vp9-hw", "h264-hw" },
        { "h264-hw", "soft-all" },
        { "soft-all", "" },    // floor: not applicable
        { "", "" },            // no profile: not applicable
        { "desconocido", "" }, // weird profile: left untouched
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

// Column cap of Home's grids (VideoListView::clampGridColumns).
// The "Tamaño de las miniaturas" setting asks for 4/5/6 fixed columns and the grid is
// sized by width, so on narrow windows it has to be trimmed: with
// 6 columns at 700 px the cell measures 113 px and the 2-line title + the channel
// get cut off. It is a pure function, no widget or network: it is tested here.
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
        // Width of the test machine (1024x600 laptop): 6 columns
        // fit (cell of 147 px), which is exactly what "Pequeñas" looks for.
        { 6, 903, 6, "6 pedidas caben a 903 px (celda 147)" },
        { 5, 903, 5, "5 pedidas a 903 px" },
        { 4, 903, 4, "4 pedidas a 903 px (celda 222)" },
        // Narrow window: 6 columns would give 113 px -> it drops to 5 (136 px).
        { 6, 700, 5, "6 recortadas a 5 a 700 px (celda 136)" },
        { 6, 600, 4, "6 recortadas a 4 a 600 px (celda 147)" },
        // Very narrow: never fewer than 3 columns.
        { 6, 380, 3, "suelo de 3 columnas a 380 px" },
        { 4, 380, 3, "4 recortadas al suelo de 3" },
        // "Grandes" never goes above 4 even if the window is huge.
        { 4, 1850, 4, "4 pedidas a 1850 px (no se estira a 8)" },
        // Width still unknown (initial layout): nothing is trimmed; the
        // Home resizeEvent will adjust it once it exists.
        { 6, 0, 6, "sin ancho no se recorta" },
        { 6, -1, 6, "ancho negativo no se recorta" },
        // Degenerate inputs: never 0 columns.
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
    // The default minimum width (kMinCellW) is the reason for the cap: below it
    // the cell does not fit the full text block.
    if (VideoListView::kMinCellW < 100) {
        std::fprintf(stderr, "SELFTEST grid FAIL kMinCellW=%d (<100)\n",
                     VideoListView::kMinCellW);
        return false;
    }

    // Cell text derived from the width. It used to be 13 px and 34 px FIXED with the
    // thumbnail growing as the window stretched, which is what left the block
    // disjointed on a large screen. The size table is checked, that the
    // line spacing fits the font, that the reserved block is the one it occupies, that
    // at a cell of 140 px it still measures what it did before (68 px: nothing changes on
    // a narrow window) and that the curve is monotonic.
    struct TxtCase
    {
        int cellW;
        int px;
        const char *why;
    };
    const TxtCase txts[] = {
        { 0, 12, "celda sin ancho -> 12 px" },
        { 100, 12, "celda diminuta se queda en 12 px (tope inferior)" },
        { 140, 12, "140 px -> 12 px" },
        { 175, 13, "175 px (5 columnas a 903 px de viewport)" },
        { 220, 14, "220 px (4 columnas a 903 px)" },
        { 260, 15, "260 px" },
        { 300, 16, "300 px" },
        { 340, 16, "340 px -> tope superior de 16 px" },
        { 370, 16, "370 px (5 columnas a pantalla grande) -> tope superior" },
        { 900, 16, "celda enorme -> tope superior" },
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
    // The reserved block has to be exactly the one the painting occupies:
    // kGridTitleLines title lines + 14 px gap + 20 px footer. At a cell
    // of 140 px it is 2*15+34 = 64 px (with 3 lines at 14 px it was 3*18+34 = 88 and the
    // user saw it as too big). If this sum does not add up, the cell overlaps
    // with the channel line or leaves a gap.
    if (VideoListView::gridTextBlockH(140)
        != VideoListView::kGridTitleLines * VideoListView::gridTitleLineH(140) + 34) {
        std::fprintf(stderr,
                     "SELFTEST grid FAIL blockH(140)=%d, want=%d\n",
                     VideoListView::gridTextBlockH(140),
                     VideoListView::kGridTitleLines
                         * VideoListView::gridTitleLineH(140) + 34);
        return false;
    }
    // Monotonically increasing and with a floor across the real cell range.
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

    // The grid cell must measure THE SAME in the three places that
    // use it: the sizeHint each model item carries, gridSize() (from which the
    // page derives the section height) and the rect QListView ultimately
    // paints. In IconMode the MODEL's sizeHint rules, so when they
    // diverged the rows occupied one thing and the section was sized with
    // another: with 19 videos (4 rows) the last one ended up entirely outside the view,
    // and with it the video's channel line. Here a real VideoListView is
    // mounted and it is checked that it does not overlap and that the section height is enough
    // for all rows. No network: ThumbnailCache requests nothing until
    // a thumbnail is painted on it.
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
        // The sizeHint the view sees (SizeHintRole), which in IconMode is the one that
        // rules above gridSize().
        const QSize hint = v.model()->index(0, 0).data(Qt::SizeHintRole).toSize();
        if (hint != cell) {
            std::fprintf(stderr,
                         "SELFTEST grid FAIL sizeHint del item (%dx%d) != "
                         "gridSize (%dx%d): la celda mide dos cosas\n",
                         hint.width(), hint.height(), cell.width(), cell.height());
            return false;
        }
        // And the real rect of the first cell, which is what has to match
        // gridSize() now that the three go to the same calculation.
        if (v.gridItemRect(0).size() != cell) {
            std::fprintf(stderr,
                         "SELFTEST grid FAIL rect real de la celda %dx%d != "
                         "gridSize %dx%d\n",
                         v.gridItemRect(0).width(), v.gridItemRect(0).height(),
                         cell.width(), cell.height());
            return false;
        }
        // The height Home gives the section has to reach for the
        // 4 rows: if not, the last one is not visible.
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
        // And adjacent rows cannot step on each other (a cell's title cannot
        // fall over the channel line of the one above).
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


namespace {

// The command line applied to the window.
//
// It is a function and not a block inside main() because the SAME arguments
// arrive from another process when there is already an instance running (see
// SingleInstance): a link opened while the app runs has to end up in the window
// that is already there, with the meaning it has when it is typed here.
void applyArgs(MainWindow &window, const QStringList &args)
{
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
        } else if (arg == QLatin1String("--pause-download")
                   && i + 1 < args.size()) {
            // Pauses the download in progress (how the downloader is driven
            // from the command line, like --download).
            const QString videoId = args.at(++i);
            QTimer::singleShot(600, &window, [&window, videoId]() {
                window.pauseDownload(videoId);
            });
        } else if (arg == QLatin1String("--resume-download")
                   && i + 1 < args.size()) {
            // Continues a paused download.
            const QString videoId = args.at(++i);
            QTimer::singleShot(600, &window, [&window, videoId]() {
                window.resumeDownload(videoId);
            });
        } else if (arg == QLatin1String("--download")
                   && i + 1 < args.size()) {
            // Queues the download of a video without touching the UI: it
            // is how the downloader is driven from the command line.
            const QString videoId = args.at(++i);
            // The options of the dialog, so the download can be driven
            // without it: --download <id> [height] [audio]
            int height = 0;
            bool audioOnly = false;
            if (i + 1 < args.size()) {
                const QString next = args.at(i + 1);
                if (next == QLatin1String("audio")) {
                    audioOnly = true;
                    ++i;
                } else {
                    bool ok = false;
                    const int h = next.toInt(&ok);
                    if (ok && h > 0) {
                        height = h;
                        ++i;
                    }
                }
            }
            QTimer::singleShot(600, &window, [&window, videoId, height, audioOnly]() {
                window.downloadVideo(videoId, height, audioOnly);
            });
        } else if (arg == QLatin1String("--play-local")
                   && i + 1 < args.size()) {
            // Plays a local file (a download): how the button of the
            // Downloads page is tested.
            const QString path = args.at(++i);
            QTimer::singleShot(600, &window, [&window, path]() {
                window.playLocal(path);
            });
        } else if (arg == QLatin1String("--downloads")) {
            QTimer::singleShot(500, &window, [&window]() {
                window.openDownloads();
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
}

} // namespace


int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    ensureConsoleAttached();
#endif
#ifdef Q_OS_WIN
#ifndef NEVERTUBE_NO_MPV
    // ANGLE (OPT-IN): zero-copy d3d11-egl instead of d3d11va-copy.
    //
    // It is NOT the default. It was made the default on x64 and had to be
    // reverted: while the D3D shader compiler was missing, the app died at
    // startup and ANGLE never really ran; once the DLL was there it DID run
    // and the player + download went unresponsive (measured on the Lenovo
    // x64). The zero-copy gain does not pay for a hang on the machine that
    // matters, so it stays behind NEVERTUBE_ANGLE=1, with the default OpenGL
    // of always.
    //
    // The probe is still here for the opt-in: forcing ANGLE without its DLLs
    // does not fall back, it kills the startup before any window exists, so
    // an opt-in without them opens with the default OpenGL and says why.
    if (!qEnvironmentVariableIsSet("QT_OPENGL")
        && qEnvironmentVariableIsSet("NEVERTUBE_ANGLE")) {
#if defined(_WIN64)
        QString angleMissing;
        if (!angleDllsAvailable(&angleMissing)) {
            std::fprintf(stderr,
                         "ANGLE: pedido por NEVERTUBE_ANGLE=1 pero falta %s; "
                         "se usa el OpenGL por defecto\n",
                         angleMissing.toUtf8().constData());
        } else {
            qputenv("QT_OPENGL", QByteArrayLiteral("angle"));
            std::fprintf(stderr,
                         "ANGLE: QT_OPENGL=angle (contexto Qt -> ANGLE/D3D11) por "
                         "NEVERTUBE_ANGLE=1; mpv debera elegir hwdec d3d11-egl "
                         "(zero-copy)\n");
        }
#endif
    }
#endif // !NEVERTUBE_NO_MPV
#endif // Q_OS_WIN
#if defined(Q_OS_LINUX) && !defined(NEVERTUBE_NO_MPV)
    // EGL on X11 (default, unless the user already chose an integration).
    // mpv's VAAPI hwdec needs a dmabuf->texture interop, and with the render API
    // (libmpv_render) the only interop that exists is EGL: the DRM render node
    // we hand over is not enough by itself. Qt uses EGL on Wayland but GLX on
    // X11, and with GLX mpv logs "VAAPI hwdec only works with OpenGL or Vulkan
    // backends" and falls back to a COPY path (vulkan-copy here). With
    // xcb_egl the widget's context is EGL on X11 too, mpv finds the interop and
    // hwdec=auto reaches zero-copy (vaapi).
    // Measured on the same build and clip (1080p30, Ryzen 7 7840U / Radeon
    // 760M): GLX 6.10 % of a core vs EGL 4.30 %; on Wayland 3.80 %.
    // Only when Qt really ends up on xcb (an explicit QT_QPA_PLATFORM wins, and
    // a Wayland session is left alone) and never overriding what the user set:
    // QT_XCB_GL_INTEGRATION=xcb_glx restores the old behaviour.
    {
        const QByteArray platform = qgetenv("QT_QPA_PLATFORM");
        const bool onX11 =
            platform.startsWith("xcb")
            || (platform.isEmpty()
                && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"));
        if (onX11 && !qEnvironmentVariableIsSet("QT_XCB_GL_INTEGRATION")) {
            qputenv("QT_XCB_GL_INTEGRATION", QByteArrayLiteral("xcb_egl"));
            std::fprintf(stderr,
                         "EGL: QT_XCB_GL_INTEGRATION=xcb_egl (contexto Qt -> EGL "
                         "en X11); mpv debera poder elegir hwdec vaapi "
                         "(zero-copy)\n");
        }
    }
#endif // Q_OS_LINUX && !NEVERTUBE_NO_MPV
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(resources);
    app.setApplicationName(QStringLiteral("nevertube"));
    app.setOrganizationName(QStringLiteral("nevertube"));
    // Wayland/GNOME association: the compositor identifies the window by its
    // app_id and looks up "<app_id>.desktop" to get the icon and the name.
    // Without this the dock/overview shows a generic icon, because the binary is
    // "nevertube_qt2" and the .desktop is installed with that name (see
    // packaging/linux/ and the install() rules in CMakeLists.txt). On X11 the
    // window icon comes from setWindowIcon() below (StartupWMClass matches this
    // file); on Windows/macOS it is a no-op.
    QGuiApplication::setDesktopFileName(QStringLiteral("nevertube_qt2.desktop"));
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    // Window icon. ALL sizes are included because QIcon does not scale: it picks
    // the entry that best matches what each surface asks for (the OS bar asks
    // for 16 or 24, the window manager 32 or 48, alt-tab 256). With a
    // single entry, the bar shows it blurry even if the PNG is 1024.
    // The Windows .exe carries it embedded separately (resources/nt.rc).
    QIcon iconoApp;
    for (int lado : {16, 24, 32, 48, 64, 128, 256})
        iconoApp.addFile(QStringLiteral(":/icon/nt_%1.png").arg(lado));
    app.setWindowIcon(iconoApp);

    // TEXT selection in the app's gray. It is here and not in the QSS because there is
    // selectable text the QSS does not reach: the Player description is a
    // QLabel with Qt::TextSelectableByMouse and it paints it with QPalette::Highlight,
    // which without this came out with the desktop theme's blue. It is applied on the
    // QApplication palette so all pages inherit it.
    QPalette pal = app.palette();
    nt::applyTextSelectionPalette(pal);
    app.setPalette(pal);

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
        std::fprintf(stderr, "SELFTEST discover -> %s\n",
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

    if (args.contains(QLatin1String("--selftest-settings-page"))) {
        // The page of Settings with the REAL window: the new section of the
        // downloads (the folder and the codec) is there.
        MainWindow window;
        window.show();
        SettingsPage *page = window.findChild<SettingsPage *>();
        bool ok = page != nullptr;
        std::fprintf(stderr, "SETTINGS-PAGE %s: la pagina de ajustes existe\n",
                     ok ? "OK" : "FAIL");
        if (ok) {
            bool sawFolder = false;
            bool sawConnections = false;
            for (QPushButton *b : page->findChildren<QPushButton *>()) {
                const QString kind =
                    b->property("downloadSetting").toString();
                if (kind == QLatin1String("folder"))
                    sawFolder = true;
                if (kind == QLatin1String("connections"))
                    sawConnections = true;
            }
            std::fprintf(stderr, "SETTINGS-PAGE %s: hay fila de carpeta\n",
                         sawFolder ? "OK" : "FAIL");
            std::fprintf(stderr, "SETTINGS-PAGE %s: hay fila de conexiones\n",
                         sawConnections ? "OK" : "FAIL");
            ok = sawFolder && sawConnections;
        }
        // The menu of the download with the codec of the REAL setting: the
        // one that is marked is the default.
        if (page) {
            SettingsRepo *set = window.settingsRepo();
            const QString code = set
                ? set->getString(
                      QString::fromLatin1(downloadsettings::kConnectionsKey),
                      QString())
                : QString();
            QVector<VideoVariant> variants;
            auto add = [&variants](int h, const char *c, qint64 bytes) {
                VideoVariant v;
                v.height = h;
                v.codec = QString::fromLatin1(c);
                v.sizeBytes = bytes;
                variants.append(v);
            };
            add(1080, "vp9", 761100000);
            add(1080, "av1", 874400000);
            add(1080, "h264", 1001900000);
            QMenu menu;
            dl::buildDownloadMenu(&menu, variants, code);
            menu.popup(QPoint(20, 20));
            QApplication::processEvents();
            QApplication::processEvents();
            QAction *res = nullptr;
            for (QAction *a : menu.actions()) {
                if (a->isSeparator() || !a->menu())
                    continue;
                res = a;
                break;
            }
            if (res && res->menu()) {
                res->menu()->popup(QPoint(180, 80));
                QApplication::processEvents();
                QApplication::processEvents();
                res->menu()->grab().save(QStringLiteral(
                    "/tmp/opencode/menu_default.png"));
            }
        }

        // The section of the downloads, drawn: it is what the person sees.
        if (page) {
            for (QWidget *w : page->findChildren<QWidget *>()) {
                if (w->property("downloadSetting").toString()
                    == QLatin1String("folder")) {
                    // The card that holds the two rows of the downloads.
                    QWidget *card = w->parentWidget();
                    while (card && card->objectName()
                                  != QLatin1String("settingsCard"))
                        card = card->parentWidget();
                    if (card)
                        card->grab().save(QStringLiteral(
                            "/tmp/opencode/ajustes_descargas.png"));
                    break;
                }
            }
        }
        std::fprintf(stderr, "SETTINGS-PAGE SELFTEST %s\n",
                     ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-download-resume"))) {
        // Offline: the walk that decides which fragment is asked for (the
        // rule the resume depends on) and the migration of the rows of the
        // first version of the downloader, on a temporary database.
        return downloadResumeSelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-download-button"))) {
        // Integration test of the download BUTTON: it builds the real
        // MainWindow, finds the button of the Player by its name and
        // presses it, and checks that the download was queued. No network:
        // the queue is what is checked.
        return downloadButtonSelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-download"))) {
        // Offline: the download plan is a pure function of the master
        // playlist, so its cases are deterministic and run in CI.
        return downloadPlanSelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-download-transport"))) {
        // Offline: the rules of the transport (the discard of the fragments
        // that do not advance).
        return downloadTransportSelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-download-settings"))) {
        // Offline: the codes of the settings of the downloader.
        return downloadSettingsSelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-download-folder"))) {
        // Offline: the folder of the downloads and the command that opens
        // it are pure functions.
        return downloadFolderSelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-single-instance"))) {
        // Offline: two objects of this same process with the same name. The
        // second one must NOT take the name and its arguments must arrive at
        // the first one (a socket, no second process: nothing to wait for).
        return singleInstanceSelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-reels"))) {
        // Offline: the filter of the Shorts (the vertical videos) is a pure
        // function of the list.
        return reelFilterSelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-variants"))) {
        // Offline: the variants of a video (height, codec and real size) are
        // a pure function of the player response.
        return videoVariantSelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-ffmpeg"))) {
        // Phase 2: FfmpegBackend offline (embedded clips, bench mode). Without
        // FFmpeg in the build is a skip, not a failure.
        return ffmpegSelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-hwprobe"))) {
        // Offline: the decision table with the REAL cases observed
        // (i5/HD4600 with av1_qsv failure, GMA 950 software, GPU with HW) does
        // not depend on this machine's GPU; it is a deterministic CI regression.
        if (!hwprobeVerdictSelfTests())
            return 1;
#ifndef NEVERTUBE_NO_MPV
        // GPU-dependent diagnostic: it prints the detected profile
        // (av1-hw/vp9-hw/h264-hw/soft-all) or INDETERMINATE. It is not a fixed
        // assert, but a check that the probe concludes.
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
        // x86 build without mpv (Phase 5): the live probe uses mpv and does not exist in
        // this build. The decision table already ran above; the real HW gate
        // of x86 is --selftest-ffmpeg (and the reactive ladder covers
        // machines without HW without needing HwProbe).
        std::fprintf(stderr,
                     "SELFTEST hwprobe -> (sin mpv en este build: tabla de "
                     "decisión OK, sondeo vivo no disponible)\n");
        return 0;
#endif
    }

    if (args.contains(QLatin1String("--selftest-home"))) {
        return homeBrowseSelfTest() ? 0 : 1;
    }

    // Column cap of the grids (pure function, no network or GPU): it works in
    // the CI, where the hardware cannot be checked nor playback tested.
    if (args.contains(QLatin1String("--selftest-grid"))) {
        return gridGeometrySelfTest() ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-theme"))) {
        // Pure QSS: no selector declared twice. A duplicate does not warn and
        // the last one wins, which is how a #sectionTitle of 22px slipped in
        // behind the 16px one and the section titles grew on all the
        // pages. Offline and deterministic: it works in the CI.
        const bool ok = nt::themeSelfTest();
        std::fprintf(stderr, "SELFTEST theme -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-i18n"))) {
        // Offline catalogs: one JSON per language under localization/. It
        // checks the key parity between languages (a missing translation is
        // the silent failure of this kind of system), that no used key is
        // misspelled, and the live-binding mechanism. Deterministic: it works
        // in the CI.
        const bool ok = nt::i18nSelfTest();
        std::fprintf(stderr, "SELFTEST i18n -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-glyphs"))) {
        // UI glyphs against the app's real font. The default font of
        // the system does not cover all non-ASCII characters: the missing
        // one is replaced by Qt with the missing-glyph marker, which measured
        // on screen is 2x5 px (an almost invisible button). It walks the literals
        // of the source tree, so a new glyph that does not exist is caught by the
        // CI. Offline and deterministic: it works in the CI.
        const bool ok = nt::glyphSelfTest();
        std::fprintf(stderr, "SELFTEST glyphs -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-feeds"))) {
        const bool proto = feedsProtocolSelfTest();
        const bool chain = feedsChainSelfTest();
        std::fprintf(stderr, "SELFTEST feeds proto=%s cadena=%s -> %s\n",
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
        // Only the parser of the dropdown: no network or GPU, so this one
        // can go in the CI smoke (unlike the full selftest).
        const bool ok = suggestParseSelfTest();
        std::fprintf(stderr, "SELFTEST suggest-parse -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-dates"))) {
        // Relative-date parser (relativeToIso): pure function, no network or
        // GPU. It pins the regression of "hace N semanas" -> seconds -> "hoy".
        const bool ok = relativeDateSelfTest();
        std::fprintf(stderr, "SELFTEST dates -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-video-url"))) {
        // The link pasted in the search box (youtubeVideoIdFromText): pure
        // function, no network or GPU. It pins that an 11-letter query is NOT
        // turned into a video and that the usual forms of the address are.
        const bool ok = youtubeUrlSelfTest();
        std::fprintf(stderr, "SELFTEST video-url -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-search-url"))) {
        // A YouTube link pasted in the search box (offline): the video is
        // opened, the URL is not saved in the history and the title is.
        const bool ok = searchUrlSelfTestRun();
        std::fprintf(stderr, "SELFTEST search-url -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-suggest-panel"))) {
        // The dropdown itself (offline): it builds a SearchPage and checks
        // the two groups, the history filtering and the discarding of late
        // responses, without asking the network for anything.
        const bool ok = searchSuggestPanelSelfTestRun();
        std::fprintf(stderr, "SELFTEST suggest-panel -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-suggest-gestures"))) {
        // Dropdown gestures in the real hierarchy (offline): the page
        // inside a QStackedWidget, with QSS set, and each gesture delivered
        // by the route the platform uses.
        const bool ok = searchSuggestGesturesSelfTestRun();
        std::fprintf(stderr, "SELFTEST suggest-gestures -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    if (args.contains(QLatin1String("--selftest-suggest"))) {
        // Offline parser + network probe of the endpoint the type-ahead uses.
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
        // TLS backend (Windows: OpenSSL 1.1 in DLLs next to the exe). If the
        // OpenSSL DLLs are missing or do not load, supportsSsl() is false and all
        // HTTPS calls fail with "TLS initialization failed".
        const bool ok = QSslSocket::supportsSsl();
        std::fprintf(stderr, "SELFTEST ssl -> %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }

    // THE SECOND PROCESS, driven by the selftest of the single instance: the
    // same binary runs this branch, does the handover and exits, which is what
    // a second instance really does. It is here (and not inside the test)
    // because the handover is the behaviour of a PROCESS: doing it from a
    // lambda of the same process would not exercise the pipe at all, and it
    // blocks the event loop that has to answer.
    //
    // Exit code: 0 = the arguments were handed over; 2 = it took the name (the
    // handover did NOT work).
    if (args.size() >= 3 && args.at(1) == QLatin1String("--handover")) {
        SingleInstance second(args.at(2));
        const bool becameOwner = second.start(args.mid(3));
        return becameOwner ? 2 : 0;
    }

    // ONE instance per user: a second process hands its arguments over and
    // exits (see SingleInstance). It is HERE and not before the selftests: a
    // test run must never talk to the app of the user, nor be refused because
    // the app is open.
    SingleInstance instance(SingleInstance::name());
    if (!instance.start(args))
        return 0;

    MainWindow window;
    window.show();
    QObject::connect(&instance, &SingleInstance::argumentsReceived, &window,
                     [&window](const QStringList &a) {
                         // With the app already open: the window comes to the
                         // front (which is half of what opening a link means)
                         // and the arguments mean what they mean on the
                         // command line.
                         if (window.isMinimized())
                             window.showNormal();
                         window.raise();
                         window.activateWindow();
                         applyArgs(window, a);
                     });

    // The queue of the downloads lives in memory: the rows that a previous
    // session left "pending" (or half way) are revived HERE, when the app
    // really opens. NOT in the constructor of MainWindow: the integration
    // tests build a window too, and they must not start the downloads of the
    // person (network, files) behind a test that is offline.
    //
    // BEFORE applyArgs(): a --download that arrives on the command line (or
    // through the socket of the single instance) is queued BEHIND the ones
    // this session is reviving, which is the order in which they were asked
    // for.
    window.resumePendingDownloads();

    applyArgs(window, args);

    return app.exec();
}