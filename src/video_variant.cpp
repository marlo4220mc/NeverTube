#include "video_variant.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>

namespace {

// El prefijo del códec, en el orden en que se buscan: los prefijos largos
// primero, para que "vp09" se identifique como vp9 (y no como "vp0") y
// "avc1"/"avc3" como el mismo h264.
struct Family
{
    const char *prefix;
    const char *name;
};

const Family kFamilies[] = {
    {"av01", "av1"},
    {"av1", "av1"},
    {"vp09", "vp9"},
    {"vp9", "vp9"},
    {"avc1", "h264"},
    {"avc3", "h264"},
};

// El idioma de una pista de audio: el id de la pista viene con el idioma
// como prefijo ("en.4" -> "en", "zh-Hant.3").
QString audioLanguageOf(const QJsonObject &format)
{
    const QJsonObject track = format.value(QStringLiteral("audioTrack")).toObject();
    QString id = track.value(QStringLiteral("id")).toString();
    const int dot = id.indexOf(QLatin1Char('.'));
    if (dot > 0)
        id.truncate(dot);
    if (!id.isEmpty())
        return id;
    return track.value(QStringLiteral("languageCode")).toString();
}

// True cuando la pista es la original y no un doblaje: lo marca el nombre
// que se muestra ("Inglés original"), la misma regla del reproductor.
bool isOriginalOf(const QJsonObject &format)
{
    const QJsonObject track = format.value(QStringLiteral("audioTrack")).toObject();
    const QString displayName =
        track.value(QStringLiteral("displayName")).toString();
    if (displayName.contains(QLatin1String("original"), Qt::CaseInsensitive))
        return true;
    const bool isDefault = track.value(QStringLiteral("audioIsDefault")).toBool();
    const QString lang = track.value(QStringLiteral("languageCode")).toString();
    if (isDefault && lang.isEmpty() && !displayName.isEmpty())
        return true;
    return lang.isEmpty() && displayName.isEmpty();
}

} // namespace

// El tamaño real de una pista: el contentLength del formato, o el "clen"
// de su URL (el adaptive no lo trae en la URL, pero el master sí).
qint64 formatBytes(const QJsonObject &f)
{
    qint64 n = f.value(QStringLiteral("contentLength")).toString().toLongLong();
    if (n > 0)
        return n;
    // A negative size is not a size (the server sends a sentinel for the
    // tracks whose size it does not know): 0 means "unknown", not "-1".
    if (n < 0)
        return 0;
    const QString url = f.value(QStringLiteral("url")).toString();
    const int pos = url.indexOf(QLatin1String("clen%3D"));
    if (pos < 0)
        return 0;
    const int start = pos + int(QLatin1String("clen%3D").size());
    int end = start;
    while (end < url.size() && url.at(end).isDigit())
        ++end;
    if (end == start)
        return 0;
    return url.mid(start, end - start).toLongLong();
}

QJsonObject preferredAudioFormat(const QJsonArray &adaptiveFormats,
                                  const QString &prefLang)
{
    // The track is chosen in TWO steps, and the order matters:
    //
    // 1. The MP4 (fMP4 with a sidx) beats the WebM, ALWAYS: the sidx is what
    //    lets the downloader resume fragment by fragment (and ask for pieces of
    //    ~2 MB, which is what evades YouTube's per-request throttle). A WebM
    //    has no index of its clusters and has to be cut blind.
    // 2. Inside the same family, the SMALLEST one: the rule is shared with the
    //    player, which does not need the heaviest track to sound the same.
    //
    // The Opus of the WebM may have a higher bitrate than the AAC, but
    // choosing it costs the index: the download of the audio would be blind.
    QJsonObject best;
    const QString pref = prefLang.trimmed().toLower();
    for (const QJsonValue &val : adaptiveFormats) {
        const QJsonObject f = val.toObject();
        if (f.value(QStringLiteral("url")).toString().isEmpty())
            continue;
        const QString mime = f.value(QStringLiteral("mimeType")).toString();
        if (!mime.startsWith(QLatin1String("audio/")))
            continue;
        const QString lang = audioLanguageOf(f);
        bool match = false;
        if (!pref.isEmpty())
            match = !lang.isEmpty() && lang.toLower().startsWith(pref);
        else
            match = isOriginalOf(f);
        if (!match)
            continue;
        // The mp4 (with a sidx) beats the WebM (without one).
        const bool isMp4 = mime.startsWith(QLatin1String("audio/mp4"));
        if (!best.isEmpty()) {
            const QString bestMime =
                best.value(QStringLiteral("mimeType")).toString();
            const bool bestIsMp4 =
                bestMime.startsWith(QLatin1String("audio/mp4"));
            if (isMp4 != bestIsMp4) {
                if (!isMp4)
                    continue;
            } else {
                const qint64 current = formatBytes(best);
                const qint64 candidate = formatBytes(f);
                if (current > 0 && candidate > 0 && candidate >= current)
                    continue;
            }
        }
        best = f;
    }
    return best;
}

