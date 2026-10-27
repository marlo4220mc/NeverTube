#pragma once

#include <QStandardPaths>
#include <QString>

// The folders that the SYSTEM gives the user, independent of the language and of
// the desktop: "Descargas"/"Downloads", "Documentos"/"Documents".
//
// It is ONE place because the app has TWO consumers and they have to agree: the
// downloader writes into the downloads folder and the database lives in the
// documents folder. Neither may fall back to the English name when the system
// uses another one.
//
// Why not just QStandardPaths (this is the whole reason the file exists): Qt 5
// reads ONLY $XDG_CONFIG_HOME/user-dirs.dirs (qstandardpaths_unix.cpp) and never
// the XDG_<...>_DIR variable, so a session with a custom XDG_CONFIG_HOME gets the
// literal English "Downloads"/"Documents" even though the folders of the system
// are elsewhere. Inside a Flatpak the first file is the one flatpak GENERATES
// from the real folders of the host, which is why the permission xdg-download /
// xdg-documents exposes exactly the translated one.
namespace xdg {

// PURE: the value of `key` ("XDG_DOWNLOAD_DIR", "XDG_DOCUMENTS_DIR") in the TEXT
// of a user-dirs.dirs, with $HOME/${HOME} replaced by `home`. Empty when the key
// is not there, the line is commented, or the value is not an absolute path.
QString parseUserDir(const QString &contents, const QString &key,
                     const QString &home);

// The folder of `key`, in this order:
//   1. XDG_<...>_DIR when the session exports it (Qt does NOT read it),
//   2. the line of $XDG_CONFIG_HOME/user-dirs.dirs,
//   3. the line of ~/.config/user-dirs.dirs (Qt only looks at the first one),
//   4. `fallback`, which is what answers on Windows and macOS (the Known Folder
//      of the system; there is no user-dirs.dirs there).
// Empty only when Qt has no answer either.
QString userDir(const QString &key, QStandardPaths::StandardLocation fallback);

QString downloadsFolder();
QString documentsFolder();

} // namespace xdg
