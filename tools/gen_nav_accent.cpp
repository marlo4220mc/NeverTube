// Genera los PNG pre-pintados de la navegación inferior para cada color de la
// paleta de acentos (nt::accentOptions() en src/ui/theme.h). El runtime ya NO
// re-tiñe: NavButton solo elige el fichero por sufijo del acento vigente
// (:/nav/<key>_<state>_<suffix>.png). El transform es un RECOLOR EXACTO al
// valor de la variante (active=acento, hover=accentHover, pressed=accentPressed):
// por píxel se fuerza el HSL COMPLETO del target y solo se conserva el alfa
// (la forma del glifo vía QColor::fromRgba — jamás QColor(QRgb), que fuerza
// alpha=255 y produce el "cuadrado negro").
//
// MOTIVO del recolor exacto (en vez de conservar la luminosidad del sprite,
// como hacía la versión anterior): la barra inferior está migrando al NUEVO
// rojo del icono (#FA011B). Preservando la luminosidad del sprite el tint
// salía en tonos que NO eran el rojo nuevo — activo #FE021F en vez de
// #FA011B, hover SALMÓN #FF697B en vez del #FF1B35 de los CTA, pressed
// #CB0118 en vez de #D90117 — y la nav quedaba fuera de la marca. Con el
// recolor exacto la nav muestra los MISMOS valores que los botones de acento
// (y el hover deja de salir pálido por la luz del sprite). El color NATIVO de
// los sprites originales es el rojo #FF0033; como el sufijo "rojo" de la
// paleta apunta ahora al rojo del icono (#FA011B), ese sufijo también se
// re-tiñe. nativeRed NUNCA cambia: es el píxel real de los PNG fuente, no el
// hex de la paleta.
//
// Tras añadir un color a nt::accentOptions() hay que volver a ejecutarla:
//   g++ -std=c++17 -fPIC tools/gen_nav_accent.cpp src/ui/theme.cpp \
//       $(pkg-config --cflags --libs Qt5Gui Qt5Core) -o /tmp/opencode/gen_nav_accent
//   /tmp/opencode/gen_nav_accent resources/nav
// Imprime por stdout las entradas del resources.qrc para los PNG generados.

#include <QColor>
#include <QFile>
#include <QImage>
#include <QString>
#include <QStringList>
#include <cstdio>

#include "../src/ui/theme.h"

// Recolor plano al valor EXACTO del target: por píxel se sustituye el RGB del
// sprite por el RGB literal de la variante y solo se conserva el alfa (la forma
// del glifo). NO se pasa por HSL a propósito: el redondeo del roundtrip
// RGB→HSL→RGB de Qt es lossy (1-3 unidades por canal — medido
// #FA011B→#FB011E) y aquí el objetivo es que la nav muestre el MISMO valor que
// los botones de acento, píxel a píxel.
static QImage tintTo(const QString &srcPath, const QColor &target)
{
    QImage img(srcPath);
    if (img.isNull())
        return img;
    img = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = QColor::fromRgba(line[x]); // NO QColor(QRgb)
            if (c.alpha() == 0)
                continue;
            QColor r(target);
            r.setAlpha(c.alpha());
            line[x] = r.rgba();
        }
    }
    return img;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "uso: %s <dir-nav>\n", argv[0]);
        return 2;
    }
    const QString dir = argv[1];
    // Claves de la barra inferior (setupNav en mainwindow.cpp).
    const QStringList keys = {QStringLiteral("home"), QStringLiteral("search"),
                              QStringLiteral("library"),
                              QStringLiteral("settings")};
    const QStringList states = {QStringLiteral("active"),
                                QStringLiteral("hover"),
                                QStringLiteral("pressed")};

    int written = 0;
    // Color nativo de los sprites originales (el rojo NeverTube): con él se
    // COPIA el PNG tal cual (byte-idéntico) en vez de re-tintar, que es lossy
    // (2-3 unidades por canal) y cambiaría el aspecto por defecto.
    const QColor nativeRed(0xFF, 0x00, 0x33);
    for (const nt::AccentOption &opt : nt::accentOptions()) {
        const QColor accent(opt.hex);
        const bool copyOriginal = (accent == nativeRed);
        for (const QString &key : keys) {
            for (const QString &state : states) {
                const QColor target =
                    state == QLatin1String("pressed")
                        ? nt::accentPressed(accent)
                        : state == QLatin1String("hover")
                              ? nt::accentHover(accent)
                              : accent;
                const QString src = dir + QStringLiteral("/%1_%2.png")
                                        .arg(key, state);
                const QString dst = dir + QStringLiteral("/%1_%2_%3.png")
                                        .arg(key, state, opt.suffix);
                if (copyOriginal) {
                    if (!QFile::exists(dst)
                        || !QFile::remove(dst)) {
                        std::fprintf(stderr, "ERROR al limpiar: %s\n",
                                     dst.toUtf8().constData());
                        return 1;
                    }
                    QFile f(src);
                    if (!f.copy(dst)) {
                        std::fprintf(stderr, "ERROR al copiar: %s\n",
                                     dst.toUtf8().constData());
                        return 1;
                    }
                    ++written;
                    std::printf(
                        "        <file>nav/%s_%s_%s.png</file>\n",
                        key.toUtf8().constData(),
                        state.toUtf8().constData(),
                        opt.suffix.toUtf8().constData());
                    continue;
                }
                const QImage out = tintTo(src, target);
                if (out.isNull()) {
                    std::fprintf(stderr, "SKIP (no lee origen): %s\n",
                                 src.toUtf8().constData());
                    continue;
                }
                if (!out.save(dst, "PNG")) {
                    std::fprintf(stderr, "ERROR al escribir: %s\n",
                                 dst.toUtf8().constData());
                    return 1;
                }
                ++written;
                std::printf("        <file>nav/%s_%s_%s.png</file>\n",
                            key.toUtf8().constData(), state.toUtf8().constData(),
                            opt.suffix.toUtf8().constData());
            }
        }
    }
    std::fprintf(stderr, "OK: %d PNG generados en %s\n", written,
                 dir.toUtf8().constData());
    return 0;
}