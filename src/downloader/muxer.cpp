#include "muxer.h"

#include "../i18n.h"

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
    result.ok = true;
    return result;
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
    result.ok = true;
    return result;
}

} // namespace dl
