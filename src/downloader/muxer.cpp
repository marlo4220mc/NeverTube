#include "muxer.h"

#include "../i18n.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace dl {

QString findFfmpeg()
{
    // An explicit path wins: a build that ships its own ffmpeg, or a user
    // that wants a specific one.
    const QString fromEnv = qEnvironmentVariable("NEVERTUBE_FFMPEG");
    if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv))
        return fromEnv;

    // The PATH.
    const QString found = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!found.isEmpty())
        return found;

    // The directory of the app: a build that SHIPS its own ffmpeg has it
    // right next to the exe, and the PATH of a desktop session does not
    // include the install directory.
    {
        const QString dir = QCoreApplication::applicationDirPath();
        const QString ownExe = QDir(dir).filePath(QStringLiteral("ffmpeg.exe"));
        if (QFileInfo::exists(ownExe))
            return ownExe;
        const QString own = QDir(dir).filePath(QStringLiteral("ffmpeg"));
        if (QFileInfo::exists(own))
            return own;
    }

    // The usual places, for the systems where the PATH of a desktop session
    // does not include them.
    static const char *const kPlaces[] = {
        "/usr/bin/ffmpeg",
        "/usr/local/bin/ffmpeg",
        "/usr/bin/ffmpeg-avconv",
        "/opt/homebrew/bin/ffmpeg",
        "/snap/bin/ffmpeg",
        "C:/Program Files/ffmpeg/bin/ffmpeg.exe",
        "C:/ffmpeg/bin/ffmpeg.exe",
    };
    for (const char *place : kPlaces) {
        const QString path = QString::fromUtf8(place);
        if (QFileInfo::exists(path))
            return path;
    }
    return QString();
}

MuxResult muxToMp4(const QString &videoPath, const QString &audioPath,
                   const QString &outputPath, const QString &ffmpegPath)
{
    MuxResult result;
    const QString ffmpeg = ffmpegPath.isEmpty() ? findFfmpeg() : ffmpegPath;
    result.ffmpegPath = ffmpeg;
    if (ffmpeg.isEmpty()) {
        result.error = nt::tr("error.dlFfmpegMissing");
        return result;
    }
    if (!QFileInfo::exists(videoPath)) {
        result.error = nt::tr("error.dlNoVideoPart");
        return result;
    }
    if (!audioPath.isEmpty() && !QFileInfo::exists(audioPath)) {
        result.error = nt::tr("error.dlNoAudioPart");
        return result;
    }

    return runFfmpeg(ffmpeg, muxArgs(videoPath, audioPath, outputPath),
                     outputPath);
}

MuxResult convertToMp3(const QString &audioPath, const QString &outputPath,
                        const QString &ffmpegPath)
{
    MuxResult result;
    const QString ffmpeg = ffmpegPath.isEmpty() ? findFfmpeg() : ffmpegPath;
    result.ffmpegPath = ffmpeg;
    if (ffmpeg.isEmpty()) {
        result.error = nt::tr("error.dlFfmpegMissing");
        return result;
    }
    if (!QFileInfo::exists(audioPath)) {
        result.error = nt::tr("error.dlNoAudioPart");
        return result;
    }

    MuxResult res = runFfmpeg(ffmpeg, audioArgs(audioPath, outputPath),
                               outputPath);
    if (res.ok)
        return res;

    // The ffmpeg of the system may NOT have an mp3 encoder: the LGPL build
    // that ships with the app (and the ffmpeg-free of Fedora) do not carry
    // libmp3lame (it is GPL and they are LGPL). The audio is then copied
    // into an M4A, which is the SAME audio without re-encoding (the source
    // of the download is already an AAC in an m4a).
    if (!hasMp3Encoder(ffmpeg)) {
        const QString m4a = fallbackAudioPath(outputPath);
        // The failed attempt can leave a file BEHIND (ffmpeg writes it
        // before it fails): it is removed, or there would be a corrupt
        // "x.mp3" next to the good "x.m4a".
        QFile::remove(outputPath);
        QStringList args2;
        args2 << QStringLiteral("-hide_banner")
              << QStringLiteral("-loglevel")
              << QStringLiteral("error")
              << QStringLiteral("-y")
              << QStringLiteral("-i") << audioPath
              << QStringLiteral("-vn")
              << QStringLiteral("-c:a") << QStringLiteral("copy")
              << m4a;
        MuxResult res2 = runFfmpeg(ffmpeg, args2, m4a);
        if (res2.ok) {
            // The name of the file changed: the caller has to know it.
            res2.outputPath = m4a;
            return res2;
        }
    }
    return res;
}

