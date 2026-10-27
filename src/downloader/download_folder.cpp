#include "download_folder.h"

#include "../xdg_dirs.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace downloads {

QString systemDownloadsFolder()
{
    // The folder that the SYSTEM gives the user, in any language and on any
    // desktop (see ../xdg_dirs.h): inside a Flatpak the session mapping is the
    // one flatpak generates from the host, so it is already translated.
    QString base = xdg::downloadsFolder();
    // The last resorts, for a system without a downloads folder at all.
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (base.isEmpty())
        base = QDir::tempPath();
    return base;
}

QString folderOf(const QString &configuredFolder)
{
    // The folder that was set in Ajustes wins.
    if (!configuredFolder.trimmed().isEmpty())
        return configuredFolder.trimmed();
    // Otherwise the folder of downloads of the SYSTEM (already translated:
    // "Descargas", "Downloads") plus the subfolder "NeverTube", so the files
    // are not mixed with the ones of the user.
    return systemDownloadsFolder() + QLatin1String("/NeverTube");
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

    // And the cap of BYTES, which is a DIFFERENT limit from the one above:
    // Windows counts characters (UTF-16 units) but Linux and macOS count BYTES
    // and reject a name over 255 (ENAMETOOLONG). A title in a non-Latin script
    // or with emoji takes 3 or 4 bytes per character, so the 120 characters of
    // above can be 400+. 200 leaves room for " (999)" and the extension. It is
    // cut by CODE POINTS: chopping half of a surrogate pair would leave a
    // broken character in the name.
    static const int kMaxNameBytes = 200;
    while (out.toUtf8().size() > kMaxNameBytes && !out.isEmpty())
        out.chop(out.at(out.size() - 1).isLowSurrogate() ? 2 : 1);
    out = out.trimmed();
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' ')))
        out.chop(1);

    // Windows reserves the names of its devices: CON, PRN, AUX, NUL, COM1-COM9
    // and LPT1-LPT9, and it also refuses them WITH an extension ("CON.mkv"
    // cannot be created). The check is on the STEM, so "CONCIERTO" is fine, and
    // the underscore goes INSIDE the stem and not at the end of the whole name:
    // appending it to "CON.artista" would leave "CON.artista_", whose stem is
    // still "CON" and the file would still be uncreatable. One underscore makes
    // the name valid on the three systems and keeps it readable.
    const int dot = out.indexOf(QLatin1Char('.'));
    const QString stem = (dot < 0 ? out : out.left(dot)).toUpper();
    static const char *const kReserved[] = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4",
        "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
        "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    for (const char *name : kReserved) {
        if (stem == QLatin1String(name)) {
            out.insert(dot < 0 ? out.size() : dot, QLatin1Char('_'));
            break;
        }
    }

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
