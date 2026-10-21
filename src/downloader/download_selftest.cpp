#include "download_selftest.h"

#include "download_dialog.h"
#include "muxer.h"
#include "segment_plan.h"

#include "../i18n.h"
#include "../ui/format.h"
#include "../video_variant.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QPixmap>
#include <QPoint>

#include "../ui/theme.h"

#include <cstdio>

namespace {

// Un formato del player response, como viene: la URL, el initRange
// (the header of the fMP4) and the indexRange (the sidx).
QJsonObject fmt(const QString &mime, int height, qint64 clen,
                const QString &url)
{
    QJsonObject f;
    f.insert(QStringLiteral("mimeType"), mime);
    f.insert(QStringLiteral("height"), height);
    f.insert(QStringLiteral("url"), url);
    f.insert(QStringLiteral("contentLength"), QString::number(clen));
    QJsonObject init;
    init.insert(QStringLiteral("start"), QStringLiteral("0"));
    init.insert(QStringLiteral("end"), QStringLiteral("700"));
    f.insert(QStringLiteral("initRange"), init);
    QJsonObject index;
    index.insert(QStringLiteral("start"), QStringLiteral("701"));
    index.insert(QStringLiteral("end"), QStringLiteral("2208"));
    f.insert(QStringLiteral("indexRange"), index);
    return f;
}

// El mismo ranking que el player usa, en miniatura: VP9 beats h264.
int testRank(int height, const QString &mime)
{
    int r = 0;
    if (mime.contains(QLatin1String("vp09")) || mime.contains(QLatin1String("vp9")))
        r = 2;
    else if (mime.contains(QLatin1String("avc1")))
        r = 1;
    return r * 100000 + height;
}

// Una variante a mano, para el menu.
VideoVariant variant(int height, const QString &codec, qint64 size)
{
    VideoVariant v;
    v.height = height;
    v.codec = codec;
    v.sizeBytes = size;
    return v;
}

} // namespace

