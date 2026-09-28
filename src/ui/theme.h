#pragma once

#include <QColor>
#include <QString>
#include <QVector>

namespace nt {

inline const QColor kBg(0x0F, 0x0F, 0x0F);
inline const QColor kPanel(0x18, 0x18, 0x18);
inline const QColor kElevated(0x1F, 0x1F, 0x1F);
inline const QColor kTop(0x10, 0x10, 0x10);
inline const QColor kBorder(0x26, 0x26, 0x26);
inline const QColor kHover(0x24, 0x24, 0x24);
inline const QColor kText(0xEC, 0xEC, 0xEC);
inline const QColor kMuted(0x8F, 0x8F, 0x8F);

// Roles que antes vivían como hex literales dentro del QSS de theme.cpp. Se
// suben aquí para que la paleta tenga UNA fuente: el QSS se genera con
// placeholders como ya hacía con el acento. Antes el QSS escribía #2A2A2A para
// bordes (y kBorder es #262626) y #232323 para hover (y kHover es #242424), así
// que el mismo rol tenía dos valores según se pintara a mano o por QSS.
inline const QColor kDisabled(0x6A, 0x6A, 0x6A);
inline const QColor kSelection(0x2D, 0x2D, 0x2D);
inline const QColor kHandle(0x2F, 0x2F, 0x2F);
inline const QColor kHandleHover(0x3A, 0x3A, 0x3A);
inline const QColor kPopover(0x20, 0x20, 0x20);

// Acento de la app (rojo NeverTube por defecto). No es const: se cambia en
// caliente desde Ajustes (color de acento). Cada widget que pinte con kAccent
// o construya QSS debe leerlo al repintar/regenerar, no cachearlo en build.
inline QColor kAccent(0xFF, 0x00, 0x33);

// Aplica un nuevo acento. Tras llamarla hay que re-generar el stylesheet
// global (styleSheet()) y repintar los widgets que pintan con kAccent.
inline void setAccent(const QColor &c) { kAccent = c; }

// Hex "#rrggbb" del acento y de sus variantes hover/pressed, para QSS
// generado dinámicamente (theme.cpp y estilos inline de páginas).
inline QString accentHex() { return kAccent.name(); }

// Variante hover: mezcla ~10% hacia blanco. Reproduce el "#FF1A47" histórico
// del rojo (lighter() no podía: multiplicar el verde 0 nunca lo sube).
inline QColor accentHover(const QColor &c)
{
    const int k = 26; // 255*0.10 ≈ 26
    return QColor(qMin(255, c.red() + k), qMin(255, c.green() + k),
                  qMin(255, c.blue() + k));
}
inline QString accentHoverHex() { return accentHover(kAccent).name(); }

// Variante pressed: ligeramente más oscuro (equivalente al "#E0002E" original).
inline QColor accentPressed(const QColor &c) { return c.darker(115); }
inline QString accentPressedHex() { return accentPressed(kAccent).name(); }

// Acento con alfa, como cadena "rgba(r,g,b,a)" lista para QSS. Existe para el
// relleno translúcido de la nav en su pestaña activa: al generarse desde el
// acento vigente sigue al cambio en caliente de Ajustes sin tocar los PNG
// pre-pintados de resources/nav (que son por estado, no por color).
inline QString accentAlpha(int alpha255)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(kAccent.red())
        .arg(kAccent.green())
        .arg(kAccent.blue())
        .arg(alpha255);
}

// Paleta de acentos: etiqueta visible (Ajustes), hex canónico y sufijo de
// fichero PNG de la navegación (nav/<key>_<state>_<suffix>.png, pre-pintados
// offline en tools/gen_nav_accent.cpp). FUENTE ÚNICA: settings_page y la nav
// la consultan; un color nuevo solo se añade aquí y se regeneran los PNG.
struct AccentOption {
    QString label;
    QString hex;
    QString suffix;
};
QVector<AccentOption> accentOptions();
// Sufijo PNG del acento vigente (p. ej. "azul"); el primero de la paleta si
// el acento no coincide con ninguna (solo posible editando la BD a mano).
QString accentSuffix();

QString styleSheet();

} // namespace nt