QStringList muxArgs(const QString &videoPath, const QString &audioPath,
                    const QString &outputPath)
{
    QStringList args;
    args << QStringLiteral("-hide_banner")
         << QStringLiteral("-loglevel")
         << QStringLiteral("error")
         << QStringLiteral("-y");
    if (!audioPath.isEmpty()) {
        args << QStringLiteral("-i") << videoPath
             << QStringLiteral("-i") << audioPath
             << QStringLiteral("-map") << QStringLiteral("0:v:0")
             << QStringLiteral("-map") << QStringLiteral("1:a:0");
    } else {
        args << QStringLiteral("-i") << videoPath
             << QStringLiteral("-map") << QStringLiteral("0:v:0");
    }
    // Stream copy: no re-encoding, no quality loss.
    args << QStringLiteral("-c") << QStringLiteral("copy");
    if (!audioPath.isEmpty())
        args << QStringLiteral("-c:a") << QStringLiteral("copy");
    args << outputPath;
    return args;
}

QStringList audioArgs(const QString &audioPath, const QString &outputPath)
{
    QStringList args;
    args << QStringLiteral("-hide_banner")
         << QStringLiteral("-loglevel")
         << QStringLiteral("error")
         << QStringLiteral("-y")
         << QStringLiteral("-i") << audioPath
         << QStringLiteral("-vn")                    // only the audio
         << QStringLiteral("-acodec") << QStringLiteral("libmp3lame")
         << QStringLiteral("-q:a") << QStringLiteral("2")
         << outputPath;
    return args;
}

QString fallbackAudioPath(const QString &mp3Path)
{
    return QFileInfo(mp3Path).absolutePath() + QLatin1Char('/')
           + QFileInfo(mp3Path).completeBaseName() + QStringLiteral(".m4a");
}

bool hasLavfi(const QString &ffmpegPath)
{
    const QString path = ffmpegPath.isEmpty() ? findFfmpeg() : ffmpegPath;
    if (path.isEmpty())
        return false;
    QProcess process;
    process.start(path, QStringList() << QStringLiteral("-hide_banner")
                                      << QStringLiteral("-demuxers"));
    if (!process.waitForStarted(10000))
        return false;
    if (!process.waitForFinished(20000)) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    const QString out = QString::fromUtf8(process.readAllStandardOutput());
    return out.contains(QStringLiteral("lavfi"));
}

bool hasMp3Encoder(const QString &ffmpegPath)
{
    // `ffmpeg -encoders` lists the encoders that the build has: the mp3 one
    // is "libmp3lame" (or "mp3" in the builds that have the native one).
    const QString path = ffmpegPath.isEmpty() ? findFfmpeg() : ffmpegPath;
    if (path.isEmpty())
        return false;
    QProcess process;
    process.start(path, QStringList() << QStringLiteral("-hide_banner")
                                      << QStringLiteral("-encoders"));
    if (!process.waitForStarted(10000))
        return false;
    if (!process.waitForFinished(20000)) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    const QString out = QString::fromUtf8(process.readAllStandardOutput());
    return out.contains(QStringLiteral("libmp3lame"))
           || out.contains(QStringLiteral("mp3 "));
}

MuxResult runFfmpeg(const QString &ffmpeg, const QStringList &args,
                     const QString &outputPath)
{
    MuxResult result;
    result.ffmpegPath = ffmpeg;
    QProcess process;
    process.start(ffmpeg, args);
    if (!process.waitForStarted(10000)) {
        result.error = nt::tr("error.dlFfmpegRun");
        return result;
    }
    if (!process.waitForFinished(10 * 60 * 1000)) {
        process.kill();
        process.waitForFinished(2000);
        result.error = nt::tr("error.dlFfmpegTimeout");
        return result;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString out = QString::fromUtf8(process.readAllStandardError()).trimmed();
        result.error = out.isEmpty() ? nt::tr("error.dlFfmpegFailed") : out;
        return result;
    }
    if (!QFileInfo::exists(outputPath)) {
        result.error = nt::tr("error.dlFfmpegNoFile");
        return result;
    }
    result.outputBytes = QFileInfo(outputPath).size();
    result.outputPath = outputPath;
    result.ok = true;
    return result;
}

} // namespace dl
