#pragma once

#include <QString>
#include <QStringList>

// Where the downloads are written and how the folder is opened.
namespace downloads {

// The folder where the downloads are written: the one of the downloader (the
// folder of downloads of the SYSTEM plus the subfolder "NeverTube"), unless
// it was changed in Ajustes.
//
// It is a PURE function of the settings, so the button of "Abrir carpeta"
// can be checked without a screen.
QString folderOf(const QString &configuredFolder);

// The folder of downloads of the SYSTEM, in ANY language and on any desktop:
// the one the rest of the system uses ("Descargas", "Downloads",
// "Téléchargements"...). It is never empty: it falls back to Documents, then
// Home, then the temp folder.
//
// The resolution is `xdg::downloadsFolder()` (src/xdg_dirs.*), the SAME one the
// database uses for its documents folder: it is not plain QStandardPaths because
// Qt 5 reads ONLY $XDG_CONFIG_HOME/user-dirs.dirs and never the XDG_DOWNLOAD_DIR
// variable, so a session with a custom XDG_CONFIG_HOME gets the English
// "Downloads" even though the folder of the system is another one.
QString systemDownloadsFolder();

// The command that opens a path in the file manager of the desktop, with the
// path as its last argument. Empty when there is none (the desktop handles
// it by itself).
//
// It is the FALLBACK of QDesktopServices::openUrl, which returns false in
// the desktops that have no handler registered for "file://" (measured: it
// returned false for a folder AND for a file, so the button of "Abrir
// carpeta" did nothing at all).
QStringList openCommandFor(const QString &path);

// The name of the file of a download, taken from the TITLE of the video and
// not from its id: the person opens the folder and finds "El video que
// buscaba.mkv", not "dQw4w9WgXcQ.mkv".
//
// It is PURE (no disk): the characters that Windows, macOS and Linux do not
// accept in a name are replaced, the dots and spaces of the edges are
// removed (Windows drops them by itself and the comparison of names would
// lie) and a title that is left empty falls back to `fallback`. The length is
// capped so that the whole path stays under the 260 characters of Windows.
QString safeFileName(const QString &title, const QString &fallback = QString());

// The path of a NEW file inside `dir`: `base`+`ext` and, when that name is
// already taken, `base`+" (2)"+`ext`, `base`+" (3)"+`ext`... It is what makes
// it possible to have the video AND the mp3 of the same video in the same
// folder, with the same title: the second one does not overwrite the first.
QString uniquePathIn(const QString &dir, const QString &base,
                     const QString &ext);

} // namespace downloads
