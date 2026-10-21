#include "download_folder.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace downloads {

QString folderOf(const QString &configuredFolder)
{
    // The folder that was set in Ajustes wins.
    if (!configuredFolder.trimmed().isEmpty())
        return configuredFolder.trimmed();
    // Otherwise the default one: the folder of downloads of the SYSTEM
    // (which comes already translated: "Descargas", "Downloads") plus the
    // subfolder "NeverTube", so the files are not mixed with the ones of the
    // user.
    QString base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (base.isEmpty())
        base = QDir::tempPath();
    return base + QLatin1String("/NeverTube");
}

QStringList openCommandFor(const QString &path)
{
    if (path.trimmed().isEmpty())
        return QStringList();

    // "gio open" is the mechanism of the freedesktop: it asks the desktop
    // for the handler of the path (the file manager for a folder). It is
    // tried FIRST because it is the one that respects the preferences of
    // the session.
    if (!QStandardPaths::findExecutable(QStringLiteral("gio")).isEmpty())
        return QStringList{QStringLiteral("gio"), QStringLiteral("open"), path};
    // And then the file managers, one by one.
    static const char *const kKnown[] = {
        "nautilus", "dolphin", "thunar", "nemo", "org.gnome.Nautilus",
    };
    for (const char *k : kKnown) {
        const QString bin = QStandardPaths::findExecutable(QString::fromLatin1(k));
        if (!bin.isEmpty())
            return QStringList{bin, path};
    }
    // Nothing: the desktop will have to handle the URL by itself.
    return QStringList();
}

} // namespace downloads
