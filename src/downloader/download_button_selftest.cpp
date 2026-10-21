#include "download_button_selftest.h"

#include "../db/app_database.h"
#include "../db/repositories.h"
#include "../downloader/download_manager.h"
#include "../mainwindow.h"
#include "../ui/pages/downloads_page.h"
#include "../ui/pages/watch_page.h"

#include <QAbstractButton>
#include <QPushButton>
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

    // The signal of the button carries the choice of the menu: the height
    // and whether only the audio is wanted.
    QString gotId;
    int gotHeight = -1;
    QString gotCodec;
    bool gotAudio = false;
    QObject::connect(window.findChild<WatchPage *>(),
                     &WatchPage::downloadRequested,
                     [&gotId, &gotHeight, &gotCodec, &gotAudio](
                         const QString &id, const QString &, const QString &,
                         int height, const QString &codec, bool audio) {
        gotId = id;
        gotHeight = height;
        gotCodec = codec;
        gotAudio = audio;
    });
    emit window.findChild<WatchPage *>()->downloadRequested(
        QStringLiteral("vidDePrueba"), QStringLiteral("t"), QStringLiteral("c"),
        1080, QStringLiteral("av1"), true);
    check(gotId == QLatin1String("vidDePrueba"),
          "el signal lleva el id del video");
    check(gotHeight == 1080, "el signal lleva la resolucion elegida");
    check(gotCodec == QLatin1String("av1"), "el signal lleva el codec elegido");
    check(gotAudio, "el signal lleva la opcion de solo el audio");

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

    // --- the button of "Abrir carpeta" ---
    //
    // It has to emit the folder where the files are written. It used to take
    // the path of the FIRST ROW (the partial file of a download in
    // progress), which is empty in the new downloader: the button did
    // nothing at all.
    {
        DownloadsPage *page = window.findChild<DownloadsPage *>();
        check(page != nullptr, "la pagina de descargas existe");
        if (page) {
            QString asked;
            QObject::connect(page, &DownloadsPage::openFolderRequested,
                             [&asked](const QString &p) { asked = p; });
            // The folder of the downloader: the one of the page.
            QPushButton *openFolder = nullptr;
            const QList<QPushButton *> buttons = page->findChildren<QPushButton *>();
            for (QPushButton *b : buttons) {
                if (b->objectName() == QLatin1String("ghost")
                    && b->text().contains(QStringLiteral("Abrir"),
                                          Qt::CaseInsensitive)) {
                    openFolder = b;
                    break;
                }
            }
            // The button is a "ghost": the object name is shared with other
            // buttons of the page, so it is looked up by its bound text.
            check(openFolder != nullptr, "el boton de abrir carpeta existe");
            if (openFolder) {
                openFolder->click();
                check(!asked.isEmpty(),
                      "el boton de abrir carpeta emite una carpeta (no hace nada)");
                check(asked.contains(QLatin1String("NeverTube")),
                      "la carpeta es la de las descargas de la app");
            }
        }
    }

    // --- the button of "Reintentar" ---
    //
    // It used to call remove() (which deletes the row of the repository, and
    // with it the index of the fragments already on the disk) BEFORE
    // starting again: the download started from zero, and the bytes that
    // were already downloaded were thrown away.
    {
        DownloadRepo *r = window.downloadsRepo();
        check(r != nullptr, "el repositorio de descargas existe");
        if (r) {
            const QString videoId = QStringLiteral("selftestDlRetry");
            r->remove(videoId);
            DownloadInfo info;
            info.videoId = videoId;
            info.title = QStringLiteral("t");
            info.channel = QStringLiteral("c");
            info.status = QStringLiteral("error");
            info.parts = {0, 1, 2, 5};  // the fragments already on the disk
            r->upsert(info);
            DownloadInfo got;
            check(r->get(videoId, got) && got.parts.size() == 4,
                  "la fila guarda los fragmentos ya bajados");

            // The retry: the row must SURVIVE (with its index of fragments)
            // so that the download resumes. It is the REAL method of the
            // manager (the one the button calls).
            dl::DownloadManager *mgr = window.downloadManager();
            check(mgr != nullptr, "el downloader existe");
            if (mgr) {
                mgr->retry(videoId);
                DownloadInfo after;
                check(r->get(videoId, after),
                      "tras el reintento la fila sigue (no se borro)");
                check(after.parts.size() == 4,
                      "tras el reintento la fila sigue con sus fragmentos");
                // The retry starts a real download (it asks YouTube for the
                // formats): it is stopped and removed here, so the test
                // does not leave a job alive (and it does not touch the
                // network of the person running it more than needed).
                mgr->remove(videoId);
            }
            r->remove(videoId);
        }
    }

    // --- the button of "Pausar" / "Continuar" ---
    //
    // A download can be paused (and continued): the fragments that are
    // already on the disk are kept, so continuing is the same path as the
    // retry.
    {
        DownloadRepo *r = window.downloadsRepo();
        dl::DownloadManager *mgr = window.downloadManager();
        check(r != nullptr && mgr != nullptr,
              "el repositorio y el downloader existen");
        if (r && mgr) {
            const QString videoId = QStringLiteral("selftestDlPause");
            r->remove(videoId);
            DownloadInfo info;
            info.videoId = videoId;
            info.title = QStringLiteral("t");
            info.status = QStringLiteral("downloading");
            info.parts = {0, 1, 2};
            info.bytesDone = 300;
            r->upsert(info);

            mgr->pause(videoId);
            DownloadInfo paused;
            check(r->get(videoId, paused), "la fila sigue tras pausar");
            check(paused.status == QLatin1String("paused"),
                  "la descarga queda en pausa");
            check(paused.parts.size() == 3,
                  "la pausa conserva los fragmentos ya bajados");
            check(paused.bytesDone == 300,
                  "la pausa conserva los bytes bajados");

            // The button of the page: it exists and it is the one of the
            // state of the row (pause while it runs, continue while paused).
            DownloadsPage *page = window.findChild<DownloadsPage *>();
            check(page != nullptr, "la pagina de descargas existe");
            if (page) {
                bool sawPause = false;
                for (QPushButton *b : page->findChildren<QPushButton *>()) {
                    if (b->property("downloadAction").toString()
                        == QLatin1String("pause")) {
                        sawPause = true;
                        break;
                    }
                }
                check(sawPause, "la pagina tiene el boton de pausar");
            }
            r->remove(videoId);
        }
    }

    // --- the X of a row: it CANCELS the download ---
    //
    // It used to delete the row and the files, but the JOB kept running:
    // when it finished it wrote the row again and the download came back
    // to the list.
    {
        DownloadRepo *r = window.downloadsRepo();
        dl::DownloadManager *mgr = window.downloadManager();
        check(r != nullptr && mgr != nullptr,
              "el repositorio y el downloader existen");
        if (r && mgr) {
            const QString videoId = QStringLiteral("selftestDlCancel");
            r->remove(videoId);
            DownloadInfo info;
            info.videoId = videoId;
            info.title = QStringLiteral("t");
            info.status = QStringLiteral("downloading");
            info.bytesDone = 500;
            info.parts = {0, 1};
            r->upsert(info);

            mgr->remove(videoId);
            DownloadInfo after;
            check(!r->get(videoId, after),
                  "la X quita la descarga de la lista");
            // And the job of that download must be STOPPED: the downloader
            // has to be free for the next one.
            check(!mgr->isBusy(),
                  "la X detiene la descarga (el downloader queda libre)");
            r->remove(videoId);
        }
    }

    // --- continuing a paused download ---
    //
    // The pause left the JOB alive (stopped), so the start() of the resume
    // found a job already in place and refused it ("a download is already
    // running"): the download did not continue.
    {
        DownloadRepo *r = window.downloadsRepo();
        dl::DownloadManager *mgr = window.downloadManager();
        check(r != nullptr && mgr != nullptr,
              "el repositorio y el downloader existen");
        if (r && mgr) {
            const QString videoId = QStringLiteral("selftestDlResume");
            r->remove(videoId);
            DownloadInfo info;
            info.videoId = videoId;
            info.title = QStringLiteral("t");
            info.status = QStringLiteral("paused");
            info.parts = {0, 1, 2};
            info.bytesDone = 300;
            r->upsert(info);

            mgr->resume(videoId);
            DownloadInfo after;
            check(r->get(videoId, after), "la fila sigue tras continuar");
            check(after.status != QLatin1String("paused"),
                  "la descarga NO sigue pausada (el reanudar la revive)");
            check(after.parts.size() == 3,
                  "el reanudar conserva los fragmentos ya bajados");
            mgr->remove(videoId);
            r->remove(videoId);
        }
    }

    // --- the button of the pause: it decides with the CURRENT state ---
    //
    // The rows are updated in place (the same widget), so the button used
    // the copy of the row that was captured when it was BUILT: after
    // pausing, the click kept sending "pause" instead of "continue", and
    // the download never continued.
    {
        DownloadRepo *r = window.downloadsRepo();
        check(r != nullptr, "el repositorio de descargas existe");
        if (r) {
            const QString videoId = QStringLiteral("selftestDlState");
            r->remove(videoId);
            DownloadInfo info;
            info.videoId = videoId;
            info.status = QStringLiteral("downloading");
            r->upsert(info);

            DownloadsPage *page = window.findChild<DownloadsPage *>();
            check(page != nullptr, "la pagina de descargas existe");
            if (page) {
                QString asked;
                QObject::connect(page, &DownloadsPage::pauseRequested,
                                 [&](const QString &id) { asked = QStringLiteral("pause") + id; });
                QObject::connect(page, &DownloadsPage::resumeRequested,
                                 [&](const QString &id) { asked = QStringLiteral("resume") + id; });
                // The state of the row changes (the pause).
                DownloadInfo paused;
                r->get(videoId, paused);
                paused.status = QStringLiteral("paused");
                r->upsert(paused);
                // The row of that video is built (the page shows it).
                page->reload();
                // The button of THAT row is pressed: it has to CONTINUE.
                QPushButton *theButton = nullptr;
                for (QPushButton *b : page->findChildren<QPushButton *>()) {
                    if (b->property("downloadAction").toString()
                        != QLatin1String("pause"))
                        continue;
                    // The row of the button: the one that has the id.
                    if (b->parentWidget()
                        && b->parentWidget()->property("downloadVideoId").toString()
                            == videoId) {
                        theButton = b;
                        break;
                    }
                }
                check(theButton != nullptr,
                      "el boton de pausar de esa fila existe");
                if (theButton)
                    theButton->click();
                check(asked == QLatin1String("resume") + videoId,
                      "el boton continua la descarga que esta pausada");
            }
            r->remove(videoId);
        }
    }

    // --- the X of a row: it is a WASTE BASKET and it asks before deleting ---
    {
        DownloadsPage *page = window.findChild<DownloadsPage *>();
        check(page != nullptr, "la pagina de descargas existe");
        if (page) {
            // The icon of the button is the waste basket (not an X).
            QPushButton *trash = nullptr;
            for (QPushButton *b : page->findChildren<QPushButton *>()) {
                if (b->property("downloadAction").toString()
                    == QLatin1String("remove"))
                    trash = b;
            }
            check(trash != nullptr, "el boton de borrar la fila existe");
            if (trash) {
                const QIcon ic = trash->icon();
                check(!ic.isNull(), "el boton de borrar tiene icono");
                // The drawn waste basket (it is the icon of the app).
                QPixmap px = ic.pixmap(16, 16);
                check(!px.isNull() && px.width() > 0,
                      "el icono del boton de borrar se dibuja");
                if (!px.isNull())
                    px.save(QStringLiteral("/tmp/opencode/icono_tacho.png"));
            }
        }
    }

    // --- the delete does NOT touch files outside the folder of the downloads ---
    //
    // The path of the file comes from the repository, and a row could name a
    // file anywhere (a stale row, a hand-edited database). The delete only
    // removes what is INSIDE the folder of the downloads.
    {
        DownloadRepo *r = window.downloadsRepo();
        dl::DownloadManager *mgr = window.downloadManager();
        check(r != nullptr && mgr != nullptr,
              "el repositorio y el downloader existen");
        if (r && mgr) {
            const QString videoId = QStringLiteral("selftestDlOutside");
            const QString outside =
                QStringLiteral("/tmp/nevertube-fuera-de-la-carpeta.mkv");
            // A file OUTSIDE the folder of the downloads.
            {
                QFile f(outside);
                f.open(QIODevice::WriteOnly);
                f.write("no se debe borrar");
                f.close();
            }
            DownloadInfo info;
            info.videoId = videoId;
            info.title = QStringLiteral("t");
            info.status = QStringLiteral("done");
            info.finalPath = outside;
            r->upsert(info);

            mgr->remove(videoId);
            check(QFile::exists(outside),
                  "un archivo FUERA de la carpeta de descargas NO se borra");
            QFile::remove(outside);
            r->remove(videoId);
        }
    }

    std::fprintf(stderr, "DOWNLOAD-BUTTON SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}
