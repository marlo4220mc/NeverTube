#include "segment_plan.h"

#include <QRegularExpression>

namespace dl {

namespace {

// Reads an attribute of an #EXT-X-MEDIA or #EXT-X-STREAM-INF line:
//   #EXT-X-MEDIA:URI="...",TYPE=AUDIO,GROUP-ID="x",NAME="en",LANGUAGE="en"
// The value may be quoted or bare, and the attribute names are upper case.
QString attr(const QString &line, const QString &name)
{
    const QRegularExpression re(
        QStringLiteral("\\b%1=(\"([^\"]*)\"|[^,]*)").arg(name));
    const QRegularExpressionMatch m = re.match(line);
    if (!m.hasMatch())
        return QString();
    QString v = m.captured(2);
    if (v.isNull())
        v = m.captured(1);
    return v;
}

// True when the rendition is the ORIGINAL track and not a dub. YouTube marks
// the original with "acont=original" inside its URI (URL-encoded as
// "acont%3Doriginal"); the dubs have their own itag and their own group.
//
// This is the SAME rule the player uses (isOriginalAudio in yt.cpp): the
// downloader must pick the same track the player plays.
bool isOriginalTrack(const QString &line)
{
    return line.contains(QLatin1String("acont=original"))
        || line.contains(QLatin1String("acont%3Doriginal"));
}

// The audio itag inside the URI of a rendition: YouTube's HLS URIs carry
// "/itag/233/" and friends.
int audioItag(const QString &uri)
{
    const int pos = uri.indexOf(QLatin1String("/itag/"));
    if (pos < 0)
        return 0;
    const int start = pos + int(QLatin1String("/itag/").size());
    int end = start;
    while (end < uri.size() && uri.at(end).isDigit())
        ++end;
    if (end == start)
        return 0;
    return uri.mid(start, end - start).toInt();
}

// Quality of an audio rendition, from its itag.
//
// A master does NOT always mark the original with "acont=original": when the
// video has a single language there is nothing to disambiguate and the
// master just lists the renditions. What then distinguishes them is the
// BITRATE, and the itag says which one is which (measured on a real master:
// itag 233 = 48 kbps, itag 234 = 128 kbps, both with NAME="Default" and no
// LANGUAGE).
//
// The rules, from YouTube's itag table:
//   140, 234 -> m4a 128 kbps  (the original)
//   139, 233 -> m4a 48/64 kbps (a lower rendition)
//   249-251 -> Opus in WebM
// A rendition whose itag is not recognised scores 0, and the `acont=original`
// marker (checked apart) always wins.
int audioQuality(const QString &uri)
{
    switch (audioItag(uri)) {
    case 140:
    case 234:
        return 3;
    case 139:
    case 233:
        return 2;
    case 251:
    case 250:
    case 249:
        return 1;
    default:
        return 0;
    }
}

int resolutionHeight(const QString &line)
{
    const QString res = attr(line, QStringLiteral("RESOLUTION"));
    const int x = res.indexOf(QLatin1Char('x'));
    if (x <= 0)
        return 0;
    return res.mid(x + 1).toInt();
}

} // namespace

Plan planFromMaster(const QByteArray &masterBody, int maxHeight,
                    const QString &prefLang,
                    int (*codecRank)(int height, const QString &codecs))
{
    Plan plan;
    if (masterBody.isEmpty()) {
        plan.error = QStringLiteral("master vacio");
        return plan;
    }

    const QStringList lines = QString::fromUtf8(masterBody).split(QLatin1Char('\n'));

    // --- video variant ---
    // The HEIGHT wins, not the codec. The player ranks the codec first
    // (codecRankedScore) because it wants the one the GPU decodes, and that
    // is right for playback. For a DOWNLOAD the order is inverted: the user
    // asked for the file, so the best picture wins, and a VP9 1080p that
    // only decodes in software is still a perfectly good file to keep.
    // The codec only breaks the tie between two variants of the SAME height.
    QString bestInf;
    QString bestUrl;
    int bestHeight = 0;
    int bestScore = 0;
    bool haveBest = false;
    bool nextIsUrl = false;

    // --- audio renditions ---
    struct Rendition
    {
        QString line;
        QString uri;
        QString group;
        bool original = true;
    };
    QVector<Rendition> renditions;

    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("#EXT-X-MEDIA:"))) {
            if (line.contains(QLatin1String("TYPE=AUDIO"))) {
                Rendition r;
                r.line = line;
                r.uri = attr(line, QStringLiteral("URI"));
                r.group = attr(line, QStringLiteral("GROUP-ID"));
                r.original = isOriginalTrack(line);
                if (!r.uri.isEmpty())
                    renditions.append(r);
            }
            nextIsUrl = false;
        } else if (line.startsWith(QLatin1String("#EXT-X-STREAM-INF:"))) {
            const int height = resolutionHeight(line);
            const int score = codecRank ? codecRank(height, line) : height;
            if (height > 0 && height <= maxHeight) {
                // Higher height wins. The codec (the score) only breaks the
                // tie between two variants of the same height.
                if (!haveBest || height > bestHeight
                    || (height == bestHeight && score > bestScore)) {
                    bestInf = line;
                    bestUrl.clear();
                    bestScore = score;
                    bestHeight = height;
                    haveBest = true;
                    nextIsUrl = true;
                } else {
                    nextIsUrl = false;
                }
            } else {
                nextIsUrl = false;
            }
        } else if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) {
            if (nextIsUrl) {
                bestUrl = line;
                nextIsUrl = false;
            }
        } else {
            nextIsUrl = false;
        }
    }

    if (!haveBest || bestUrl.isEmpty()) {
        plan.error = QStringLiteral("el master no tiene ninguna variante de video");
        return plan;
    }

    plan.video.playlistUrl = bestUrl;
    plan.video.height = resolutionHeight(bestInf);
    plan.video.codecs = attr(bestInf, QStringLiteral("CODECS"));
    plan.video.bandwidth = attr(bestInf, QStringLiteral("BANDWIDTH")).toLongLong();
    plan.video.valid = true;

    // --- audio rendition ---
    // The rule, in order:
    //  1. the rendition the user asked for (a dub in prefLang) if there is
    //     one;
    //  2. the ORIGINAL track;
    //  3. otherwise the best rendition by bitrate (a master that does not
    //     mark the original still lists several, and picking the lowest
    //     would download 48 kbps audio for a 1080p video).
    //
    // The difference with the player is that here the URI of the CHOSEN
    // track is kept, because the downloader has to fetch that playlist
    // itself (the player lets mpv resolve it).
    const Rendition *chosen = nullptr;
    if (!prefLang.isEmpty()) {
        for (const Rendition &r : renditions) {
            const QString lang = attr(r.line, QStringLiteral("LANGUAGE"));
            if (!lang.isEmpty() && lang.startsWith(prefLang, Qt::CaseInsensitive)) {
                chosen = &r;
                break;
            }
        }
    }
    if (!chosen) {
        for (const Rendition &r : renditions) {
            if (r.original) {
                chosen = &r;
                break;
            }
        }
    }
    if (!chosen) {
        int bestQuality = -1;
        for (const Rendition &r : renditions) {
            const int q = audioQuality(r.uri);
            if (q > bestQuality) {
                bestQuality = q;
                chosen = &r;
            }
        }
    }
    if (!chosen && !renditions.isEmpty()) {
        chosen = &renditions.first();
    }
    if (chosen) {
        plan.audio.playlistUrl = chosen->uri;
        plan.audio.language = attr(chosen->line, QStringLiteral("LANGUAGE"));
        plan.audio.name = attr(chosen->line, QStringLiteral("NAME"));
        plan.audio.isOriginal = chosen->original;
        plan.audio.valid = true;
    }

    plan.valid = true;
    return plan;
}

QVector<Segment> segmentsFromMediaPlaylist(const QByteArray &mediaBody)
{
    QVector<Segment> out;
    if (mediaBody.isEmpty())
        return out;

    const QStringList lines = QString::fromUtf8(mediaBody).split(QLatin1Char('\n'));
    double pendingDuration = 0.0;
    bool havePending = false;

    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("#EXTINF:"))) {
            const QString rest = line.mid(int(QLatin1String("#EXTINF:").size()));
            const int comma = rest.indexOf(QLatin1Char(','));
            pendingDuration = rest.left(comma >= 0 ? comma : rest.size()).toDouble();
            havePending = true;
        } else if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) {
            Segment s;
            s.url = line.trimmed();
            s.duration = havePending ? pendingDuration : 0.0;
            out.append(s);
            havePending = false;
        }
    }
    return out;
}

qint64 estimateBytes(qint64 bandwidth, double durationSeconds)
{
    if (bandwidth <= 0 || durationSeconds <= 0.0)
        return 0;
    return static_cast<qint64>(static_cast<double>(bandwidth) * durationSeconds / 8.0);
}

} // namespace dl
