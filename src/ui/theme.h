#pragma once

#include <QColor>
#include <QPalette>
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

// Roles that used to live as hex literals inside the theme.cpp QSS. They are
// moved up here so the palette has a SINGLE source: the QSS is generated with
// placeholders as it already did with the accent. The QSS used to write #2A2A2A
// for borders (and kBorder is #262626) and #232323 for hover (and kHover is
// #242424), so the same role had two values depending on whether it was painted
// by hand or through the QSS.
inline const QColor kDisabled(0x6A, 0x6A, 0x6A);
inline const QColor kSelection(0x2D, 0x2D, 0x2D);
inline const QColor kHandle(0x2F, 0x2F, 0x2F);
inline const QColor kHandleHover(0x3A, 0x3A, 0x3A);
inline const QColor kPopover(0x20, 0x20, 0x20);

// Declarations of the QToolTip rule, WITHOUT the selector and WITHOUT the
// placeholders, so the single global sheet and the player's control bar can both
// use them and cannot drift apart. The bar needs its own copy because the
// tooltip windows of the bar do not receive the global sheet (measured: they come
// out with a transparent background, see the comment where m_ctrlBar sets its
// sheet).
inline QString tooltipDeclarations()
{
    return QStringLiteral(
        "background-color: %1;"
        "color: %2;"
        "border: 1px solid %3;"
        "padding: 3px 7px;"
        "font-size: 12px;")
        .arg(kPopover.name(), kText.name(), kBorder.name());
}

// The same rule, ready to be used inside a QToolTip { } block: for the control
// bar of the player, whose tooltips the global sheet does not reach.
inline QString tooltipRule()
{
    return QStringLiteral("QToolTip { %1 }").arg(tooltipDeclarations());
}

// Secondary text, in two weights. kMuted (#8F8F8F, 5.9:1 on kBg) is the
// dimmed one for short labels; these are for the long blocks, which are really
// read and so are not left at kMuted:
//
//   kMetaText (#9AA2AD, 7.4:1) -> video metadata line, description and
//     Home empty-state body. Lowering this to kMuted removed 20% of
//     contrast (7.4:1 -> 5.9:1) in the most-read text of the page.
//   kTextSoft (#C8C8C8, 11.5:1) -> player error label and body of the
//     hardware probe banner: dimmed text on a black surface.
//
// They were also written by hand as #9aa2ad / #c8c8c8 (with two different
// capitalizations for the same color) in five places.
inline const QColor kMetaText(0x9A, 0xA2, 0xAD);
inline const QColor kTextSoft(0xC8, 0xC8, 0xC8);

// App accent (by default the icon's red: measured on the new icon's play
// button it comes out #FA011B; the accent is aligned to the brand). It is not
// const: it is changed live from Settings (accent color). Every widget that
// paints with kAccent or builds QSS must read it when repainting/regenerating,
// not cache it at build time.
inline QColor kAccent(0xFA, 0x01, 0x1B);

// Applies a new accent. After calling it the global stylesheet (styleSheet())
// must be regenerated and the widgets that paint with kAccent repainted.
inline void setAccent(const QColor &c) { kAccent = c; }

// Hex "#rrggbb" of the accent and its hover/pressed variants, for QSS
// generated dynamically (theme.cpp and inline styles of pages).
inline QString accentHex() { return kAccent.name(); }

// TEXT SELECTION color across the app: the app's gray (kSelection, the same as
// the menus and the drop-down views), not the desktop theme's. It is needed
// because the QSS does not reach the Player description: it is a QLabel with
// Qt::TextSelectableByMouse and it paints the selection with
// QPalette::Highlight, which without this came from the system theme (blue on
// GTK), a color that is not in the app palette.
//
// ONLY Highlight and HighlightedText are touched: the rest of the roles are
// left as they were, so as not to change the background of anything that today
// depends on the desktop theme (tooltips, menus, status bars) and that the QSS
// does not cover. HighlightedText goes to kText because on kSelection the text
// must remain readable (11:1).
inline QColor textSelectionBg() { return kSelection; }
inline QColor textSelectionFg() { return kText; }
inline void applyTextSelectionPalette(QPalette &p)
{
    for (int g : { QPalette::Active, QPalette::Inactive }) {
        p.setColor(QPalette::ColorGroup(g), QPalette::Highlight, kSelection);
        p.setColor(QPalette::ColorGroup(g), QPalette::HighlightedText, kText);
    }
}

