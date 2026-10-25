#include "download_resume_selftest.h"

#include "../db/app_database.h"
#include "../db/repositories.h"
#include "download_manager.h"
#include "muxer.h"
#include "segment_plan.h"
#include "speed_window.h"

#include <QDir>
#include <QStringList>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

// Two things that cost real bytes on a real machine and that had no test:
//
//   1. the RESUME. The pool gave ONE fragment to each connection and, when
//      that fragment was already on the disk, it moved on to the next
//      connection. With the first eight fragments downloaded the eight
//      connections found theirs done, NOTHING was asked for, and the download
//      stayed still FOR EVER (nothing in flight, nothing calling the
//      allocator again). The rule lives in dl::pickNextFragment and is pure.
//
//   2. the rows of the FIRST version of the downloader, where the mp3 shared
//      the row (and the folder of the fragments) of the video: their key is
//      the plain video id and `quality` is "mp3". They have to MIGRATE, with
//      their bytes, or the mp3 is resumed as a video (a full video under the
//      name of an mp3) and the video of that id finds the row of the mp3
//      "done" and refuses to download, silently.
bool downloadResumeSelfTest()
{
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        std::fprintf(stderr, "DOWNLOAD-RESUME %s: %s\n", cond ? "OK" : "FAIL",
                     what);
        if (!cond)
            ok = false;
    };

    // --- 1. the walk that decides which fragment is asked for -------------
    {
        using namespace dl;
        // The encoding of the index of the row: the video ones are positive
        // and the audio ones start at -2, so the video 0 and the audio 0 do
        // not collide and neither of them is lost.
        check(fragmentMark(true, 0) == 0, "el video 0 se marca con 0");
        check(fragmentMark(false, 0) == -2, "el audio 0 se marca con -2");
        check(fragmentMark(true, 3) == 3, "el video 3 se marca con 3");
        check(fragmentMark(false, 3) == -5, "el audio 3 se marca con -5");
        check(fragmentMark(true, 0) != fragmentMark(false, 0),
              "el video 0 y el audio 0 NO se confunden");

        const int videoCount = 100;
        const int audioCount = 10;

        // THE BUG: the first eight fragments are already on the disk (a
        // resume). The first connection has to take the NINTH, not nothing.
        QVector<int> firstEight;
        for (int i = 0; i < 8; ++i)
            firstEight.append(fragmentMark(true, i));
        FragmentPick pick =
            pickNextFragment(false, videoCount, audioCount, 0, 0, firstEight);
        check(pick.found, "con los 8 primeros ya bajados se pide ALGO");
        check(pick.found && pick.video && pick.index == 8,
              "el fragmento que se pide es el 9 (indice 8)");
        check(pick.videoAdvance == 9 && pick.audioAdvance == 0,
              "la caminata consume los 8 saltados y el elegido");
        // The caller adds the advances: the next connection goes on at 9.
        pick = pickNextFragment(false, videoCount, audioCount,
                                pick.videoAdvance, pick.audioAdvance,
                                firstEight);
        check(pick.found && pick.video && pick.index == 9,
              "el siguiente transporte sigue por el 10 (indice 9)");

        // A hole in the middle: 50 is on the disk.
        QVector<int> hole{fragmentMark(true, 50)};
        pick = pickNextFragment(false, videoCount, audioCount, 50, 0, hole);
        check(pick.found && pick.video && pick.index == 51,
              "un hueco en el medio se salta");
        check(pick.videoAdvance == 2, "el hueco cuenta como un salto");

        // The video is exhausted: the audio goes on, skipping what is there.
        QVector<int> allVideo;
        for (int i = 0; i < videoCount; ++i)
            allVideo.append(fragmentMark(true, i));
        allVideo.append(fragmentMark(false, 0));
        allVideo.append(fragmentMark(false, 1));
        allVideo.append(fragmentMark(false, 2));
        pick = pickNextFragment(false, videoCount, audioCount, 0, 0, allVideo);
        check(pick.found && !pick.video && pick.index == 3,
              "con el video entero bajado, se pide el audio que falta");
        check(pick.videoAdvance == videoCount && pick.audioAdvance == 4,
              "la caminata se lleva todo el video y 3 audios");

        // Nothing left: the pool does not ask for anything.
        QVector<int> everything = allVideo;
        for (int i = 3; i < audioCount; ++i)
            everything.append(fragmentMark(false, i));
        pick = pickNextFragment(false, videoCount, audioCount, 0, 0, everything);
        check(!pick.found, "sin nada pendiente no se pide nada");
        check(pick.videoAdvance == videoCount
                  && pick.audioAdvance == audioCount,
              "y la caminata llega hasta el final (no se queda a medias)");

        // Only the audio: the video fragments are not asked for at all.
        pick = pickNextFragment(true, videoCount, audioCount, 0, 0,
                                QVector<int>());
        check(pick.found && !pick.video && pick.index == 0,
              "en solo-audio se empieza por el audio 0");
        check(pick.videoAdvance == 0, "en solo-audio el video no se toca");

        // A download with NO index at all (nothing to do) does not loop.
        pick = pickNextFragment(false, 0, 0, 0, 0, QVector<int>());
        check(!pick.found, "un plan sin fragmentos no devuelve nada");

        // The real shape of a resumed 1080p download: 246 fragments, 77 of
        // them scattered on the disk.
        QVector<int> scattered;
        for (int i = 0; i < 77; ++i)
            scattered.append(fragmentMark(true, i * 3));
        pick = pickNextFragment(false, 246, 12, 0, 0, scattered);
        check(pick.found && pick.video && pick.index == 1,
              "con 77 fragmentos sueltos se pide el primero que falta");
    }

    // --- 2. the migration of the rows of the first version -----------------
    {
        QTemporaryDir tmp;
        check(tmp.isValid(), "hay una carpeta temporal");
        AppDatabase db(tmp.path() + QStringLiteral("/descargas.db"));
        check(db.open(), "la base de datos del test abre");
        DownloadRepo repo(db);
        dl::DownloadManager mgr(nullptr, &repo, nullptr, nullptr);
        // The delete only touches files INSIDE this folder: the test must never
        // reach the real folder of the downloads.
        mgr.setDirectory(tmp.path());

        const QString videoId = QStringLiteral("heredadaVid");
        const QString audioKey = downloadKey(videoId, true);

        // The row of the first version: the key is the VIDEO id, the quality
        // says mp3, and the bytes of the mp3 are in the folder of the video.
        DownloadInfo legacy;
        legacy.key = videoId;
        legacy.videoId = videoId;
        legacy.title = QStringLiteral("una cancion");
        legacy.quality = QStringLiteral("mp3");
        legacy.status = QStringLiteral("paused");
        legacy.parts = QVector<int>{dl::fragmentMark(false, 0),
                                    dl::fragmentMark(false, 1),
                                    dl::fragmentMark(false, 2)};
        legacy.bytesDone = 300;
        repo.upsert(legacy);
        // The fragments on the disk, in the folder of the OLD key.
        const QString oldParts = tmp.path() + QLatin1Char('/') + videoId
                                 + QStringLiteral(".parts");
        QDir().mkpath(oldParts);
        {
            QFile f(oldParts + QStringLiteral("/audio-0.seg"));
            f.open(QIODevice::WriteOnly);
            f.write("bytes que ya estaban");
        }
        check(QFileInfo::exists(oldParts + QStringLiteral("/audio-0.seg")),
              "la carpeta de fragmentos vieja existe");

        // The retry of that row: it migrates BEFORE starting.
        check(mgr.retry(videoId), "el reintento de la fila heredada se acepta");
        DownloadInfo migrated;
        check(!repo.get(videoId, migrated),
              "la fila vieja (clave = id del video) desaparece");
        check(repo.get(audioKey, migrated),
              "la fila del mp3 existe con su propia clave");
        check(migrated.quality == QLatin1String("mp3"),
              "la fila migrada sigue siendo mp3 (no se convierte en video)");
        check(migrated.videoId == videoId,
              "la fila migrada guarda el id del video");
        check(migrated.parts.size() == 3,
              "la fila migrada conserva el indice de fragmentos");
        check(migrated.status != QLatin1String("paused"),
              "la fila migrada se reanuda (no se queda pausada)");
        const QString newParts = tmp.path() + QLatin1Char('/') + audioKey
                                 + QStringLiteral(".parts");
        check(QFileInfo::exists(newParts + QStringLiteral("/audio-0.seg")),
              "los bytes ya bajados se MUEVEN a la carpeta de la clave nueva");
        check(!QFileInfo::exists(oldParts),
              "la carpeta vieja no se queda duplicada");
        mgr.remove(audioKey);
        repo.remove(audioKey);

        // The VIDEO of the same id must not be refused by the row of the mp3
        // (the mp3 was "done" in the row of the video and the video was never
        // downloaded, silently).
        DownloadInfo legacyDone;
        legacyDone.key = videoId;
        legacyDone.videoId = videoId;
        legacyDone.title = QStringLiteral("otro");
        legacyDone.quality = QStringLiteral("mp3");
        legacyDone.status = QStringLiteral("done");
        repo.upsert(legacyDone);
        mgr.start(videoId, QStringLiteral("otro"), QStringLiteral("canal"), 0);
        DownloadInfo videoRow;
        check(repo.get(videoId, videoRow),
              "el VIDEO se descarga aunque exista la fila heredada del mp3");
        check(videoRow.quality != QLatin1String("mp3"),
              "la fila del video no es la del mp3");
        check(repo.get(audioKey, migrated),
              "y el mp3 heredado se migro a su propia fila");
        check(migrated.status == QLatin1String("done"),
              "el mp3 que ya estaba terminado sigue terminado");
        mgr.remove(videoId);
        mgr.remove(audioKey);
        repo.remove(videoId);
        repo.remove(audioKey);

        // A row of a previous session that was interrupted KEEPS its index:
        // the upsert is a REPLACE, so an empty index would throw the bytes
        // away (and the download would start from zero without saying so).
        DownloadInfo half;
        half.key = QStringLiteral("aMediasVid");
        half.videoId = half.key;
        half.title = QStringLiteral("a medias");
        half.status = QStringLiteral("downloading");
        half.bytesPerSecond = 999;
        for (int i = 0; i < 77; ++i)
            half.parts.append(dl::fragmentMark(true, i));
        repo.upsert(half);
        mgr.resumePending();
        DownloadInfo after;
        check(repo.get(half.key, after), "la fila interrumpida sigue");
        check(after.parts.size() == 77,
              "la reanudacion CONSERVA los 77 fragmentos (no los borra)");
        check(after.status != QLatin1String("downloading"),
              "y ya no miente diciendo que esta descargando");
        check(after.bytesPerSecond == 0,
              "la velocidad de la sesion anterior no se arrastra");
        check(mgr.currentKey() == half.key,
              "la descarga interrumpida es la que arranca");
        mgr.remove(half.key);
        repo.remove(half.key);
    }

    // --- 3. the speed over a WINDOW, not since the start -------------------
    {
        dl::SpeedWindow w;
        check(w.bytesPerSecond(1000) == 0, "sin medidas la velocidad es 0");
        w.add(1000, 0);
        check(w.bytesPerSecond(1000) == 0,
              "con una sola medida la velocidad es 0");
        w.add(2000, 1000000);
        check(w.bytesPerSecond(2000) == 1000000,
              "1 MB en 1 s son 1 MB/s");

        // A STALL: the bytes do not move, so the speed is 0 even though the
        // download was fast before.
        w.add(13000, 1000000);
        check(w.bytesPerSecond(13000) == 0,
              "si los bytes no avanzan, la velocidad baja a 0 (no se queda alta)");

        // The old samples fall out of the window: the speed is the one of the
        // last seconds.
        dl::SpeedWindow w2;
        w2.add(0, 0);
        w2.add(1000, 50000000);          // 50 MB in the first second
        w2.add(20000, 51000000);         // then 1 MB in 19 s more
        const qint64 recent = w2.bytesPerSecond(20000);
        check(recent > 0 && recent < 50000000,
              "la ventana se olvida de lo viejo (no da la media del principio)");

        // Under a quarter of a second the number is noise: it must be 0 and
        // not a division by a tiny elapsed time.
        dl::SpeedWindow w3;
        w3.add(1000, 1000);
        w3.add(1050, 10000000);
        check(w3.bytesPerSecond(1050) == 0,
              "con menos de 250 ms no se inventa una velocidad");

        // A row that comes back with fewer bytes (a resume that restarted the
        // counter) is not a negative speed.
        dl::SpeedWindow w4;
        w4.add(1000, 5000000);
        w4.add(5000, 100);
        check(w4.bytesPerSecond(5000) == 0,
              "un contador que retrocede no da velocidad negativa");
    }

    // --- 4. a finished download is only "done" with a real file -----------
    {
        QTemporaryDir tmp;
        check(tmp.isValid(), "hay carpeta temporal para la verificacion del archivo");
        const QString empty = tmp.path() + QStringLiteral("/vacio.mkv");
        {
            QFile f(empty);
            f.open(QIODevice::WriteOnly);
        }
        check(!dl::outputIsUsable(empty),
              "un archivo de 0 bytes NO es un resultado valido");
        const QString full = tmp.path() + QStringLiteral("/bueno.mkv");
        {
            QFile f(full);
            f.open(QIODevice::WriteOnly);
            f.write("contenido");
        }
        check(dl::outputIsUsable(full), "un archivo con bytes si es valido");
        check(!dl::outputIsUsable(tmp.path() + QStringLiteral("/no-existe.mkv")),
              "un archivo que no existe no es valido");
        check(!dl::outputIsUsable(tmp.path()),
              "una carpeta no es un archivo valido");
        check(!dl::outputIsUsable(QString()), "una ruta vacia no es valida");
    }

    // --- 5. the sweep of orphan folders and the "all at once" -------------
    {
        QTemporaryDir tmp;
        check(tmp.isValid(), "hay carpeta temporal para el barrido");
        AppDatabase db(tmp.path() + QStringLiteral("/barrido.db"));
        check(db.open(), "la base del barrido abre");
        DownloadRepo repo(db);
        dl::DownloadManager mgr(nullptr, &repo, nullptr, nullptr);
        mgr.setDirectory(tmp.path());

        // A folder of fragments with NO row: it is rubbish.
        const QString orphan = tmp.path() + QStringLiteral("/huerfano.parts");
        QDir().mkpath(orphan);
        {
            QFile f(orphan + QStringLiteral("/video-0.seg"));
            f.open(QIODevice::WriteOnly);
            f.write("basura");
        }
        // A folder of fragments WITH its row: it is the resume of that one.
        const QString kept = tmp.path() + QStringLiteral("/vivo.parts");
        QDir().mkpath(kept);
        {
            QFile f(kept + QStringLiteral("/video-0.seg"));
            f.open(QIODevice::WriteOnly);
            f.write("bytes que sirven");
        }
        DownloadInfo alive;
        alive.key = QStringLiteral("vivo");
        alive.videoId = alive.key;
        alive.status = QStringLiteral("paused");
        repo.upsert(alive);

        const int removed = mgr.sweepOrphanParts();
        check(removed == 1, "el barrido borra solo la carpeta sin fila");
        check(!QFileInfo::exists(orphan), "la carpeta huerfana desaparece");
        check(QFileInfo::exists(kept + QStringLiteral("/video-0.seg")),
              "la carpeta de una descarga con fila se queda");

        // "Pausar todo" / "Reanudar todo".
        DownloadInfo running;
        running.key = QStringLiteral("corriendo");
        running.videoId = running.key;
        running.status = QStringLiteral("downloading");
        repo.upsert(running);
        DownloadInfo done;
        done.key = QStringLiteral("terminada");
        done.videoId = done.key;
        done.status = QStringLiteral("done");
        repo.upsert(done);

        mgr.pauseAll();
        DownloadInfo after;
        check(repo.get(running.key, after)
                  && after.status == QLatin1String("paused"),
              "pausar todo pausa la que corre");
        check(repo.get(alive.key, after)
                  && after.status == QLatin1String("paused"),
              "la que ya estaba en pausa sigue en pausa");
        check(repo.get(done.key, after)
                  && after.status == QLatin1String("done"),
              "pausar todo NO toca una descarga terminada");

        mgr.resumeAll();
        check(repo.get(alive.key, after)
                  && after.status != QLatin1String("paused"),
              "reanudar todo revive la pausada");
        check(repo.get(done.key, after)
                  && after.status == QLatin1String("done"),
              "reanudar todo no toca la terminada");
        mgr.remove(QStringLiteral("vivo"));
        mgr.remove(QStringLiteral("corriendo"));
        repo.remove(QStringLiteral("terminada"));
    }

    // --- 6. the queue under CHURN -----------------------------------------
    //
    // The state machine of the downloader (start / pause / resume / remove /
    // all at once) has many terminal points that have to free the downloader
    // and advance the queue. A loop that hammers them, with NO Yt (so the
    // downloads stay waiting for their formats, which is a legal state), is
    // what catches a "busy for ever" or a row that lies about its state.
    {
        QTemporaryDir tmp;
        check(tmp.isValid(), "hay carpeta temporal para el churn");
        AppDatabase db(tmp.path() + QStringLiteral("/churn.db"));
        check(db.open(), "la base del churn abre");
        DownloadRepo repo(db);
        dl::DownloadManager mgr(nullptr, &repo, nullptr, nullptr);
        mgr.setDirectory(tmp.path());

        QStringList ids;
        for (int i = 0; i < 10; ++i)
            ids.append(QStringLiteral("churn%1").arg(i));

        bool invHeld = true;
        for (int round = 0; round < 60; ++round) {
            const QString id = ids.at((round * 7) % ids.size());
            const QString key = downloadKey(id, false);
            switch (round % 5) {
            case 0:
                mgr.start(id, QStringLiteral("t"), QStringLiteral("c"), 0);
                break;
            case 1:
                mgr.pause(key);
                break;
            case 2:
                mgr.resume(key);
                break;
            case 3:
                mgr.remove(key);
                break;
            default:
                mgr.pauseAll();
                mgr.resumeAll();
                break;
            }
            // INVARIANT: with nothing in flight, no row can say "downloading"
            // (that is the lie the page shows for ever).
            if (!mgr.isBusy()) {
                for (const DownloadInfo &d : repo.all()) {
                    if (d.status == QLatin1String("downloading")
                        || d.status == QLatin1String("muxing")) {
                        invHeld = false;
                    }
                }
            }
        }
        check(invHeld,
              "sin nada en vuelo, ninguna fila miente diciendo que descarga");

        // Everything out: the downloader is free and the list is empty.
        mgr.pauseAll();
        for (const QString &id : ids)
            mgr.remove(downloadKey(id, false));
        check(!mgr.isBusy(), "tras sacar todo el downloader queda libre");
        check(repo.all().isEmpty(), "y la lista queda vacia");
    }

    std::fprintf(stderr, "DOWNLOAD-RESUME SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}
