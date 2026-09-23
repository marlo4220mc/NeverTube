// Genera los PNG pre-pintados de la navegación inferior para cada color de la
// paleta de acentos (nt::accentOptions() en src/ui/theme.h). El runtime ya NO
// re-tiñe: NavButton solo elige el fichero por sufijo del acento vigente
// (:/nav/<key>_<state>_<suffix>.png). La fuente de cada estado es el PNG
// original (active/hover/pressed, con su sombreado propio) y el transform es
// idéntico al que pintaba el retint en runtime: por píxel, se conserva la
// luminosidad del sprite y se fuerza el tono/saturación de la variante
// (activo=acento, hover=accentHover, pressed=accentPressed), preservando el
// canal alfa (QColor::fromRgba — jamás QColor(QRgb), que fuerza alpha=255 y
// produce el "cuadrado negro"). El acento nativo de los sprites (rojo
// #FF0033) se COPIA byte-idéntico: el retint HSL es ligeramente lossy y el
// default no debe cambiar ni un píxel.
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

// Transform de tintado: misma lógica que el retint runtime ya verificado.
static QImage tintTo(const QString &srcPath, const QColor &target)
{
    QImage img(srcPath);
    if (img.isNull())
        return img;
    img = img.convertToFormat(QImage::Format_ARGB32);
    const int targetH = target.hslHue();
    const int targetS = target.hslSaturation();
    for (int y = 0; y < img.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = QColor::fromRgba(line[x]); // NO QColor(QRgb)
            if (c.alpha() == 0)
                continue;
            QColor r;
            r.setHsl(targetH, targetS, c.lightness(), c.alpha());
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