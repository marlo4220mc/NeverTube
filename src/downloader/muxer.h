#pragma once

#include <QString>
#include <QStringList>

// The muxing of the download: the two partial files (the video segments and
// the audio segments, both MPEG-TS) become ONE mp4 with a video track and an
// audio track.
//
// Why an external ffmpeg and not the library: the FFmpeg that is vendored in
// the repository has NO muxer. It is built with --disable-everything and only
// demuxers, and `avformat_write_frame` is not even exported, so it can read
// an mp4 but not write one (verified on the DLLs). The binaries of the
// vendored package are the codec libraries only, there is no CLI.
//
// So the ffmpeg is looked for on the system, at runtime. That is also what
// makes the downloader NOT depend on it: the segments can be downloaded and
// kept even with no ffmpeg around, and the mux is a separate step that can
// be retried when one shows up.
//
// The mux itself is a stream copy (-c copy): no re-encoding, no quality loss,
// and it takes 0.05 s for 30 s of video (measured).
namespace dl {

struct MuxResult
{
    bool ok = false;
    QString error;         // empty when ok
    QString ffmpegPath;     // where ffmpeg was found (empty when not)
    qint64 outputBytes = 0;
};

// Where ffmpeg is, or an empty string when it is not installed. It looks in
// the PATH and in the usual places; the environment variable
// NEVERTUBE_FFMPEG overrides it (useful for a build that ships its own).
QString findFfmpeg();

// Muxes the two partial files into `outputPath`.
//
// The arguments are the plain ones: -i video -i audio -c copy, plus
// -movflags +faststart so the moov is at the front of the file and the
// player does not have to read it from the end.
//
// The partials are NOT deleted: if the mux fails, the bytes that were
// downloaded are still there and the mux can be retried.
MuxResult muxToMp4(const QString &videoPath, const QString &audioPath,
                   const QString &outputPath, const QString &ffmpegPath = QString());

} // namespace dl
