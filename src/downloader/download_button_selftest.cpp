#include "download_button_selftest.h"

#include "../db/app_database.h"
#include "../db/repositories.h"
#include "../mainwindow.h"

#include <QAbstractButton>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

#include <cstdio>

bool downloadButtonSelfTest()
{
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        std::fprintf(stderr, "DOWNLOAD-BUTTON %s: %s\n", cond ? "OK" : "FAIL",
                     what);
        if (!cond)
            ok = false;
    };

    // The REAL window, with the real Player page: the button is the one the
    // person presses.
    MainWindow window;
    window.show();

    // --- the button ---
    QAbstractButton *button = window.downloadButton();
    check(button != nullptr,
          "el boton de descarga existe en la pagina del player");
    if (!button) {
        std::fprintf(stderr, "DOWNLOAD-BUTTON SELFTEST FAIL\n");
        return false;
    }
    check(button->objectName() == QLatin1String("downloadButton"),
          "el boton tiene el nombre esperado");
    // The button has no text: its tooltip AND its accessible name ARE its
    // label, and both have to be there.
    check(!button->toolTip().isEmpty(),
          "el boton tiene tooltip (es su etiqueta: no tiene texto)");
    check(!button->accessibleName().isEmpty(),
          "el boton tiene nombre accesible (es su etiqueta)");
    check(button->isEnabled(), "el boton esta habilitado");

    // Pressing it with NO video on screen must not queue anything: there is
    // no video to download. It has to be harmless.
    button->click();

    // --- the queue path (the same one the button uses) ---
    // The row has to appear in the repository with the state "pending": that
    // is what the page of Downloads reads.
    //
    // The window uses the REAL database (there is no way to give it
    // another one), so the test works with its own id and leaves no trace:
    // the row is removed at the end whatever happens.
    DownloadRepo *repo = window.downloadsRepo();
    check(repo != nullptr, "la app tiene el repositorio de descargas");
    if (repo) {
        const QString videoId = QStringLiteral("selftestDlBtn");
        repo->remove(videoId);
        window.queueDownload(videoId);
        DownloadInfo info;
        check(repo->get(videoId, info), "la descarga queda en la lista");
        check(info.status == QLatin1String("pending"),
              "la descarga entra con el estado pending");
        check(info.videoId == videoId, "la fila guarda el id del video");
        // The same id twice is not queued again: the state is shown.
        window.queueDownload(videoId);
        int rows = 0;
        for (const DownloadInfo &d : repo->all()) {
            if (d.videoId == videoId)
                ++rows;
        }
        check(rows == 1, "el mismo video no se encola dos veces");
        repo->remove(videoId);
        check(!repo->get(videoId, info), "se puede borrar de la lista");
    }

    std::fprintf(stderr, "DOWNLOAD-BUTTON SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}
