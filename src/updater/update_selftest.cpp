#include "update_selftest.h"

#include "update_check.h"

#include "../i18n.h"

#include <QDir>
#include <QFile>
#include <QStringList>

#include <cstdio>

bool updateSelfTest()
{
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        std::printf("  UPDATE %s: %s\n", cond ? "OK" : "FAIL", what);
        if (!cond)
            ok = false;
    };

    // --- the version of the binary ---
    check(!nt::appVersion().isEmpty(), "el binario sabe su version");
    check(nt::appVersion() != QLatin1String("0.0.0"),
          "la version llega del build (CMake), no de un literal suelto");
    check(nt::compareVersions(nt::appVersion(), nt::appVersion()) == 0,
          "la version del binario se compara consigo misma como igual");

    // --- the order of two versions ---
    struct Case
    {
        const char *a;
        const char *b;
        int want;
    };
    const Case cases[] = {
        {"1.0.0", "1.0.0", 0},
        {"1.0.0", "1.0.1", -1},
        {"1.0.1", "1.0.0", 1},
        {"1.9.0", "1.10.0", -1},
        {"10.0.0", "9.9.9", 1},
        {"v1.0.0", "1.0.0", 0},
        {"1.0", "1.0.0", 0},
        // A pre-release is OLDER than its own core, in the two directions: the
        // updater never offers a beta as if it were the stable release.
        {"1.0.0-alpha.1", "1.0.0", -1},
        {"1.0.0", "1.0.0-alpha.1", 1},
        {"1.0.0-alpha.1", "1.0.0-alpha.2", -1},
        {"1.0.0-alpha.2", "1.0.0-alpha.1", 1},
        {"1.0.0-alpha.2", "1.0.0-beta.1", -1},
        {"1.0.0-beta.1", "1.0.0-rc.1", -1},
        {"1.0.0-rc.1", "1.0.0-rc.2", -1},
        {"1.0.0-alpha", "1.0.0-alpha.1", -1},
        {"1.0.0-2", "1.0.0-alpha", -1}, // numeric ids sort first
        {"1.0.1-alpha.1", "1.0.0", 1},  // the core still decides
        {"2.0.0-alpha.1", "1.9.9", 1},
    };
    for (const Case &c : cases) {
        const int got = nt::compareVersions(QString::fromLatin1(c.a),
                                            QString::fromLatin1(c.b));
        const bool okCase = got == c.want;
        if (!okCase)
            std::printf("  UPDATE FAIL: %s vs %s dio %d y se esperaba %d\n",
                        c.a, c.b, got, c.want);
        check(okCase, "el orden de dos versiones es el de SemVer");
        // The comparison has to be a total order: the other direction says
        // exactly the opposite.
        check(nt::compareVersions(QString::fromLatin1(c.b),
                                  QString::fromLatin1(c.a))
                  == -c.want,
              "y el orden contrario es el opuesto");
    }

    check(nt::isNewerVersion(QStringLiteral("1.0.1"),
                             QStringLiteral("1.0.0")),
          "una version mayor es una actualizacion");
    check(!nt::isNewerVersion(QStringLiteral("1.0.0"),
                              QStringLiteral("1.0.0")),
          "la misma version no es una actualizacion");
    check(!nt::isNewerVersion(QStringLiteral("0.9.9"),
                              QStringLiteral("1.0.0")),
          "una version vieja NUNCA es una actualizacion");
    check(!nt::isNewerVersion(QStringLiteral("1.0.0-beta.1"),
                              QStringLiteral("1.0.0")),
          "un pre-lanzamiento no actualiza al estable");

    // --- the tag of the release and the name of the installer ---
    check(nt::versionFromTag(QStringLiteral("v1.0.0"))
              == QLatin1String("1.0.0"),
          "el tag pierde la v");
    check(nt::versionFromTag(QStringLiteral("v1.0.0-alpha.1"))
              == QLatin1String("1.0.0-alpha.1"),
          "el tag de un pre-lanzamiento conserva el sufijo");
    check(nt::versionFromTag(QStringLiteral("1.0.0"))
              == QLatin1String("1.0.0"),
          "un tag sin v se queda igual");

    check(nt::installerAssetNameFor(QStringLiteral("win7-x86"),
                                    QStringLiteral("1.0.0"))
              == QLatin1String("NeverTube-1.0.0-Windows-7-8-x86-Setup.exe"),
          "el instalador de Win7/8 x86");
    check(nt::installerAssetNameFor(QStringLiteral("win10-x86"),
                                    QStringLiteral("1.0.0-alpha.1"))
              == QLatin1String(
                     "NeverTube-1.0.0-alpha.1-Windows-10-11-x86-Setup.exe"),
          "el instalador de Win10 x86, con sufijo de pre-lanzamiento");
    check(nt::installerAssetNameFor(QStringLiteral("x64"),
                                    QStringLiteral("1.0.0"))
              == QLatin1String("NeverTube-1.0.0-Windows-10-11-x64-Setup.exe"),
          "el instalador de x64");
    check(nt::installerAssetNameFor(QStringLiteral("arm64"),
                                    QStringLiteral("1.0.0"))
              .isEmpty(),
          "una plataforma que no se publica no tiene nombre");
    check(nt::installerAssetNameFor(QString(), QStringLiteral("1.0.0"))
              .isEmpty(),
          "sin cocina (Linux) no hay nombre de instalador");
    check(nt::installerAssetNameFor(QStringLiteral("x64"), QString())
              .isEmpty(),
          "sin version no hay nombre de instalador");
    check(nt::legacyInstallerAssetNameFor(QStringLiteral("x64"),
                                          QStringLiteral("1.0.0"))
              == QLatin1String("nevertube-1.0.0-setup-x64.exe"),
          "el nombre antiguo se sigue pudiendo calcular");

    {
        // Whatever this build is, the flavor has to be one of the three that
        // the release publishes (or empty on Linux): a typo here would ask
        // for an asset that does not exist and the update would never work.
        const QString flavor = nt::installerFlavorForBuild();
        const bool known = flavor.isEmpty()
            || flavor == QLatin1String("x64")
            || flavor == QLatin1String("win10-x86")
            || flavor == QLatin1String("win7-x86");
        check(known, "la cocina de este build es una de las publicadas");
        if (flavor.isEmpty()) {
            check(nt::installerAssetName(QStringLiteral("1.0.0")).isEmpty(),
                  "en Linux no hay instalador que buscar");
        } else {
            const QString name =
                nt::installerAssetName(QStringLiteral("1.0.0"));
            check(name.startsWith(QStringLiteral("NeverTube-1.0.0-"))
                      && name.endsWith(QStringLiteral("-Setup.exe")),
                  "el instalador de este build lleva su cocina en el nombre");
        }
    }

    // --- selection: the release hands out the installer of THIS machine ---
    {
        auto asset = [](const char *name) {
            nt::UpdateAsset a;
            a.name = QString::fromLatin1(name);
            a.url = QUrl(QStringLiteral(
                "https://github.com/marlo4220mc/NeverTube/releases/download/"
                "v9.9.9/x"));
            a.size = 1;
            return a;
        };

        nt::UpdateRelease rel;
        rel.found = true;
        rel.tag = QStringLiteral("v9.9.9");
        rel.version = QStringLiteral("9.9.9");
        rel.assets << asset("NeverTube-9.9.9-Windows-10-11-x64-Setup.exe")
                   << asset("NeverTube-9.9.9-Windows-10-11-x86-Setup.exe")
                   << asset("NeverTube-9.9.9-Windows-7-8-x86-Setup.exe")
                   << asset("SHA256SUMS.txt");
        check(rel.installerAssetFor(QStringLiteral("x64")).name
                  == QLatin1String(
                         "NeverTube-9.9.9-Windows-10-11-x64-Setup.exe"),
              "con los tres en el release, x64 se lleva el suyo");
        check(rel.installerAssetFor(QStringLiteral("win10-x86")).name
                  == QLatin1String(
                         "NeverTube-9.9.9-Windows-10-11-x86-Setup.exe"),
              "el de Windows 10/11 de 32 bits no confunde con el de Win7");
        check(rel.installerAssetFor(QStringLiteral("win7-x86")).name
                  == QLatin1String(
                         "NeverTube-9.9.9-Windows-7-8-x86-Setup.exe"),
              "el de Win7 se lleva el suyo");
        check(rel.installerAssetFor(QString()).name.isEmpty(),
              "sin cocina no se elige nada");
        check(rel.installerAssetFor(QStringLiteral("arm64")).name.isEmpty(),
              "una cocina que no se publica no elige nada");

        // A release published under the OLD names keeps installing: the
        // selection accepts both spellings (compatibility with the releases
        // that exist today).
        nt::UpdateRelease oldRel;
        oldRel.found = true;
        oldRel.tag = QStringLiteral("v9.9.9");
        oldRel.version = QStringLiteral("9.9.9");
        oldRel.assets << asset("nevertube-9.9.9-setup-x64.exe")
                      << asset("nevertube-9.9.9-setup-win10-x86.exe")
                      << asset("nevertube-9.9.9-setup-win7-x86.exe");
        check(oldRel.installerAssetFor(QStringLiteral("x64")).name
                  == QLatin1String("nevertube-9.9.9-setup-x64.exe"),
              "el nombre antiguo sigue seleccionandose");

        // When both spellings are present the current one wins.
        nt::UpdateRelease both = rel;
        both.assets << asset("nevertube-9.9.9-setup-x64.exe");
        check(both.installerAssetFor(QStringLiteral("x64")).name
                  == QLatin1String(
                         "NeverTube-9.9.9-Windows-10-11-x64-Setup.exe"),
              "con los dos nombres en el release, manda el actual");

        // The name carries the version: another release never hands out this
        // one's file, whatever spellings exist.
        nt::UpdateRelease other;
        other.found = true;
        other.tag = QStringLiteral("v1.0.0");
        other.version = QStringLiteral("1.0.0");
        other.assets << asset("NeverTube-9.9.9-Windows-10-11-x64-Setup.exe");
        check(other.installerAssetFor(QStringLiteral("x64")).name.isEmpty(),
              "el instalador de OTRA version no se elige");
    }

    // --- the shape of the answer of the API ---
    {
        const QByteArray json = QByteArrayLiteral(
            "{\"tag_name\":\"v1.2.0\",\"body\":\"notes\","
            "\"published_at\":\"2026-10-27T00:00:00Z\",\"prerelease\":false,"
            "\"draft\":false,\"assets\":["
            "{\"name\":\"NeverTube-1.2.0-Windows-10-11-x64-Setup.exe\","
            "\"browser_download_url\":\"https://github.com/marlo4220mc/"
            "NeverTube/releases/download/v1.2.0/"
            "NeverTube-1.2.0-Windows-10-11-x64-Setup.exe\",\"size\":123},"
            "{\"name\":\"SHA256SUMS.txt\",\"browser_download_url\":"
            "\"https://github.com/marlo4220mc/NeverTube/releases/download/"
            "v1.2.0/SHA256SUMS.txt\",\"size\":10}]}");
        QString error;
        const nt::UpdateRelease rel = nt::parseReleaseJson(json, &error);
        check(error.isEmpty() && rel.found, "una respuesta valida se lee");
        check(rel.version == QLatin1String("1.2.0"),
              "la version sale del tag");
        check(rel.notes == QLatin1String("notes"), "las notas llegan");
        check(rel.assets.size() == 2, "los dos ficheros del release llegan");
        check(rel.sumsAsset().name == QLatin1String("SHA256SUMS.txt"),
              "el manifiesto se encuentra por su nombre exacto");
        const nt::UpdateAsset installer = rel.assets.at(0);
        check(installer.name
                  == QLatin1String(
                         "NeverTube-1.2.0-Windows-10-11-x64-Setup.exe"),
              "el instalador se encuentra por su nombre exacto");

        QString bad;
        nt::parseReleaseJson(QByteArrayLiteral("no json"), &bad);
        check(!bad.isEmpty(), "una respuesta rota explica el motivo");

        // A release of another version does NOT hand out this one's asset: the
        // name carries the version, so the selection is exact by construction.
        const nt::UpdateAsset other =
            nt::parseReleaseJson(
                QByteArrayLiteral("{\"tag_name\":\"v9.9.9\",\"assets\":[]}"),
                nullptr)
                .installerAsset();
        check(other.name.isEmpty(),
              "sin instalador en el release no se elige nada");
    }

    // --- SHA256SUMS.txt ---
    {
        const QByteArray sums = QByteArrayLiteral(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
            "  NeverTube-1.0.0-Windows-10-11-x64-Setup.exe\n"
            "1b4f0e9851971998e732078544c96b36c3d01cedf7caa332359d6f1d83567014"
            " *NeverTube-1.0.0-Windows-7-8-x86-Setup.exe\n"
            "# un comentario\n"
            "\n");
        QString hash;
        QString error;
        check(nt::checksumFor(
                  sums,
                  QStringLiteral("NeverTube-1.0.0-Windows-10-11-x64-Setup.exe"),
                  &hash, &error)
                  && hash == QLatin1String("ba7816bf8f01cfea414140de5dae2223"
                                           "b00361a396177a9cb410ff61f20015ad"),
              "el hash del instalador sale del manifiesto");
        // The binary-mode form `sha256sum` writes ("*name").
        check(nt::checksumFor(
                  sums,
                  QStringLiteral("NeverTube-1.0.0-Windows-7-8-x86-Setup.exe"),
                  &hash, &error)
                  && hash == QLatin1String("1b4f0e9851971998e732078544c96b3"
                                           "6c3d01cedf7caa332359d6f1d83567014"),
              "la forma con asterisco tambien vale");
        // A name that is a PREFIX of another entry is not an entry: the
        // comparison is exact, never a substring match.
        check(!nt::checksumFor(
                  sums,
                  QStringLiteral("NeverTube-1.0.0-Windows-10-11-x64-Setup"),
                  &hash, &error),
              "un nombre que solo es prefijo no cuenta");
        check(!nt::checksumFor(
                  sums,
                  QStringLiteral(
                      "NeverTube-1.0.0-Windows-10-11-x64-Setup.exe.asc"),
                  &hash, &error),
              "un fichero de firma del instalador no cuenta como el instalador");
        check(!nt::checksumFor(sums, QStringLiteral("otro.exe"), &hash, &error),
              "un fichero que no esta en el manifiesto no tiene hash");
        check(error == nt::tr("update.err.sumsEntryMissing"),
              "y el motivo es el que se le ensena a la persona");

        const QByteArray twice = QByteArrayLiteral(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
            "  NeverTube-1.0.0-Windows-10-11-x64-Setup.exe\n"
            "1b4f0e9851971998e732078544c96b36c3d01cedf7caa332359d6f1d83567014"
            "  NeverTube-1.0.0-Windows-10-11-x64-Setup.exe\n");
        check(!nt::checksumFor(
                  twice,
                  QStringLiteral("NeverTube-1.0.0-Windows-10-11-x64-Setup.exe"),
                  &hash, &error),
              "un manifiesto que nombra el fichero dos veces no es una respuesta");
        check(error == nt::tr("update.err.sumsAmbiguous"),
              "y el motivo dice que era ambiguo");

        // A line whose "hash" is not a SHA-256 is not an entry either: that is
        // what makes the `.sig`/`SHA256SUMS.txt.asc` noise around a manifest
        // harmless.
        check(!nt::checksumFor(
                  QByteArrayLiteral(
                      "deadbeef  NeverTube-1.0.0-Windows-10-11-x64-Setup.exe\n"),
                  QStringLiteral("NeverTube-1.0.0-Windows-10-11-x64-Setup.exe"),
                  &hash, &error),
              "una linea que no es un SHA-256 no cuenta");

        // The manifest must name the file the release actually carries: one
        // that still lists the OLD spelling does not verify the asset under
        // the new name (the release job writes the sums FROM the final names).
        check(!nt::checksumFor(
                  QByteArrayLiteral(
                      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff6"
                      "1f20015ad  nevertube-1.0.0-setup-x64.exe\n"),
                  QStringLiteral("NeverTube-1.0.0-Windows-10-11-x64-Setup.exe"),
                  &hash, &error)
                  && error == nt::tr("update.err.sumsEntryMissing"),
              "un manifiesto con el nombre antiguo no verifica el asset nuevo");
    }

    // --- the SHA-256 of a file ---
    {
        const QString dir =
            QDir::temp().absoluteFilePath(QStringLiteral("nevertube_selftest"));
        QDir().mkpath(dir);
        const QString file =
            dir + QStringLiteral("/update_sha256.txt");
        QFile::remove(file);
        {
            QFile f(file);
            check(f.open(QIODevice::WriteOnly), "se puede escribir el fichero de prueba");
            f.write(QByteArrayLiteral("abc"));
        }
        QString error;
        const QString hash = nt::sha256OfFile(file, &error);
        // The canonical SHA-256 of "abc".
        check(hash == QLatin1String("ba7816bf8f01cfea414140de5dae2223b00361a3"
                                    "96177a9cb410ff61f20015ad"),
              "el SHA-256 de un fichero se calcula bien");
        const QString other = QLatin1String("1b4f0e9851971998e732078544c96b3"
                                            "6c3d01cedf7caa332359d6f1d83567014");
        check(hash != other,
              "un hash que no es el suyo NO pasa la verificacion");

        // The one gate that decides: verifyDownloadedFile() is what the
        // download calls, and it is where a signature check would be added.
        check(nt::verifyDownloadedFile(file, hash, &error),
              "un fichero con el hash del manifiesto es apto para ejecutarse");
        check(!nt::verifyDownloadedFile(file, other, &error)
                  && error == nt::tr("update.err.hashMismatch"),
              "un hash distinto lo descarta y dice por que");
        check(!nt::verifyDownloadedFile(file, QString(), &error)
                  && error == nt::tr("update.err.sumsEntryMissing"),
              "sin hash con el que comparar no se acepta nada");

        QFile::remove(file);
        check(nt::sha256OfFile(file, &error).isEmpty()
                  && !error.isEmpty(),
              "un fichero que no existe no tiene hash y lo dice");
        check(!nt::verifyDownloadedFile(file, hash, &error)
                  && !error.isEmpty(),
              "un fichero que no existe tampoco pasa la verificacion");
    }

    // --- the download refuses to start when it cannot be verified ---
    {
        nt::UpdateRelease rel;
        rel.found = true;
        rel.tag = QStringLiteral("v1.0.0");
        rel.version = QStringLiteral("1.0.0");

        nt::UpdateDownloader dl;
        QStringList failures;
        QStringList readies;
        QObject::connect(&dl, &nt::UpdateDownloader::failed, &dl,
                         [&failures](const QString &m) { failures << m; });
        QObject::connect(&dl, &nt::UpdateDownloader::ready, &dl,
                         [&readies](const QString &) { readies << QString(); });

        // No assets at all: on Windows that is "the release does not carry the
        // installer", on Linux "this build has no installer". Either way it
        // fails BEFORE any download, and the message is a real translation.
        dl.start(rel);
        const QString flavor = nt::installerFlavorForBuild();
        const QString expected = flavor.isEmpty()
            ? nt::tr("update.err.noInstaller")
            : nt::tr("update.err.assetMissing");
        check(failures.size() == 1 && failures.first() == expected,
              "sin instalador la descarga no empieza y explica por que");
        check(failures.first() != QLatin1String("update.err.assetMissing")
                  && failures.first() != QLatin1String("update.err.noInstaller"),
              "el motivo esta traducido (las claves existen en el catalogo)");

        // Cancelling something that is not running changes nothing: the button
        // can be pressed after the download ended.
        int cancelled = 0;
        QObject::connect(&dl, &nt::UpdateDownloader::cancelled, &dl,
                         [&cancelled]() { ++cancelled; });
        dl.cancel();
        check(cancelled == 0 && !dl.isRunning(),
              "cancelar una descarga que no corre no hace nada");

        // The same release WITHOUT the manifest cannot be verified, so it is
        // not offered either.
        rel.assets.append(nt::UpdateAsset{
            nt::installerAssetName(QStringLiteral("1.0.0")),
            QUrl(QStringLiteral("https://github.com/marlo4220mc/NeverTube/"
                                "releases/download/v1.0.0/x.exe")),
            1});
        failures.clear();
        dl.start(rel);
        if (flavor.isEmpty()) {
            // Linux: the flavor is empty, so start() refuses BEFORE looking at
            // the assets (that is the branch the real build takes: there is
            // nothing to download).
            check(failures.size() == 1
                      && failures.first() == nt::tr("update.err.noInstaller"),
                  "en Linux no hay instalador y lo dice antes de mirar los assets");
        } else {
            check(failures.size() == 1
                      && failures.first() == nt::tr("update.err.sumsMissing"),
                  "sin SHA256SUMS.txt el instalador no se descarga");
        }

        // An asset that does NOT live on GitHub is not our installer.
        rel.assets.append(nt::UpdateAsset{
            QStringLiteral("SHA256SUMS.txt"),
            QUrl(QStringLiteral("https://example.com/SHA256SUMS.txt")), 1});
        failures.clear();
        dl.start(rel);
        if (flavor.isEmpty()) {
            check(failures.size() == 1
                      && failures.first() == nt::tr("update.err.noInstaller"),
                  "en Linux sigue sin bajarse nada (no hay instalador)");
        } else {
            check(failures.size() == 1
                      && failures.first() == nt::tr("update.err.sumsMissing"),
                  "un manifiesto fuera de GitHub no vale");
        }
    }

    // --- the names this updater looks for are the names the pipeline
    // publishes and the notes promise. The recipes run this selftest with the
    // repository as the working directory, so the cross-check runs in CI; from
    // an installed copy those files are not there and it says so and skips.
    {
        const QStringList sources = {
            QStringLiteral(".github/workflows/release.yml"),
            QStringLiteral("docs/release/notes.md"),
        };
        static const char *const kFragments[] = {
            "NeverTube-",
            "Windows-10-11-x64-Setup.exe",
            "Windows-10-11-x86-Setup.exe",
            "Windows-7-8-x86-Setup.exe",
            "SHA256SUMS.txt",
        };
        int present = 0;
        for (const QString &path : sources) {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly))
                continue;
            ++present;
            const QString text = QString::fromUtf8(f.readAll());
            for (const char *fragment : kFragments) {
                const bool has = text.contains(QLatin1String(fragment));
                if (!has)
                    std::printf("  UPDATE FAIL: %s no contiene '%s'\n",
                                path.toUtf8().constData(), fragment);
                check(has, "el pipeline y las notas publican los nombres "
                           "que el updater busca");
            }
        }
        if (present == 0)
            std::printf(
                "  UPDATE SKIP: sin checkout, no hay nombres que cruzar\n");
    }

    // Every string of the updater has to exist in the catalogs: a key that is
    // only ever passed as a VARIABLE (the error of checksumFor) would never be
    // seen by --selftest-i18n, and a typo would reach the screen as
    // "update.err.something".
    {
        static const char *const kKeys[] = {
            "update.title",          "update.installed",
            "update.available",      "update.notesTitle",
            "update.noNotes",        "update.now",
            "update.later",          "update.install",
            "update.installNote",    "update.cancelDownload",
            "update.downloading",    "update.ready",
            "update.hashNote",
            "update.cancelled",      "update.upToDate",
            "update.noRelease",      "update.linuxNote",
            "update.err.network",    "update.err.rateLimit",
            "update.err.http",       "update.err.json",
            "update.err.noInstaller", "update.err.assetMissing",
            "update.err.sumsMissing", "update.err.sumsEntryMissing",
            "update.err.sumsAmbiguous", "update.err.hashMismatch",
            "update.err.write",      "settings.checkUpdates",
        };
        QStringList missing;
        for (const char *key : kKeys) {
            if (nt::tr(key) == QString::fromUtf8(key))
                missing << QString::fromUtf8(key);
        }
        if (!missing.isEmpty())
            std::printf("  UPDATE FAIL: claves sin traducir: %s\n",
                        missing.join(QStringLiteral(", ")).toUtf8().constData());
        check(missing.isEmpty(),
              "todas las cadenas del actualizador estan en el catalogo");
    }

    std::printf("UPDATE SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}
