#include "muxer.h"

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
        result.error = QStringLiteral("ffmpeg no encontrado");
        return result;
    }
    if (!QFileInfo::exists(videoPath)) {
        result.error = QStringLiteral("falta el video parcial");
        return result;
    }
    if (!audioPath.isEmpty() && !QFileInfo::exists(audioPath)) {
        result.error = QStringLiteral("falta el audio parcial");
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
    // The moov at the front: a player does not have to read it from the end.
    args << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    args << outputPath;

    QProcess process;
    process.start(ffmpeg, args);
    if (!process.waitForStarted(10000)) {
        result.error = QStringLiteral("no se pudo ejecutar ffmpeg");
        return result;
    }
    if (!process.waitForFinished(10 * 60 * 1000)) {
        process.kill();
        process.waitForFinished(2000);
        result.error = QStringLiteral("ffmpeg tardo demasiado");
        return result;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString out = QString::fromUtf8(process.readAllStandardError()).trimmed();
        result.error = out.isEmpty() ? QStringLiteral("ffmpeg fallo") : out;
        return result;
    }
    if (!QFileInfo::exists(outputPath)) {
        result.error = QStringLiteral("ffmpeg no creo el archivo");
        return result;
    }
    result.outputBytes = QFileInfo(outputPath).size();
    result.ok = true;
    return result;
}

} // namespace dl
