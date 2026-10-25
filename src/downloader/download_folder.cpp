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

QString safeFileName(const QString &title, const QString &fallback)
{
    // The characters that are not allowed (Windows is the strictest one) and
    // the control ones. The `:` also covers the drive of a path, and the `/`
    // and `\` would create a folder instead of a name.
    static const QString kForbidden =
        QStringLiteral("/\\:*?\"<>|");
    QString out;
    out.reserve(title.size());
    for (const QChar c : title) {
        if (c.unicode() < 0x20 || c == QChar(0x7f))
            out.append(QLatin1Char(' '));
        else if (kForbidden.contains(c))
            out.append(QLatin1Char(' '));
        else
            out.append(c);
    }
    // The spaces are collapsed: a title with "a   b" does not need three.
    out = out.simplified();
    // Windows does not allow a name that ends with a dot or a space (it
    // trims them by itself), so they are removed to keep the path we compute
    // and the one on the disk identical.
    while (!out.isEmpty()
           && (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))))
        out.chop(1);
    // The cap: 120 characters leaves room for the folder, the extension and
    // the " (12)" of a repeated name inside the 260 of the Windows limit.
    if (out.size() > 120)
        out = out.left(120).trimmed();
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' ')))
        out.chop(1);
    if (out.isEmpty())
        out = fallback.trimmed().isEmpty() ? QStringLiteral("video")
                                           : fallback.trimmed();
    return out;
}

QString uniquePathIn(const QString &dir, const QString &base,
                     const QString &ext)
{
    const QString folder = dir.isEmpty() ? QDir::currentPath() : dir;
    const QString name = base.trimmed().isEmpty() ? QStringLiteral("video")
                                                  : base;
    QString path = folder + QLatin1Char('/') + name + ext;
    // 999 is not a limit of the file system: it is a bound so that a loop
    // that cannot find a free name terminates instead of hanging the thread
    // that finalizes the download.
    for (int n = 2; QFileInfo::exists(path) && n < 1000; ++n) {
        path = folder + QLatin1Char('/') + name + QStringLiteral(" (")
               + QString::number(n) + QLatin1Char(')') + ext;
    }
    return path;
}

} // namespace downloads
