// Generates the pre-painted PNGs of the bottom navigation for each color of
// the accent palette (nt::accentOptions() in src/ui/theme.h). The runtime NO
// longer re-tints: NavButton just picks the file by suffix of the current
// accent (:/nav/<key>_<state>_<suffix>.png). The transform is an EXACT RECOLOR
// to the variant's value (active=accent, hover=accentHover, pressed=accentPressed):
// per pixel the full HSL of the target is forced and only the alpha is kept
// (the glyph shape via QColor::fromRgba — never QColor(QRgb), which forces
// alpha=255 and produces the "black square").
//
// REASON for the exact recolor (instead of preserving the sprite's luminance,
// like the previous version did): the bottom bar is migrating to the NEW
// icon red (#FA011B). Preserving the sprite's luminance, the tint came out in
// tones that were NOT the new red — active #FE021F instead of
// #FA011B, hover SALMON #FF697B instead of the CTA #FF1B35, pressed
// #CB0118 instead of #D90117 — and the nav was off-brand. With the
// exact recolor the nav shows the SAME values as the accent buttons
// (and the hover no longer comes out pale from the sprite's light). The NATIVE color of
// the original sprites is red #FF0033; since the palette's "rojo" suffix now
// points to the icon red (#FA011B), that suffix is also
// re-tinted. nativeRed NEVER changes: it is the real pixel of the source PNGs, not the
// palette hex.
//
// After adding a color to nt::accentOptions() it must be run again:
//   g++ -std=c++17 -fPIC tools/gen_nav_accent.cpp src/ui/theme.cpp \
//       $(pkg-config --cflags --libs Qt5Gui Qt5Core) -o /tmp/opencode/gen_nav_accent
//   /tmp/opencode/gen_nav_accent resources/nav
// Prints to stdout the resources.qrc entries for the generated PNGs.

#include <QColor>
#include <QFile>
#include <QImage>
#include <QString>
#include <QStringList>
#include <cstdio>

#include "../src/ui/theme.h"

// Flat recolor to the EXACT target value: per pixel the sprite's RGB is
// replaced with the variant's literal RGB and only the alpha is kept (the
// glyph shape). It deliberately does NOT go through HSL: the rounding of Qt's
// RGB→HSL→RGB roundtrip is lossy (1-3 units per channel — measured
// #FA011B→#FB011E) and the goal here is that the nav shows the SAME value as
// the accent buttons, pixel by pixel.
static QImage tintTo(const QString &srcPath, const QColor &target)
{
    QImage img(srcPath);
    if (img.isNull())
        return img;
    img = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = QColor::fromRgba(line[x]); // NOT QColor(QRgb)
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
    // Bottom bar keys (setupNav in mainwindow.cpp).
    const QStringList keys = {QStringLiteral("home"), QStringLiteral("search"),
                              QStringLiteral("library"),
                              QStringLiteral("settings"),
                              QStringLiteral("downloads")};
    const QStringList states = {QStringLiteral("active"),
                                QStringLiteral("hover"),
                                QStringLiteral("pressed")};

    int written = 0;
    // Native color of the original sprites (the NeverTube red): with it the
    // PNG is COPIED as is (byte-identical) instead of re-tinting, which is lossy
    // (2-3 units per channel) and would change the default look.
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