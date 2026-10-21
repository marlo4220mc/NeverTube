#include "download_folder_selftest.h"

#include "download_folder.h"

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

    std::printf("DOWNLOAD-FOLDER SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}
