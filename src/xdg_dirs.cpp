#include "xdg_dirs.h"

#include <QDir>
#include <QFile>
#include <QStringList>

namespace xdg {

namespace {

// xdg-user-dirs writes the values with $HOME
// ("XDG_DOWNLOAD_DIR=\"$HOME/Descargas\"") and, sometimes, without quotes. Only
// $HOME/${HOME} are expanded: they are the only variable that the format uses.
QString expandHome(const QString &value, const QString &home)
{
    if (value.isEmpty() || home.isEmpty())
        return value;
    QString out = value;
    out.replace(QStringLiteral("${HOME}"), home);
    out.replace(QStringLiteral("$HOME"), home);
    return out;
}

} // namespace

QString parseUserDir(const QString &contents, const QString &key,
                     const QString &home)
{
    const QStringList lines = contents.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0 || line.left(eq).trimmed() != key)
            continue;
        QString value = line.mid(eq + 1).trimmed();
        if (value.size() >= 2 && value.startsWith(QLatin1Char('"'))
            && value.endsWith(QLatin1Char('"')))
            value = value.mid(1, value.size() - 2).trimmed();
        // A relative value is not a folder of the file system, so it is not an
        // answer: the caller falls through to the next source.
        const QString absolute = expandHome(value, home);
        return QDir::isAbsolutePath(absolute) ? absolute : QString();
    }
    return QString();
}

QString userDir(const QString &key, QStandardPaths::StandardLocation fallback)
{
    const QString home = QDir::homePath();

    // 1. The variable, for the sessions that export it. Qt does NOT read it.
    const QString fromEnv = expandHome(
        QFile::decodeName(qgetenv(key.toLatin1().constData())), home);
    if (!fromEnv.isEmpty() && QDir::isAbsolutePath(fromEnv))
        return fromEnv;

    // 2 and 3. The mapping of the session (the file that xdg-user-dirs writes).
    // BOTH files are read: Qt 5 reads only $XDG_CONFIG_HOME (default
    // ~/.config) and never the variable, so a session with a custom
    // XDG_CONFIG_HOME got the English "Downloads" while the folder of the
    // system was another one.
    QStringList files;
    const QString cfg = QFile::decodeName(qgetenv("XDG_CONFIG_HOME"));
    if (!cfg.isEmpty())
        files << cfg + QLatin1String("/user-dirs.dirs");
    files << home + QLatin1String("/.config/user-dirs.dirs");
    for (const QString &path : files) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString value =
            parseUserDir(QString::fromUtf8(f.readAll()), key, home);
        if (!value.isEmpty())
            return value;
    }

    // 4. Qt. On Windows and macOS it is the only path that counts: there is no
    // user-dirs.dirs and the answer is the Known Folder of the system.
    return QStandardPaths::writableLocation(fallback);
}

QString downloadsFolder()
{
    return userDir(QStringLiteral("XDG_DOWNLOAD_DIR"),
                   QStandardPaths::DownloadLocation);
}

QString documentsFolder()
{
    return userDir(QStringLiteral("XDG_DOCUMENTS_DIR"),
                   QStandardPaths::DocumentsLocation);
}

} // namespace xdg
