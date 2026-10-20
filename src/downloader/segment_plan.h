#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// Plan of a download: WHICH segments to fetch, from WHERE, and how big the
// result will be. It is a PURE function of the master playlist (and the
// preferences), so it can be tested offline with no network at all.
//
// Why a plan and not the download itself: the master playlist of YouTube
// carries a video variant (the segments) and, separately, the audio
// renditions in #EXT-X-MEDIA. The two are different playlists and the
// downloader has to fetch BOTH. This module decides which ones, so the
// network part only has to execute the plan.
//
// The unit of the plan is the SEGMENT, not a byte range. The master is
// already fragmented by YouTube with segments of ~1.3 MB (measured), which is
// well under the ~8 MB per-request budget that YouTube's throttle allows
// (measured: a range of 8 MB runs at 14 MB/s, from 16 MB it drops to
// 0.36 MB/s). So every segment fits in ONE request and no splitting
// machinery is needed. Resume is then a set of segment indexes, not a set of
// byte offsets.

namespace dl {

// One segment of a media playlist.
struct Segment
{
    QString url;       // absolute URL of the segment
    double duration = 0.0;  // #EXTINF, seconds
    qint64 size = 0;   // Content-Length when known, 0 otherwise
};

// The init segment of a media playlist (#EXT-X-MAP). It is the "moov" of an
// fMP4: WITHOUT it the file has no header and nothing can read it (measured:
// ffmpeg answers "trun track id unknown, no tfhd was found"). It is a
// separate URL and it is NOT one of the segments.
struct InitSegment
{
    QString url;
    bool present = false;
};

// The video part of the plan: the chosen variant of the master.
struct VideoPlan
{
    QString playlistUrl;  // the media playlist of the variant
    QVector<Segment> segments;
    int height = 0;       // RESOLUTION of the variant
    QString codecs;       // CODECS attribute of the variant
    qint64 bandwidth = 0; // BANDWIDTH attribute (bits/s)
    double duration = 0.0; // the sum of the EXTINF of the media playlist
    bool valid = false;
};

// The audio part of the plan: the chosen #EXT-X-MEDIA rendition and its
// media playlist. YouTube puts the audio in its OWN playlist (the segment of
// the video variant is video-only), so it is a second download.
struct AudioPlan
{
    QString playlistUrl;  // the media playlist of the audio rendition
    QString language;     // LANGUAGE attribute ("en", "es-419", ...)
    QString name;         // NAME attribute
    bool isOriginal = false;  // the original track, not a dub
    QVector<Segment> segments;
    bool valid = false;
};

// The whole plan of one download.
struct Plan
{
    VideoPlan video;
    AudioPlan audio;
    qint64 estimatedBytes = 0;  // video + audio, from BANDWIDTH x duration
    double videoDuration = 0.0;  // the real duration of the video track
    double durationSeconds = 0.0;
    bool valid = false;
    QString error;  // empty when valid
};

// Parses a MASTER playlist (the multivariant one) and picks:
//  - the video variant: the best by (rank, height), where the rank honours
//    the codec profile the same way the PLAYER does (so the downloaded file
//    is the codec the machine can decode), and never above maxHeight;
//  - the audio rendition: the original track, or the dub in prefLang when
//    there is one (the same rule the player uses, but here the URI of the
//    CHOSEN track is kept, which the player discards because mpv resolves it
//    by itself).
//
// codecRank is the same ranking the player uses, passed in as a function so
// this module does not depend on the Yt class: a higher score wins.
// Returns an empty plan (valid == false) with `error` set when the master
// has nothing usable.
Plan planFromMaster(const QByteArray &masterBody, int maxHeight,
                    const QString &prefLang,
                    int (*codecRank)(int height, const QString &codecs));

// Parses a MEDIA playlist (the segments of one variant or rendition) and
// returns its segments in order, with their #EXTINF durations. A segment
// that follows an #EXT-X-BYTERANGE is still one segment: the downloader
// asks for the whole thing, so the byte range is only informational here.
//
// `initOut` receives the #EXT-X-MAP (the init segment) when the playlist has
// one: an fMP4 variant NEEDS it at the front of the file, and a playlist
// without it cannot be read at all.
QVector<Segment> segmentsFromMediaPlaylist(const QByteArray &mediaBody,
                                            InitSegment *initOut = nullptr);

// Estimated size in bytes of a stream of `bandwidth` bits/s lasting
// `durationSeconds`. The master carries BANDWIDTH, so the size of a download
// is known BEFORE starting, with no extra request: measured, the estimate
// for the 1080p master was 354.6 MB against 343 MB real (1.7 %).
qint64 estimateBytes(qint64 bandwidth, double durationSeconds);

// The total duration of a media playlist: the sum of its #EXTINF. It is
// what gives the size of the download (BANDWIDTH x duration) before it
// starts, with no extra request.
double durationOfMediaPlaylist(const QByteArray &mediaBody);

} // namespace dl
