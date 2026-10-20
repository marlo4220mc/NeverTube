#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

// Plan of a download: WHICH bytes to fetch, from where, and how big the
// result will be. It is a PURE function of the formats that the player
// response gives (the adaptive ones), so it can be tested offline with no
// network at all.
//
// WHY THE ADAPTIVE AND NOT THE HLS MASTER: the throttle of YouTube is per
// request AND per size. Measured: a request of 2 MB runs at 14 MB/s, and the
// SAME resource asked for whole (3.87 MB) runs at 0.011 MB/s, a thousand
// times slower. The master hides that because its segments are already
// small (1.3 MB), but the audio of the master is AAC in LATM, that the
// FFmpeg of this system cannot mux ("Error applying bitstream filters to an
// output packet for stream #1: Not yet implemented in FFmpeg"). The adaptive
// gives both tracks as clean mp4/m4a, and it has the index (the sidx) that
// says where every fragment starts, so it can be fetched in pieces.

namespace dl {

// One fragment of a resource: a range of bytes plus its duration.
struct Range
{
    qint64 start = 0;
    qint64 end = 0;   // inclusive, like the Range header
    double duration = 0.0;
    qint64 size() const { return end - start + 1; }
};

// One of the variants that can be downloaded: a height with its real size.
// It is what the dialog of the download offers.
struct VideoOption
{
    int height = 0;
    qint64 contentLength = 0;  // the real size of the track
    QString codecs;
    QJsonObject format;  // the format it comes from
};

// One TRACK of the download (the video or the audio).
struct Track
{
    QString url;         // the signed URL of the resource
    QJsonObject format;  // the format it comes from
    qint64 initStart = 0;  // the init segment (the fMP4 header)
    qint64 initEnd = 0;
    qint64 indexStart = 0;  // the sidx (the index of the fragments)
    qint64 indexEnd = 0;
    qint64 contentLength = 0;  // the real size of the resource
    bool valid = false;
};

// What is being downloaded: the video with its audio, or only the audio
// converted to mp3.
enum class DownloadKind
{
    Video,  // the file with both tracks
    Audio,  // only the audio, as an mp3
};

// The whole plan of one download.
struct Plan
{
    Track video;
    Track audio;

    // Every variant of the video, from the highest to the lowest: the dialog
    // offers them (with their size) and the user picks one.
    QVector<VideoOption> options;

    DownloadKind kind = DownloadKind::Video;
    double durationSeconds = 0.0;
    // The REAL size of the download: the contentLength of the video plus the
    // the one of the audio. It is known BEFORE it starts and it does not
    // move.
    qint64 estimatedBytes = 0;
    bool valid = false;
    QString error;  // empty when valid
};

// Builds the plan from the adaptive formats of the player response.
//
// The video variants are offered by height, and the SMALLEST one of each
// height wins: for a download the size is what matters (the VP9 of the
// 1080p is 31 MB against the 80 of the h264, for the same picture).
//
// The audio is the ORIGINAL track, or the one in the preferred language (the
// same rule the player uses).
Plan planFromFormats(const QJsonArray &adaptiveFormats, int maxHeight,
                      const QString &prefLang,
                      int (*codecRank)(int height, const QString &mimeOrCodecs));

// A copy of the plan with another variant of the video (the one the user
// picked in the dialog). A height that is not in the plan leaves it
// untouched.
Plan planWithHeight(const Plan &plan, int height);

// The REAL size of a resource, from its URL: YouTube puts it there as
// "clen%3D30846580" (percent-encoded). It is the size of the track, not an
// estimate.
qint64 contentLengthOf(const QString &url);

// The fragments of a resource, from its sidx box (the index). `rangeStart` is
// the position of the range that was asked for, in the file.
//
// The sidx is a box of ISO/IEC 14496-12: a list of subsegments with their
// exact size and duration. It is the same index the synthetic AV1 HLS of the
// player uses (Yt::av1ParseSidx), and it is what gives the DOWNLOAD its
// fragments: without it the whole resource would be ONE request, and YouTube
// throttles that (measured: 0.011 MB/s for 3.87 MB against 14 MB/s for
// 2 MB).
QVector<Range> rangesFromSidx(const QByteArray &box, qint64 rangeStart,
                              double *totalDurationOut);

// The fragments of a track that has NO index (a WebM/Opus one: its clusters
// are not indexed). The resource is cut in pieces of `chunkBytes` and the
// pieces are put back together in order.
//
// It is the same idea as the synthetic AV1 HLS of the player (fixed chunks
// of ~2 MB), and it is needed because YouTube throttles a request by its
// size: the same track asked for whole runs at 0.011 MB/s and a 2 MB range
// of it runs at 14 MB/s (measured).
QVector<Range> rangesWithoutIndex(qint64 contentLength, qint64 chunkBytes);

} // namespace dl
