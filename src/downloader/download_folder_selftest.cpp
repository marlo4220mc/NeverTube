#include "download_folder_selftest.h"

#include "download_folder.h"
#include "../xdg_dirs.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>

#include <cstdio>

bool downloadFolderSelfTest()
{
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        std::printf("  DOWNLOAD-FOLDER %s: %s\n", cond ? "OK" : "FAIL",
                     what);
        if (!cond)
            ok = false;
    };

    // --- the folder of the downloads ---
    check(!downloads::folderOf(QString()).isEmpty(),
          "sin carpeta configurada hay una carpeta por defecto");
    check(downloads::folderOf(QStringLiteral("/mi/carpeta"))
              == QLatin1String("/mi/carpeta"),
          "la carpeta configurada gana");
    check(downloads::folderOf(QStringLiteral("  /mi/carpeta  "))
              == QLatin1String("/mi/carpeta"),
          "la carpeta configurada se limpia de espacios");
    check(downloads::folderOf(QString()).endsWith(QLatin1String("NeverTube")),
          "la carpeta por defecto es la subcarpeta NeverTube");

    // --- the folder of the system, whatever the language ---
    // The default folder hangs from the folder of downloads of the SYSTEM and
    // not from a hardcoded "Downloads": in a Spanish session it is
    // "~/Descargas" and the two things have to agree.
    check(!downloads::systemDownloadsFolder().isEmpty(),
          "la carpeta de descargas del sistema nunca esta vacia");
    check(QDir::isAbsolutePath(downloads::systemDownloadsFolder()),
          "la carpeta de descargas del sistema es un path absoluto");
    check(downloads::folderOf(QString())
              == downloads::systemDownloadsFolder()
                     + QLatin1String("/NeverTube"),
          "la carpeta por defecto cuelga de la del sistema (no de ~/Downloads)");
    // The same resolution gives the documents folder, which is where the SQLite
    // database lives (src/db/app_database.cpp).
    check(!xdg::documentsFolder().isEmpty(),
          "la carpeta de documentos del sistema nunca esta vacia");

    // The format of xdg-user-dirs, line by line.
    const QString kDownload = QStringLiteral("XDG_DOWNLOAD_DIR");
    const QString kDocuments = QStringLiteral("XDG_DOCUMENTS_DIR");
    check(xdg::parseUserDir(QStringLiteral("XDG_DOWNLOAD_DIR=\"$HOME/Descargas\"\n"),
                            kDownload, QStringLiteral("/home/u"))
              == QLatin1String("/home/u/Descargas"),
          "$HOME se expande (el formato real de xdg-user-dirs)");
    check(xdg::parseUserDir(QStringLiteral("XDG_DOWNLOAD_DIR=\"${HOME}/Bajadas\"\n"),
                            kDownload, QStringLiteral("/h"))
              == QLatin1String("/h/Bajadas"),
          "${HOME} tambien se expande");
    check(xdg::parseUserDir(
              QStringLiteral("XDG_DOCUMENTS_DIR=\"$HOME/Documentos\"\n"
                             "XDG_DOWNLOAD_DIR=\"/mnt/datos/bajadas\"\n"),
              kDownload, QStringLiteral("/h")) == QLatin1String("/mnt/datos/bajadas"),
          "la clave es la de DESCARGAS y el valor sin comillas tambien vale");
    check(xdg::parseUserDir(
              QStringLiteral("XDG_DOCUMENTS_DIR=\"$HOME/Documentos\"\n"
                             "XDG_DOWNLOAD_DIR=\"/mnt/datos/bajadas\"\n"),
              kDocuments, QStringLiteral("/h"))
              == QLatin1String("/h/Documentos"),
          "la MISMA funcion da DOCUMENTOS (la carpeta de la base de datos)");
    check(xdg::parseUserDir(QStringLiteral("# XDG_DOWNLOAD_DIR=\"$HOME/No\"\n"),
                            kDownload, QStringLiteral("/h"))
              .isEmpty(),
          "una linea comentada no cuenta");
    check(xdg::parseUserDir(
              QStringLiteral("XDG_DOWNLOAD_DIR=Bajadas\n"), kDownload,
              QStringLiteral("/h")).isEmpty(),
          "un valor relativo no es una carpeta del sistema");
    check(xdg::parseUserDir(QString(), kDownload, QStringLiteral("/h")).isEmpty(),
          "sin archivo no hay valor");

    // --- the command that opens the path ---
    check(downloads::openCommandFor(QString()).isEmpty(),
          "un path vacio no tiene comando");
    const QStringList cmd =
        downloads::openCommandFor(QStringLiteral("/tmp"));
    // There may be NO file manager at all (a Windows runner has none): the
    // command is optional, but when there is one it has to be right. The
    // check that it exists failed in the CI (a runner without a file
    // manager).
    if (!cmd.isEmpty()) {
        check(cmd.last() == QLatin1String("/tmp"),
              "el path es el ULTIMO argumento (el que se abre)");
        // The binary is either a full path or a name that is in the PATH.
        check(QFileInfo(cmd.first()).isAbsolute()
                  || !QStandardPaths::findExecutable(cmd.first()).isEmpty(),
              "el ejecutable existe");
    } else {
        check(true, "sin explorador de archivos no hay comando (no es un fallo)");
    }

    // --- the name of the file: valid on every system, without killing the
    // language (the title of a video is written by a person, in any script) ---
    check(downloads::safeFileName(QStringLiteral("CON")) != QLatin1String("CON"),
          "un nombre de dispositivo de Windows no pasa tal cual");
    check(downloads::safeFileName(QStringLiteral("con")) == QLatin1String("con_"),
          "tambien en minusculas");
    // The reserved names are refused WITH extension too, and the rule looks at
    // the part before the FIRST dot: the underscore has to go inside the stem,
    // not at the end of the whole name (that would leave the stem as "CON").
    check(downloads::safeFileName(QStringLiteral("CON.artista"))
              == QLatin1String("CON_.artista"),
          "un nombre de dispositivo CON extension se arregla en el stem");
    check(downloads::safeFileName(QStringLiteral("lpt9.txt"))
              == QLatin1String("lpt9_.txt"),
          "LPT9 con extension, en minusculas");
    check(downloads::safeFileName(QStringLiteral("COM10"))
              == QLatin1String("COM10"),
          "COM10 no es un nombre reservado (solo COM1-COM9)");
    check(downloads::safeFileName(QStringLiteral("CONCIERTO en vivo"))
              == QLatin1String("CONCIERTO en vivo"),
          "un titulo que solo EMPIEZA por CON no se toca");
    check(downloads::safeFileName(QStringLiteral("Canción de ñandú"))
              == QStringLiteral("Canción de ñandú"),
          "los acentos y la ñ se conservan (no se reduce a ASCII)");
    check(downloads::safeFileName(QStringLiteral("Video: prueba / 2"))
              == QLatin1String("Video prueba 2"),
          "los caracteres prohibidos se vuelven espacios");
    check(downloads::safeFileName(QStringLiteral("Final."))
              == QLatin1String("Final"),
          "un punto final no queda en el nombre");
    check(downloads::safeFileName(QString(120, QLatin1Char('a')))
              == QString(120, QLatin1Char('a')),
          "120 caracteres ASCII caben enteros");
    {
        // 120 characters of 3 bytes each: 360 bytes, over the 255 of NAME_MAX.
        const QString cjk(120, QChar(0x4E2D));
        const QString short3 = downloads::safeFileName(cjk);
        check(short3.toUtf8().size() <= 200,
              "un nombre no pasa de 200 bytes (NAME_MAX de Linux)");
        check(!short3.isEmpty(), "y no queda vacio al recortar");
    }
    check(downloads::safeFileName(QString()) == QLatin1String("video"),
          "un titulo vacio cae al nombre de respaldo");

    std::printf("DOWNLOAD-FOLDER SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}
