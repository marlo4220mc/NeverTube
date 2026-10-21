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
    // The file that was written. It is NOT always `outputPath`: the audio
    // only falls back to an M4A when the ffmpeg has no mp3 encoder (the
    // LGPL builds do not carry libmp3lame), and then the name changes.
    QString outputPath;
};

// Where ffmpeg is, or an empty string when it is not installed. It looks in
// the PATH and in the usual places; the environment variable
// NEVERTUBE_FFMPEG overrides it (useful for a build that ships its own).
// The directory of the app is also looked at (a build that SHIPS its own
// ffmpeg has it right next to the exe).
QString findFfmpeg();

// Whether the ffmpeg has the lavfi INPUT (the trimmed builds that ship
// with the app do NOT have it: they cannot synthesize a track).
bool hasLavfi(const QString &ffmpegPath = QString());

// Whether the ffmpeg has an mp3 encoder. The LGPL builds (the one that
// ships with the app, the ffmpeg-free of Fedora) do NOT carry libmp3lame
// (it is GPL), and then the audio falls back to an M4A.
bool hasMp3Encoder(const QString &ffmpegPath = QString());

// Runs the ffmpeg with `args` and checks that it wrote `outputPath`.
MuxResult runFfmpeg(const QString &ffmpeg, const QStringList &args,
                     const QString &outputPath);

// The arguments of the two commands, as PURE functions (so the selftest can
// check them without running an ffmpeg).
//
// The mux of the video: `-i video -i audio -map 0:v:0 -map 1:a:0 -c copy`.
// The audio only: `-i audio -vn -acodec libmp3lame -q:a 2` (and the
// fall-back that copies the audio into an M4A when there is no mp3
// encoder).
QStringList muxArgs(const QString &videoPath, const QString &audioPath,
                    const QString &outputPath);
QStringList audioArgs(const QString &audioPath, const QString &outputPath);

// The M4A that the fall-back writes when the ffmpeg has no mp3 encoder:
// the same name with the other extension.
QString fallbackAudioPath(const QString &mp3Path);

// Muxes the two partial files into `outputPath`.
//
// The arguments are the plain ones: -i video -i audio -c copy.
//
// The container is decided by the EXTENSION of the output, and it is MKV:
// the audio of the HLS master is AAC in ADTS, and an mp4 wants it in ASC.
// The bitstream filter that does that conversion fails with "Not yet
// implemented in FFmpeg" in the FFmpeg of this system, and the mp4 ends up
// with the audio cut at the first second. Matroska takes the ADTS as it is
// and needs no conversion at all.
//
// The partials are NOT deleted: if the mux fails, the bytes that were
// downloaded are still there and the mux can be retried.
MuxResult muxToMp4(const QString &videoPath, const QString &audioPath,
                   const QString &outputPath, const QString &ffmpegPath = QString());

// Converts the audio track to an mp3. It is the "solo el audio" option of
// the dialog: the segments of the audio rendition are MPEG-TS, and an mp3 is
// what a person can put in a music player.
//
// The conversion is done with ffmpeg (-vn -acodec libmp3lame). The source
// is the partial .ts of the audio, so the conversion happens after the
// segments are downloaded (and it needs the audio to be there).
MuxResult convertToMp3(const QString &audioPath, const QString &outputPath,
                        const QString &ffmpegPath = QString());

} // namespace dl
