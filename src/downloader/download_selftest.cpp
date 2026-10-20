#include "download_selftest.h"

#include "segment_plan.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cstdio>

namespace {

// A format of the player response, as it comes: the URL, the initRange
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

// The same ranking that the player uses, in miniature: VP9 beats h264.
int testRank(int height, const QString &mime)
{
    int r = 0;
    if (mime.contains(QLatin1String("vp09")) || mime.contains(QLatin1String("vp9")))
        r = 2;
    else if (mime.contains(QLatin1String("avc1")))
        r = 1;
    return r * 100000 + height;
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
    formats.append(fmt(QStringLiteral("video/mp4; codecs=\"vp09.00.40.08\""),
                       1080, 30846580,
                       QStringLiteral("https://x/videoplayback?clen%3D30846580")));
    formats.append(fmt(QStringLiteral("video/mp4; codecs=\"avc1.640028\""),
                       1080, 80911999,
                       QStringLiteral("https://x/videoplayback?clen%3D80911999")));
    formats.append(fmt(QStringLiteral("video/mp4; codecs=\"avc1.4D401F\""),
                       720, 26455880,
                       QStringLiteral("https://x/videoplayback?clen%3D26455880")));
    formats.append(fmt(QStringLiteral("audio/mp4; codecs=\"mp4a.40.2\""), 0,
                       3449447,
                       QStringLiteral("https://x/videoplayback?clen%3D3449447")));

    const dl::Plan p = dl::planFromFormats(formats, 1080, QString(), &testRank);
    check(p.valid, "el plan se arma");
    check(p.video.valid, "hay pista de video");
    check(p.video.url.contains(QLatin1String("clen%3D30846580")),
          "el video elige el MAS PEQUEÑO de su altura (VP9 30,8 MB, no h264 80,9 MB)");
    check(p.video.contentLength == 30846580,
          "el tamaño real del video se lee");
    check(p.video.initStart == 0 && p.video.initEnd == 700,
          "el init del video se lee del initRange");
    check(p.video.indexStart == 701 && p.video.indexEnd == 2208,
          "el indice del video se lee del indexRange");
    check(p.audio.valid, "hay pista de audio");
    check(p.audio.contentLength == 3449447,
          "el tamaño real del audio se lee");
    check(p.estimatedBytes == 30846580 + 3449447,
          "el total es la suma de las dos pistas");

    // --- the options for the dialog ---
    check(p.options.size() == 2, "hay una opcion por altura que cabe (1080p y 720p)");
    check(p.options.first().height == 1080,
          "la primera opcion es la mas alta que cabe");
    check(p.options.first().contentLength == 30846580,
          "la opcion lleva el tamaño real de su variante");

    // --- the height chosen in the dialog ---
    const dl::Plan p720 = dl::planWithHeight(p, 720);
    check(p720.video.url.contains(QLatin1String("clen%3D26455880")),
          "se puede pedir otra resolucion");
    check(p720.video.contentLength == 26455880,
          "la resolucion elegida lleva su tamano");
    check(p720.audio.contentLength == 3449447,
          "el audio no cambia al cambiar la resolucion");
    const dl::Plan pNo = dl::planWithHeight(p, 9999);
    check(pNo.video.url == p.video.url,
          "una resolucion que no existe deja el plan como estaba");

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

    std::printf("DOWNLOAD SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}
