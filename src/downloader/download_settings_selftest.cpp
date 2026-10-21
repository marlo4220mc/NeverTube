#include "download_settings_selftest.h"

#include "download_settings.h"

#include <QStringList>

#include <cstdio>

bool downloadSettingsSelfTest()
{
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        std::printf("  DOWNLOAD-SETTINGS %s: %s\n", cond ? "OK" : "FAIL",
                     what);
        if (!cond)
            ok = false;
    };

    // --- the number of connections ---
    check(downloadsettings::connectionsOf(QStringLiteral("8")) == 8,
          "8 conexiones se respetan");
    check(downloadsettings::connectionsOf(QStringLiteral("1")) == 1,
          "1 conexion se respeta");
    check(downloadsettings::connectionsOf(QStringLiteral("16")) == 16,
          "16 conexiones se respetan");
    check(downloadsettings::connectionsOf(QStringLiteral("0"))
              == downloadsettings::kMinConnections,
          "0 conexiones suben al minimo");
    check(downloadsettings::connectionsOf(QStringLiteral("99"))
              == downloadsettings::kMaxConnections,
          "99 conexiones bajan al maximo");
    check(downloadsettings::connectionsOf(QStringLiteral("abc"))
              == downloadsettings::kDefaultConnections,
          "un valor que no es un numero es el de por defecto");
    check(downloadsettings::connectionsOf(QString())
              == downloadsettings::kDefaultConnections,
          "sin valor es el de por defecto");
    check(downloadsettings::valueForConnections(4) == QLatin1String("4"),
          "el valor guardado es el numero");
    check(downloadsettings::valueForConnections(99)
              == QString::number(downloadsettings::kMaxConnections),
          "el valor guardado tambien se recorta");

    std::printf("DOWNLOAD-SETTINGS SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}
