#include "video_variant_selftest.h"

#include "video_variant.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cstdio>

namespace {

// Un formato del player response, como viene.
QJsonObject fmt(const QString &mime, int height, qint64 clen,
                const QString &url)
{
    QJsonObject f;
    f.insert(QStringLiteral("mimeType"), mime);
    f.insert(QStringLiteral("height"), height);
    f.insert(QStringLiteral("url"), url);
    f.insert(QStringLiteral("contentLength"), QString::number(clen));
    return f;
}

} // namespace

bool videoVariantSelfTest()
{
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        std::printf("  VARIANT %s: %s\n", cond ? "OK" : "FAIL", what);
        if (!cond)
            ok = false;
    };

    // --- the family of the codec ---
    check(videoCodecFamily(QStringLiteral("av01.0.09M.08")) == QLatin1String("av1"),
          "av01 es av1");
    check(videoCodecFamily(QStringLiteral("vp09.00.40.08")) == QLatin1String("vp9"),
          "vp09 es vp9");
    check(videoCodecFamily(QStringLiteral("vp9")) == QLatin1String("vp9"),
          "vp9 a secas es vp9");
    check(videoCodecFamily(QStringLiteral("avc1.640028")) == QLatin1String("h264"),
          "avc1 es h264");
    check(videoCodecFamily(QStringLiteral("avc3.4d401f")) == QLatin1String("h264"),
          "avc3 es h264 tambien");
    check(videoCodecFamily(QStringLiteral("video/mp4; codecs=\"av01.0.09M.08\""))
              == QLatin1String("av1"),
          "el mimeType completo tambien se lee");
    check(videoCodecFamily(QStringLiteral("video/webm; codecs=\"vp9\""))
              == QLatin1String("vp9"),
          "el mimeType de webm tambien se lee");
    check(videoCodecFamily(QStringLiteral("av01")).isEmpty()
              == false,
          "un prefijo sin puntos tambien se lee");
    check(videoCodecFamily(QStringLiteral("audio/mp4; codecs=\"mp4a.40.2\"")).isEmpty(),
          "el audio no es una variante de video");

    // --- the variants of a video ---
    QJsonArray formats;
    formats.append(fmt(QStringLiteral("video/mp4; codecs=\"av01.0.09M.08\""),
                       1080, 916837934, QStringLiteral("https://x/av1")));
    formats.append(fmt(QStringLiteral("video/webm; codecs=\"vp9\""), 1080,
                       798045967, QStringLiteral("https://x/vp9")));
    formats.append(fmt(QStringLiteral("video/mp4; codecs=\"avc1.64002A\""), 1080,
                       1050540210, QStringLiteral("https://x/h264")));
    formats.append(fmt(QStringLiteral("video/mp4; codecs=\"av01.0.08M.08\""),
                       720, 491989478, QStringLiteral("https://x/av1_720")));
    formats.append(fmt(QStringLiteral("video/webm; codecs=\"vp9\""), 720,
                       548904112, QStringLiteral("https://x/vp9_720")));
    // One that comes encrypted: it cannot be asked by range, so it does not
    // count as a downloadable variant.
    QJsonObject cipher = fmt(QStringLiteral("video/mp4; codecs=\"av01.0.09M.08\""),
                             1080, 999, QString());
    cipher.remove(QStringLiteral("url"));
    cipher.insert(QStringLiteral("signatureCipher"),
                  QStringLiteral("s=abc&sp=sig&url=https%3A%2F%2Fx%2Fenc"));
    formats.append(cipher);
    // The audio is not a variant.
    formats.append(fmt(QStringLiteral("audio/mp4; codecs=\"mp4a.40.2\""), 0,
                       13218813, QStringLiteral("https://x/audio")));

    const QVector<VideoVariant> v = videoVariantsOf(formats);
    check(v.size() == 5, "hay una variante por (altura, codec): 3 del 1080 y 2 del 720");
    check(v.size() == 5 && v.at(0).height == 1080 && v.at(1).height == 1080
              && v.at(2).height == 1080,
          "las tres primeras son las del 1080");
    check(v.size() == 5 && v.at(3).height == 720 && v.at(4).height == 720,
          "las dos ultimas son las del 720");
    check(v.size() == 5 && v.at(0).codec == QLatin1String("av1"),
          "la primera variante del 1080 es el av1");
    check(v.size() == 5 && v.at(1).codec == QLatin1String("vp9"),
          "la segunda variante del 1080 es el vp9");
    check(v.size() == 5 && v.at(2).codec == QLatin1String("h264"),
          "la tercera variante del 1080 es el h264");
    check(v.size() == 5 && v.at(0).sizeBytes == 916837934,
          "la variante lleva su tamano real");
    check(v.size() == 5 && v.at(0).format.value(QStringLiteral("url")).toString()
              == QLatin1String("https://x/av1"),
          "la variante lleva el formato del que viene");

    // The same codec with another bitrate: the SMALLEST one wins (for a
    // download the size is what matters).
    QJsonArray dup;
    dup.append(fmt(QStringLiteral("video/mp4; codecs=\"av01.0.09M.08\""), 1080,
                   500, QStringLiteral("https://x/small")));
    dup.append(fmt(QStringLiteral("video/mp4; codecs=\"av01.0.12M.08\""), 1080,
                   900, QStringLiteral("https://x/big")));
    const QVector<VideoVariant> dv = videoVariantsOf(dup);
    check(dv.size() == 1, "el mismo codec con dos bitrates es UNA variante");
    check(dv.size() == 1 && dv.at(0).sizeBytes == 500,
          "de los dos bitrates gana el mas chico");

    // An empty list is an empty list (not a crash).
    check(videoVariantsOf(QJsonArray()).isEmpty(), "sin formatos no hay variantes");

    // The audio: the MP4 (with a sidx, so it can be fetched and resumed
    // fragment by fragment) beats the WebM even when the WebM has a higher
    // bitrate; inside the same family the SMALLEST one wins.
    QJsonArray audioFmts;
    QJsonObject aac = fmt(QStringLiteral("audio/mp4; codecs=\"mp4a.40.2\""), 0, 10000000,
                          QStringLiteral("https://x/aac"));
    aac.insert(QStringLiteral("bitrate"), 128000);
    audioFmts.append(aac);

    QJsonObject opus = fmt(QStringLiteral("audio/webm; codecs=\"opus\""), 0, 12000000,
                           QStringLiteral("https://x/opus"));
    opus.insert(QStringLiteral("bitrate"), 160000);
    audioFmts.append(opus);

    const QJsonObject bestAud = preferredAudioFormat(audioFmts);
    check(bestAud.value(QStringLiteral("mimeType")).toString().contains(QLatin1String("mp4")),
          "el audio preferido es el MP4 (tiene sidx) aunque el opus tenga mas bitrate");

    // Without an mp4, the smallest one (the same rule as the player: the
    // family does not change, so the lightest track wins).
    QJsonArray onlyWebm;
    QJsonObject opusLow = fmt(QStringLiteral("audio/webm; codecs=\"opus\""), 0, 5000000,
                              QStringLiteral("https://x/opus-low"));
    opusLow.insert(QStringLiteral("bitrate"), 64000);
    onlyWebm.append(opusLow);
    onlyWebm.append(opus);
    const QJsonObject bestWebm = preferredAudioFormat(onlyWebm);
    check(bestWebm.value(QStringLiteral("url")).toString() == QLatin1String("https://x/opus-low"),
          "sin mp4 gana el mas chico de la familia");

    std::printf("VARIANT SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}