qint64 audioBytesOf(const QJsonArray &adaptiveFormats)
{
    return formatBytes(preferredAudioFormat(adaptiveFormats));
}

QString videoCodecFamily(const QString &mimeOrCodecs)
{
    if (mimeOrCodecs.isEmpty())
        return QString();
    const QString s = mimeOrCodecs.toLower();

    // The mimeType brings the codecs in quotes: "video/mp4; codecs=\"av01.0.09M.08\"".
    // What matters is the part AFTER "codecs=", but a bare codec works too.
    QString probe;
    const int at = s.indexOf(QLatin1String("codecs="));
    if (at >= 0) {
        probe = s.mid(at + int(QLatin1String("codecs=").size()));
        probe.remove(QLatin1Char('"'));
        probe = probe.section(QLatin1Char(';'), 0, 0);
        probe = probe.trimmed();
    } else {
        // Without "codecs=", the mimeType itself is not a codec: take what
        // is after the slash ("video/webm" -> "webm", which is not a codec).
        probe = s.section(QLatin1Char(';'), 0, 0).trimmed();
        const int slash = probe.indexOf(QLatin1Char('/'));
        if (slash >= 0)
            probe = probe.mid(slash + 1);
    }
    if (probe.isEmpty())
        return QString();

    for (const Family &f : kFamilies) {
        if (probe.startsWith(QLatin1String(f.prefix)))
            return QString::fromLatin1(f.name);
    }
    return QString();
}

QVector<VideoVariant> videoVariantsOf(const QJsonArray &adaptiveFormats,
                                      bool requireInitRange)
{
    QVector<VideoVariant> out;
    for (const QJsonValue &val : adaptiveFormats) {
        const QJsonObject f = val.toObject();
        // The ones that come encrypted (signatureCipher) cannot be asked by
        // byte range: they are not downloadable.
        if (f.value(QStringLiteral("url")).toString().isEmpty())
            continue;
        const QString mime = f.value(QStringLiteral("mimeType")).toString();
        if (!mime.startsWith(QLatin1String("video/")))
            continue;
        const int height = f.value(QStringLiteral("height")).toInt();
        if (height <= 0)
            continue;
        // Without the init segment it is not a playable fMP4: the downloader
        // cannot assemble it.
        if (requireInitRange
            && f.value(QStringLiteral("initRange")).toObject().isEmpty())
            continue;
        const QString codec = videoCodecFamily(mime);
        if (codec.isEmpty())
            continue;
        const qint64 size = formatBytes(f);

        // One variant per (height, codec): of the same codec with another
        // bitrate, the SMALLEST wins (for a download the size is what
        // matters).
        int at = -1;
        for (int i = 0; i < out.size(); ++i) {
            if (out.at(i).height == height && out.at(i).codec == codec) {
                at = i;
                break;
            }
        }
        if (at >= 0) {
            if (size > 0 && out.at(at).sizeBytes > 0 && size >= out.at(at).sizeBytes)
                continue;
            out[at].sizeBytes = size;
            out[at].format = f;
            continue;
        }
        VideoVariant v;
        v.height = height;
        v.codec = codec;
        v.sizeBytes = size;
        v.format = f;
        out.append(v);
    }

    // From the highest to the lowest, and inside a height, AV1 first (the
    // smallest one for the same picture), then VP9, then H264.
    std::sort(out.begin(), out.end(), [](const VideoVariant &a, const VideoVariant &b) {
        if (a.height != b.height)
            return a.height > b.height;
        const int ra = a.codec == QLatin1String("av1")
                           ? 0
                           : (a.codec == QLatin1String("vp9") ? 1 : 2);
        const int rb = b.codec == QLatin1String("av1")
                           ? 0
                           : (b.codec == QLatin1String("vp9") ? 1 : 2);
        if (ra != rb)
            return ra < rb;
        return a.sizeBytes < b.sizeBytes;
    });
    return out;
}