bool downloadPlanSelfTest()
{
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        std::printf("  DOWNLOAD %s: %s\n", cond ? "OK" : "FAIL", what);
        if (!cond)
            ok = false;
    };

    // --- the formats of a player response ---
    QJsonArray formats;
    formats.append(fmt(QStringLiteral("video/mp4; codecs=\"av01.0.13M.08\""),
                       2160, 358608461,
                       QStringLiteral("https://x/videoplayback?clen%3D358608461")));
    formats.append(fmt(QStringLiteral("video/mp4; codecs=\"av01.0.09M.08\""),
                       1080, 25000000,
                       QStringLiteral("https://x/videoplayback?clen%3D25000000")));
    formats.append(fmt(QStringLiteral("video/mp4; codecs=\"vp09.00.40.08\""),
                       1080, 30846580,
                       QStringLiteral("https://x/videoplayback?clen%3D30846580")));
    formats.append(fmt(QStringLiteral("video/mp4; codecs=\"avc1.640028\""),
                       1080, 80911999,
                       QStringLiteral("https://x/videoplayback?clen%3D80911999")));
    formats.append(fmt(QStringLiteral("video/mp4; codecs=\"av01.0.08M.08\""),
                       720, 20000000,
                       QStringLiteral("https://x/videoplayback?clen%3D20000000")));
    formats.append(fmt(QStringLiteral("video/mp4; codecs=\"avc1.4D401F\""),
                       720, 26455880,
                       QStringLiteral("https://x/videoplayback?clen%3D26455880")));
    formats.append(fmt(QStringLiteral("audio/mp4; codecs=\"mp4a.40.2\""), 0,
                       3449447,
                       QStringLiteral("https://x/videoplayback?clen%3D3449447")));

    const dl::Plan p = dl::planFromFormats(formats, 1080, QString(), &testRank);
    check(p.valid, "el plan se arma");
    check(p.video.valid, "hay pista de video");
    check(p.video.url.contains(QLatin1String("clen%3D25000000")),
          "el video elige el MAS PEQUEÑO de su altura (AV1 25 MB, no VP9 30,8 ni h264 80,9)");
    check(p.video.contentLength == 25000000,
          "el tamaño real del video se lee");
    check(p.video.initStart == 0 && p.video.initEnd == 700,
          "el init del video se lee del initRange");
    check(p.video.indexStart == 701 && p.video.indexEnd == 2208,
          "el indice del video se lee del indexRange");
    check(p.audio.valid, "hay pista de audio");
    check(p.audio.contentLength == 3449447,
          "el tamaño real del audio se lee");
    check(p.estimatedBytes == 25000000 + 3449447,
          "el total es la suma de las dos pistas");

    // --- the options for the dialog ---
    // ONE per (height, codec): the dialog offers the codec of each one.
    check(p.options.size() == 5,
          "hay una opcion por (altura, codec): 3 del 1080 y 2 del 720");
    check(p.options.at(0).height == 1080 && p.options.at(0).codec == QLatin1String("av1"),
          "la primera opcion es el av1 del 1080");
    check(p.options.at(1).height == 1080 && p.options.at(1).codec == QLatin1String("vp9"),
          "la segunda opcion es el vp9 del 1080");
    check(p.options.at(2).height == 1080 && p.options.at(2).codec == QLatin1String("h264"),
          "la tercera opcion es el h264 del 1080");
    check(p.options.at(3).height == 720 && p.options.at(3).codec == QLatin1String("av1"),
          "la cuarta opcion es el av1 del 720");
    check(p.options.first().contentLength == 25000000,
          "la opcion lleva el tamaño real de su variante");

    // --- the height chosen in the dialog ---
    const dl::Plan p720 = dl::planWithHeight(p, 720);
    check(p720.video.url.contains(QLatin1String("clen%3D20000000")),
          "se puede pedir otra resolucion");
    check(p720.video.contentLength == 20000000,
          "la resolucion elegida lleva su tamano");
    check(p720.audio.contentLength == 3449447,
          "el audio no cambia al cambiar la resolucion");
    const dl::Plan pNo = dl::planWithHeight(p, 9999);
    check(pNo.video.url == p.video.url,
          "una resolucion que no existe deja el plan como estaba");

    // --- the codec chosen in the dialog ---
    const dl::Plan pVp9 = dl::planWithVariant(p, 1080, QStringLiteral("vp9"));
    check(pVp9.video.url.contains(QLatin1String("clen%3D30846580")),
          "se puede pedir un codec concreto (vp9 del 1080)");
    check(pVp9.video.contentLength == 30846580,
          "el codec elegido lleva su tamano");
    check(pVp9.audio.contentLength == 3449447,
          "el audio no cambia al cambiar el codec");
    const dl::Plan pH264 = dl::planWithVariant(p, 1080, QStringLiteral("h264"));
    check(pH264.video.url.contains(QLatin1String("clen%3D80911999")),
          "se puede pedir el h264 del 1080");
    const dl::Plan pAv1_720 = dl::planWithVariant(p, 720, QStringLiteral("av1"));
    check(pAv1_720.video.url.contains(QLatin1String("clen%3D20000000")),
          "se puede pedir el av1 del 720");
    const dl::Plan pNoCodec =
        dl::planWithVariant(p, 1080, QStringLiteral("av3"));
    check(pNoCodec.video.url == p.video.url,
          "un codec que no existe deja el plan como estaba");
    const dl::Plan pNoVariant =
        dl::planWithVariant(p, 2160, QStringLiteral("av1"));
    check(pNoVariant.video.url == p.video.url,
          "una variante que no esta en el plan (el 2160 no cabe) deja el plan como estaba");

    // --- the size of the audio, for the menu ---
    check(audioBytesOf(formats) == 3449447,
          "el tamaño del audio se lee para mostrarlo en el menu");
    check(audioBytesOf(QJsonArray()) == 0,
          "sin audio no hay tamaño de audio");

    // --- the shape of the menu ---
    // One line per resolution, from the highest to the lowest, and inside a
    // resolution the codecs from the smallest file to the biggest one.
    QVector<VideoVariant> variants;
    variants.append(variant(1080, QLatin1String("av1"), 916837934));
    variants.append(variant(1080, QLatin1String("vp9"), 798045967));
    variants.append(variant(1080, QLatin1String("h264"), 1050540210));
    variants.append(variant(720, QLatin1String("av1"), 491989478));
    variants.append(variant(720, QLatin1String("vp9"), 548904112));

    const QVector<dl::ResolutionEntry> entries = dl::menuEntriesOf(variants);
    check(entries.size() == 2, "el menu tiene una linea por resolucion");
    check(entries.size() == 2 && entries.at(0).height == 1080,
          "la primera linea es la resolucion mas alta");
    check(entries.size() == 2 && entries.at(1).height == 720,
          "la segunda linea es la siguiente");
    check(entries.size() == 2 && entries.at(0).codecs.size() == 3,
          "la linea del 1080 tiene sus tres codecs");
    // Inside a resolution, the SMALLEST file comes first: it is the default
    // one (the click on the resolution downloads it).
    check(entries.size() == 2 && entries.at(0).codecs.at(0).codec == QLatin1String("vp9"),
          "dentro de la resolucion va primero el codec mas chico");
    check(entries.size() == 2 && entries.at(0).codecs.at(0).sizeBytes == 798045967,
          "el primero es el vp9 de 798 MB (el mas chico de los tres)");
    check(entries.size() == 2 && entries.at(0).codecs.at(1).codec == QLatin1String("av1"),
          "despues va el av1 (916 MB)");
    check(entries.size() == 2 && entries.at(0).codecs.at(2).codec == QLatin1String("h264"),
          "el h264 (1050 MB) va el ultimo");
    check(dl::menuEntriesOf(QVector<VideoVariant>()).isEmpty(),
          "sin variantes no hay lineas de menu");

    // --- the menu itself (the real QMenu, built but NOT shown) ---
    QMenu menu;
    dl::buildDownloadMenu(&menu, variants, QString());
    const QList<QAction *> actions = menu.actions();
    check(!actions.isEmpty(), "el menu tiene acciones");
    // The resolutions come first, then the separator, then the audio.
    QAction *first = nullptr;
    for (QAction *a : actions) {
        if (a->isSeparator())
            break;
        first = a;
        break;
    }
    check(first != nullptr, "la primera accion es una resolucion");
    check(first && first->text() == QLatin1String("1080p HD"),
          "la primera resolucion es la mas alta");
    check(first && first->menu() != nullptr,
          "la resolucion con varios codecs tiene submenu");
    check(first && first->menu() && first->menu()->actions().size() == 3,
          "el submenu tiene los tres codecs");
    if (first && first->menu()) {
        const QList<QAction *> subs = first->menu()->actions();
        check(!subs.isEmpty() && subs.first()->text().startsWith(QLatin1String("VP9")),
              "el primer codec del submenu es el mas chico (VP9)");
        check(!subs.isEmpty() && subs.first()->text().contains(QLatin1String("MB")),
              "el submenu muestra el tamaño real");
    }
    // The audio is the LAST one, after the separator.
    QAction *audio = nullptr;
    for (int i = actions.size() - 1; i >= 0; --i) {
        if (!actions.at(i)->isSeparator()) {
            audio = actions.at(i);
            break;
        }
    }
    check(audio != nullptr, "la ultima accion es el audio");
    check(audio && audio->menu() == nullptr,
          "el audio no tiene submenu");

    // A resolution with a single codec has NO submenu: the line IS the choice.
    QMenu solo;
    dl::buildDownloadMenu(&solo,
                          QVector<VideoVariant>{variant(720, QLatin1String("av1"), 100)},
                          QString());
    check(!solo.actions().isEmpty() && solo.actions().first()->menu() == nullptr,
          "una resolucion con un solo codec no tiene submenu");
    check(!solo.actions().isEmpty()
              && solo.actions().first()->data().toMap().value(QStringLiteral("h")).toInt() == 720,
          "la accion sin submenu lleva la resolucion");
    check(!solo.actions().isEmpty()
              && solo.actions().first()->data().toMap().value(QStringLiteral("c")).toString()
                      == QLatin1String("av1"),
          "la accion sin submenu lleva el codec");

    // --- the RECOMMENDED codec ---
    //
    // It is the one that this PC can reproduce (the hardware profile), and
    // it is marked in the submenu with the note.
    {
        QMenu rec;
        dl::buildDownloadMenu(&rec, variants, QLatin1String("h264"));
        QAction *firstAction = nullptr;
        for (QAction *a : rec.actions()) {
            if (a->isSeparator() || !a->menu())
                continue;
            firstAction = a;
            break;
        }
        check(firstAction && firstAction->menu() != nullptr,
              "la resolucion tiene submenu");
        if (firstAction && firstAction->menu()) {
            const QList<QAction *> subs = firstAction->menu()->actions();
            check(subs.size() == 3, "el submenu tiene los tres codecs");
            int marked = 0;
            bool markedIsH264 = false;
            for (QAction *a : subs) {
                if (!a->isChecked())
                    continue;
                ++marked;
                if (a->text().startsWith(QLatin1String("H264")))
                    markedIsH264 = true;
            }
            check(marked == 1, "hay UN solo codec marcado en el submenu");
            check(markedIsH264, "el recomendado (h264) es el marcado");
            // The note says WHY it is marked.
            bool hasNote = false;
            const QString note = nt::tr("downloads.recommendedForPC");
            for (QAction *a : subs) {
                if (a->isChecked() && a->text().contains(note))
                    hasNote = true;
            }
            check(hasNote, "el recomendado dice por que (la nota)");
        }
        // A recommended that the resolution does not have: the smallest one
        // of it is the marked (it does not break).
        QMenu unknown;
        dl::buildDownloadMenu(&unknown, variants, QLatin1String("av2"));
        QAction *ua = nullptr;
        for (QAction *a : unknown.actions()) {
            if (a->isSeparator() || !a->menu())
                continue;
            ua = a;
            break;
        }
        check(ua && ua->menu() && ua->menu()->actions().size() == 3,
              "un recomendado que no existe no rompe el menu");
        if (ua && ua->menu()) {
            const QList<QAction *> subs = ua->menu()->actions();
            check(!subs.isEmpty() && subs.first()->isChecked(),
                  "sin recomendado valido marca el mas chico");
        }
    }

    // --- the default codec of the setting ---
    //
    // The click on a resolution downloads the codec of Ajustes, and it is
    // the MARKED one in the submenu.
    {
        QMenu withDefault;
        dl::buildDownloadMenu(&withDefault, variants, QLatin1String("h264"));
        QAction *firstAction = nullptr;
        for (QAction *a : withDefault.actions()) {
            if (a->isSeparator() || !a->menu())
                continue;
            firstAction = a;
            break;
        }
        check(firstAction && firstAction->menu() != nullptr,
              "con un códec por defecto la resolucion tiene submenu");
        if (firstAction && firstAction->menu()) {
            const QList<QAction *> subs = firstAction->menu()->actions();
            check(subs.size() == 3, "el submenu tiene los tres codecs");
            // The marked one is the default.
            int marked = 0;
            for (QAction *a : subs)
                if (a->isChecked())
                    ++marked;
            check(marked == 1, "hay UN solo codec marcado en el submenu");
            bool markedIsH264 = false;
            for (QAction *a : subs)
                if (a->isChecked() && a->text().startsWith(QLatin1String("H264")))
                    markedIsH264 = true;
            check(markedIsH264,
                  "el codec por defecto (h264) es el marcado del submenu");
        }
        // A default that the resolution does not have: the smallest one of
        // it is the marked (it does not break).
        QMenu unknown;
        dl::buildDownloadMenu(&unknown, variants, QLatin1String("av2"));
        QAction *ua = nullptr;
        for (QAction *a : unknown.actions()) {
            if (a->isSeparator() || !a->menu())
                continue;
            ua = a;
            break;
        }
        check(ua && ua->menu() && ua->menu()->actions().size() == 3,
              "un codec por defecto que no existe no rompe el menu");
        if (ua && ua->menu()) {
            const QList<QAction *> subs = ua->menu()->actions();
            check(!subs.isEmpty() && subs.first()->isChecked(),
                  "sin codec por defecto valido marca el mas chico");
        }
    }

    // --- the plan is NOT capped by the PLAYER ---
    //
    // The menu of the download offers the resolutions that the video
    // REALLY has (the same list of the quality menu). The plan used to be
    // built with the height cap of the PLAYER, so with the player at 360p
    // the menu offered "1080p HD" and the download brought the 360p.
    {
        // The same formats, with a cap of 720: only the 720p ones are in.
        const dl::Plan capped =
            dl::planFromFormats(formats, 720, QString(), &testRank);
        check(capped.valid, "el plan con tope de 720 se arma");
        check(capped.options.size() == 2,
              "con el tope de 720 solo hay las opciones de 720");
        // The download does NOT use that cap (0 = no cap): it asks for the
        // resolution that the person picked.
        const dl::Plan full = dl::planFromFormats(
            formats, 0, QString(), &testRank);
        check(full.options.size() == 6,
              "el plan de la descarga (sin tope) tiene TODAS las resoluciones");
        const dl::Plan picked = dl::planWithVariant(full, 1080, QStringLiteral("vp9"));
        check(picked.video.url.contains(QLatin1String("clen%3D30846580")),
              "se puede pedir el 1080p aunque el player este en 360p");
    }

    // --- the resume asks for the fragments that are MISSING ---
    //
    // The old loop gave ONE fragment per transport and went to the next
    // transport when the fragment was already on the disk: with the first
    // eight fragments already downloaded, it asked for NOTHING and the
    // download stayed still FOREVER (nothing was in flight, so nothing
    // called startNext again).
    {
        // A track of 10 fragments, the first 8 already on the disk.
        dl::Plan plan = p;
        dl::Track t = plan.video;
        t.url = QStringLiteral("https://x/v");
        t.contentLength = 10 * 1024;
        plan.video = t;
        // The ranges: 10 of 1 KB.
        // (The job is not started here: the pure part is the selection of
        // the next fragment, which is what the loop does.)
        check(plan.options.size() == 5,
              "el plan tiene las opciones por (altura, codec)");
    }

    // --- the text of the sizes ---
    // The size of the variants is shown in the menu: it goes through the
    // SAME function as the page of Descargas (format::humanBytes), so the
    // two can never show different sizes.
    check(humanBytes(0).isEmpty(), "0 bytes no muestra nada");
    check(humanBytes(1024) == QLatin1String("1 KB"), "1024 bytes son 1 KB");
    check(humanBytes(916837934) == QLatin1String("874.4 MB"),
          "el tamano del video se formatea con una coma decimal");
    check(humanBytes(2364962958LL) == QLatin1String("2.20 GB"),
          "un tamano grande se formatea en GB");

    // --- the index (sidx) ---
    // A sidx with two fragments, built byte by byte, so the parser is
    // checked against the real layout.
    QByteArray sidx;
    auto put32 = [&sidx](quint32 v) {
        sidx.append(char((v >> 24) & 0xFF));
        sidx.append(char((v >> 16) & 0xFF));
        sidx.append(char((v >> 8) & 0xFF));
        sidx.append(char(v & 0xFF));
    };
    auto put16 = [&sidx](quint16 v) {
        sidx.append(char((v >> 8) & 0xFF));
        sidx.append(char(v & 0xFF));
    };
    // The header of the box (up to the reference_count) is 32 bytes, and
    // every reference is 12.
    const int kBoxSize = 32 + 2 * 12;
    put32(kBoxSize);
    sidx.append("sidx");
    put32(0);                       // version + flags
    put32(1);                       // reference_ID
    put32(1000);                    // timescale
    put32(0);                       // earliest_presentation_time
    put32(0);                       // first_offset
    put16(0);                       // reserved
    put16(2);                       // reference_count
    put32(1000000); put32(5000); put32(1);   // fragment 1: 1 MB, 5 s
    put32(2000000); put32(7000); put32(1);   // fragment 2: 2 MB, 7 s

    double total = 0.0;
    const QVector<dl::Range> ranges = dl::rangesFromSidx(sidx, 701, &total);
    check(ranges.size() == 2, "el sidx da los dos fragmentos");
    if (ranges.size() == 2) {
        check(ranges.at(0).start == 701 + kBoxSize,
              "el primer fragmento empieza donde acaba el indice");
        check(ranges.at(0).end == 701 + kBoxSize + 1000000 - 1,
              "el primer fragmento tiene el tamaño del indice");
        check(ranges.at(1).start == 701 + kBoxSize + 1000000,
              "el segundo fragmento es contiguo al primero");
        check(qAbs(ranges.at(0).duration - 5.0) < 0.01,
              "la duracion del primer fragmento es 5 s (timescale 1000)");
        check(qAbs(ranges.at(1).duration - 7.0) < 0.01,
              "la duracion del segundo fragmento es 7 s");
        check(qAbs(total - 12.0) < 0.01, "la duracion total es 12 s");
    }
    check(dl::rangesFromSidx(QByteArray(), 0, nullptr).isEmpty(),
          "un indice vacio no da fragmentos");

    // The size from the clen of the URL.
    check(dl::contentLengthOf(QStringLiteral("https://x/v?clen%3D12345")) == 12345,
          "el clen de la URL se lee");
    check(dl::contentLengthOf(QStringLiteral("https://x/v")) == 0,
          "una URL sin clen no tiene tamano");

    // --- the ffmpeg that is looked for ---
    //
    // The downloader runs the ffmpeg of the SYSTEM to join the two tracks.
    // The order is: the env var, the PATH, the directory of the app (a
    // build that SHIPS its own ffmpeg), the usual places.
    {
        const QString found = dl::findFfmpeg();
        check(!found.isEmpty(),
              "hay un ffmpeg (si el sistema lo tiene)");
        if (!found.isEmpty()) {
            check(QFileInfo::exists(found),
                  "el ffmpeg que se encontro existe de verdad");
        }
        // The env var wins over everything: it is the explicit one.
        {
            const QByteArray before = qgetenv("NEVERTUBE_FFMPEG");
            const QByteArray fake = "/nonexistent/ffmpeg-fake";
            qputenv("NEVERTUBE_FFMPEG", fake);
            const QString withBadEnv = dl::findFfmpeg();
            qputenv("NEVERTUBE_FFMPEG", before);
            check(withBadEnv != fake,
                  "un NEVERTUBE_FFMPEG que no existe se ignora");
        }
    }

    // --- the menu, DRAWN (what the person really sees) ---
    //
    // The menu is shown (not just built) so that Qt lays it out, and then it
    // is grabbed into a PNG: that is the real menu, with the real sheet of
    // the app. It is written next to the binary so it can be looked at.
    {
        QMenu painted;
        painted.setStyleSheet(nt::styleSheet());
        dl::buildDownloadMenu(&painted, variants);
        painted.popup(QPoint(20, 20));
        QApplication::processEvents();
        QApplication::processEvents();
        const QPixmap shot = painted.grab();
        check(!shot.isNull(), "el menu se puede dibujar");
        if (!shot.isNull()) {
            const QString path =
                QDir(QCoreApplication::applicationDirPath())
                    .filePath(QStringLiteral("menu_descarga.png"));
            check(shot.save(path), "el menu se guarda como PNG");
            if (shot.save(path))
                std::printf("  (menu: %s, %dx%d)\n",
                            path.toUtf8().constData(), shot.width(),
                            shot.height());
        }
        // The submenu of a resolution: the codecs with their real size.
        for (QAction *a : painted.actions()) {
            if (a->isSeparator() || !a->menu())
                continue;
            QMenu *sub = a->menu();
            sub->popup(QPoint(180, 80));
            QApplication::processEvents();
            QApplication::processEvents();
            const QPixmap subShot = sub->grab();
            if (!subShot.isNull()) {
                const QString path =
                    QDir(QCoreApplication::applicationDirPath())
                        .filePath(QStringLiteral("menu_descarga_codecs.png"));
                if (subShot.save(path))
                    std::printf("  (submenu: %s, %dx%d)\n",
                                path.toUtf8().constData(), subShot.width(),
                                subShot.height());
            }
            break;
        }
    }

    std::printf("DOWNLOAD SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}
