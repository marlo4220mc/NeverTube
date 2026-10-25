#include "segment_plan.h"

#include "../i18n.h"

#include "../video_variant.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

namespace dl {

namespace {

// The size of a format: the URL of the adaptive does NOT carry the "clen"
// (that is of the master), so it comes from the format itself.
qint64 sizeOfFormat(const QJsonObject &f)
{
    qint64 n = contentLengthOf(f.value(QStringLiteral("url")).toString());
    if (n <= 0)
        n = f.value(QStringLiteral("contentLength")).toString().toLongLong();
    // A NEGATIVE size is not a size: it comes from a format whose
    // contentLength is a sentinel (the server sends -1 for the tracks whose
    // size it does not know) or from a truncated read. The size that is
    // used is the one of the URL (the "clen"), and when there is none the
    // track is left without size (0) instead of with a negative one: a
    // negative total made the progress bar go backwards and the download
    // look like it was finished.
    if (n < 0)
        n = 0;
    return n;
}

qint64 rangeStart(const QJsonObject &r)
{
    return r.value(QStringLiteral("start")).toString().toLongLong();
}

qint64 rangeEnd(const QJsonObject &r)
{
    return r.value(QStringLiteral("end")).toString().toLongLong();
}

// The language of an audio track: the id of its track is prefixed with it


void fillTrack(Track &t, const QJsonObject &f)
{
    t.url = f.value(QStringLiteral("url")).toString();
    t.format = f;
    t.initStart = rangeStart(f.value(QStringLiteral("initRange")).toObject());
    t.initEnd = rangeEnd(f.value(QStringLiteral("initRange")).toObject());
    t.indexStart = rangeStart(f.value(QStringLiteral("indexRange")).toObject());
    t.indexEnd = rangeEnd(f.value(QStringLiteral("indexRange")).toObject());
    t.contentLength = sizeOfFormat(f);
    t.valid = !t.url.isEmpty();
}

} // namespace

qint64 contentLengthOf(const QString &url)
{
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

Plan planFromFormats(const QJsonArray &adaptiveFormats, int maxHeight,
                      const QString &prefLang,
                      int (*codecRank)(int height, const QString &mimeOrCodecs))
{
    Plan plan;
    Q_UNUSED(codecRank);

    // --- the video variants ---
    //
    // ONE per (height, codec): the dialog offers the codec of each
    // resolution, so the user can pick the AV1 (smaller) or the H264 (the
    // one that opens anywhere).
    for (const QJsonValue &val : adaptiveFormats) {
        const QJsonObject f = val.toObject();
        if (f.value(QStringLiteral("url")).toString().isEmpty())
            continue;  // signed entries (signatureCipher) without decrypting
        const QString mime = f.value(QStringLiteral("mimeType")).toString();
        if (!mime.startsWith(QLatin1String("video/")))
            continue;
        const int h = f.value(QStringLiteral("height")).toInt();
        // maxHeight <= 0 means NO cap: the download is not the playback,
        // and the person asked for a file (the menu offers the resolutions
        // the video really has).
        if (h <= 0 || (maxHeight > 0 && h > maxHeight))
            continue;
        if (f.value(QStringLiteral("initRange")).toObject().isEmpty())
            continue;  // without an init it cannot be a playable fMP4
        const QString codec = videoCodecFamily(mime);
        if (codec.isEmpty())
            continue;
        const qint64 size = sizeOfFormat(f);

        int at = -1;
        for (int i = 0; i < plan.options.size(); ++i) {
            if (plan.options.at(i).height == h
                && plan.options.at(i).codec == codec) {
                at = i;
                break;
            }
        }
        if (at >= 0) {
            // The SMALLEST of the same (height, codec): for a download the
            // size is what matters.
            const qint64 current = plan.options.at(at).contentLength;
            if (size > 0 && current > 0 && size >= current)
                continue;
        }
        VideoOption opt;
        opt.height = h;
        opt.codec = codec;
        opt.contentLength = size;
        opt.codecs = mime;
        opt.format = f;
        if (at >= 0)
            plan.options[at] = opt;
        else
            plan.options.append(opt);
    }
    if (plan.options.isEmpty()) {
        plan.error = nt::tr("error.dlNoFormat");
        return plan;
    }
    std::sort(plan.options.begin(), plan.options.end(),
              [](const VideoOption &a, const VideoOption &b) {
                  if (a.height != b.height)
                      return a.height > b.height;
                  // Inside a height: AV1 first (the smallest one for the
                  // same picture), then VP9, then H264.
                  const int ra = a.codec == QLatin1String("av1")
                                     ? 0
                                     : (a.codec == QLatin1String("vp9") ? 1 : 2);
                  const int rb = b.codec == QLatin1String("av1")
                                     ? 0
                                     : (b.codec == QLatin1String("vp9") ? 1 : 2);
                  if (ra != rb)
                      return ra < rb;
                  return a.contentLength < b.contentLength;
              });

    // The chosen video: the highest one, and among the same height the
    // SMALLEST (the AV1 usually wins: for a download the size is what
    // matters).
    VideoOption *chosen = &plan.options.first();
    for (VideoOption &o : plan.options) {
        if (o.height == chosen->height && o.contentLength < chosen->contentLength
            && chosen->contentLength > 0) {
            chosen = &o;
        }
    }
    fillTrack(plan.video, chosen->format);

    // --- the audio ---
    //
    // The AAC of an mp4 is preferred over the Opus of a WebM, and it is not
    // a whim: the mp4 one carries a sidx (it is fragmented, so it can be
    // fetched in pieces) and the WebM one does not (it is a stream of
    // clusters, and a track without index cannot be split).
    //
    // It is the SAME rule the player uses (preferredAudioFormat): one copy,
    // so the size that the menu shows is the one that gets downloaded.
    const QJsonObject bestAudio = preferredAudioFormat(adaptiveFormats, prefLang);
    if (!bestAudio.isEmpty())
        fillTrack(plan.audio, bestAudio);

    plan.estimatedBytes = plan.video.contentLength + plan.audio.contentLength;
    if (plan.estimatedBytes < 0)
        plan.estimatedBytes = 0;
    plan.valid = true;
    return plan;
}

Plan planWithHeight(const Plan &plan, int height)
{
    if (height <= 0)
        return plan;
    for (const VideoOption &o : plan.options) {
        if (o.height != height)
            continue;
        Plan out = plan;
        fillTrack(out.video, o.format);
        out.estimatedBytes = (out.kind == DownloadKind::Audio)
                                 ? out.audio.contentLength
                                 : (out.video.contentLength + out.audio.contentLength);
        return out;
    }
    return plan;
}

Plan planWithVariant(const Plan &plan, int height, const QString &codec)
{
    if (height <= 0 || codec.isEmpty())
        return plan;
    for (const VideoOption &o : plan.options) {
        if (o.height != height || o.codec != codec)
            continue;
        Plan out = plan;
        fillTrack(out.video, o.format);
        out.estimatedBytes = (out.kind == DownloadKind::Audio)
                                 ? out.audio.contentLength
                                 : (out.video.contentLength + out.audio.contentLength);
        return out;
    }
    return plan;
}

QVector<Range> rangesWithoutIndex(qint64 contentLength, qint64 chunkBytes)
{
    QVector<Range> out;
    if (contentLength <= 0 || chunkBytes <= 0)
        return out;
    qint64 pos = 0;
    while (pos < contentLength) {
        Range r;
        r.start = pos;
        r.end = qMin(contentLength - 1, pos + chunkBytes - 1);
        out.append(r);
        pos = r.end + 1;
    }
    return out;
}

QVector<Range> rangesFromSidx(const QByteArray &box, qint64 rangeStart,
                              double *totalDurationOut)
{
    QVector<Range> out;
    if (totalDurationOut)
        *totalDurationOut = 0.0;
    if (box.size() < 32)
        return out;

    // The sidx is found by its type: the range that was asked for does NOT
    // start at it (YouTube puts a box of its own in front of it), and the
    // box does not fill the range either.
    const int at = box.indexOf("sidx");
    if (at < 4)
        return out;
    const int boxStart = at - 4;

    auto rd32 = [&box](int o) -> quint32 {
        if (o + 4 > box.size())
            return 0;
        return (quint32(quint8(box.at(o))) << 24)
               | (quint32(quint8(box.at(o + 1))) << 16)
               | (quint32(quint8(box.at(o + 2))) << 8)
               | quint32(quint8(box.at(o + 3)));
    };
    const quint32 boxSize = rd32(boxStart);
    if (boxSize < 32)
        return out;

    int p = boxStart + 8;
    const quint32 verFlags = rd32(p);
    p += 4;  // version + flags
    p += 4;  // reference_ID
    const quint32 timescale = rd32(p);
    p += 4;
    qint64 firstOffset = 0;
    if ((verFlags >> 24) == 0) {
        p += 4;  // earliest_presentation_time
        firstOffset = qint64(rd32(p));
        p += 4;
    } else {
        p += 8;
        firstOffset = (qint64(rd32(p)) << 32) | rd32(p + 4);
        p += 8;
    }
    if (timescale == 0)
        return out;
    p += 2;  // reserved
    const int count = int((quint32(quint8(box.at(p))) << 8)
                          | quint32(quint8(box.at(p + 1))));
    p += 2;
    if (p + count * 12 > box.size())
        return out;

    // The first fragment starts where the box ends (plus the offset that the
    // box says). The position of the box inside the range is added, because
    // the range does not start at the box.
    qint64 offset = rangeStart + boxStart + boxSize + firstOffset;
    double total = 0.0;
    for (int i = 0; i < count; ++i) {
        const qint64 size = qint64(rd32(p) & 0x7fffffffu);
        const quint32 dur = rd32(p + 4);
        p += 12;
        if (size <= 0)
            break;  // the "no more fragments" marker
        Range r;
        r.start = offset;
        r.end = offset + size - 1;
        r.duration = double(dur) / double(timescale);
        total += r.duration;
        out.append(r);
        offset += size;
    }
    if (totalDurationOut)
        *totalDurationOut = total;
    return out;
}

} // namespace dl