// Hover variant: mixes ~10% toward white. From the icon's red (#FA011B) it
// comes out #FF1B35 (lighter() could not: multiplying green 0 never raises it).
inline QColor accentHover(const QColor &c)
{
    const int k = 26; // 255*0.10 ≈ 26
    return QColor(qMin(255, c.red() + k), qMin(255, c.green() + k),
                  qMin(255, c.blue() + k));
}
inline QString accentHoverHex() { return accentHover(kAccent).name(); }

// Pressed variant: slightly darker (from the icon's red #FA011B it comes out
// #D90117).
inline QColor accentPressed(const QColor &c) { return c.darker(115); }
inline QString accentPressedHex() { return accentPressed(kAccent).name(); }

// Accent with alpha, as an "rgba(r,g,b,a)" string ready for QSS. It exists for
// the translucent fill of the nav on its active tab: generated from the current
// accent it follows the hot change in Settings without touching the pre-painted
// PNGs in resources/nav (which are per state, not per color).
inline QString accentAlpha(int alpha255)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(kAccent.red())
        .arg(kAccent.green())
        .arg(kAccent.blue())
        .arg(alpha255);
}

// Player control box (QWidget#playerOverlay). It stays translucent over the
// video, so it uses alpha and not an opaque token. 165/255 (~65%) is the usual
// one: measured over the video, #ECECEC text gives 16.2:1 on dark video, 10.1:1
// on medium and 5.5:1 on light, so opacity is not what has to be touched. What
// did not read was the border, hence overlayEdgeRgba().
inline QString overlayBgRgba() { return QStringLiteral("rgba(15,15,15,165)"); }

// Top edge of the box and outline of the track, in white at 28% (71/255).
// Reason (measured against the video, which with the floating box is the border
// neighbor): gray #2A2A2A gives only 1.33:1 on dark video and this white gives
// only 1.04:1 on light video. Neither delimits the box on its own, but with
// both there is always an edge that reads (2.47 / 3.63 / 12.60).
inline QString overlayEdgeRgba() { return QStringLiteral("rgba(255,255,255,71)"); }

// Accent palette: visible label (Settings), canonical hex and PNG file suffix
// of the navigation (nav/<key>_<state>_<suffix>.png, pre-painted offline in
// tools/gen_nav_accent.cpp). SINGLE SOURCE: settings_page and the nav query it;
// a new color is only added here and the PNGs are regenerated.
struct AccentOption {
    QString label;
    QString hex;
    QString suffix;
};
QVector<AccentOption> accentOptions();
// PNG suffix of the current accent (e.g. "azul"); the first of the palette if
// the accent does not match any (only possible by editing the DB by hand).
QString accentSuffix();

QString styleSheet();

// PURE QSS self-test (offline, no GPU or network): checks that no selector is
// declared twice. A repeated selector gives no warning and the override is mute
// (the last one wins), which is how a QLabel#sectionTitle of 22px slipped in
// behind the 16px one and raised the section titles of ALL pages without any
// test seeing it. Called from --selftest-theme.
bool themeSelfTest();

// GLYPH self-test (offline, no network or GPU): checks that EVERY non-ASCII
// character of the source-tree literals exists in the app's real font. Reason:
// the system default font does not have them all and Qt replaces the missing
// one with the missing-glyph box (measured: 2x5 px), i.e. an invisible button.
// Called from --selftest-glyphs.
bool glyphSelfTest();

} // namespace nt