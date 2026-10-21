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

// The command that opens a path in the file manager of the desktop, with the
// path as its last argument. Empty when there is none (the desktop handles
// it by itself).
//
// It is the FALLBACK of QDesktopServices::openUrl, which returns false in
// the desktops that have no handler registered for "file://" (measured: it
// returned false for a folder AND for a file, so the button of "Abrir
// carpeta" did nothing at all).
QStringList openCommandFor(const QString &path);

} // namespace downloads
