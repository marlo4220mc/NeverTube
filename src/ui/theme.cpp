#include "theme.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QRawFont>
#include <QHash>
#include <QStringList>

#include <algorithm>

#include <cstdio>

#include "scan.h"

#include "../i18n.h"

namespace nt {

// Single source of the accent palette (Settings labels, persisted hex and
// suffix of the pre-painted navigation PNGs). The default is the icon's red
// (#FA011B, the play of the app icon; see kAccent in theme.h).
// Keep in sync with tools/gen_nav_accent.cpp only in the sense that
// regenerating the PNGs after changing HERE is mandatory.
QVector<AccentOption> accentOptions()
{
    return {
        {nt::tr("accent.red"), QStringLiteral("#FA011B"),
         QStringLiteral("rojo")},
        {nt::tr("accent.blue"), QStringLiteral("#1E88E5"),
         QStringLiteral("azul")},
        {nt::tr("accent.green"), QStringLiteral("#22C55E"),
         QStringLiteral("verde")},
        {nt::tr("accent.orange"), QStringLiteral("#F97316"),
         QStringLiteral("naranja")},
        {nt::tr("accent.purple"), QStringLiteral("#A855F7"),
         QStringLiteral("purpura")},
        {nt::tr("accent.cyan"), QStringLiteral("#22D3EE"),
         QStringLiteral("cian")},
        {nt::tr("accent.pink"), QStringLiteral("#EC4899"),
         QStringLiteral("rosa")},
    };
}

QString accentSuffix()
{
    const QString cur = kAccent.name(); // lowercase "#rrggbb"
    const auto opts = accentOptions();
    for (const AccentOption &o : opts)
        if (QColor(o.hex).name() == cur)
            return o.suffix;
    return opts.value(0).suffix;
}

QString styleSheet()
{
    // The QSS is a ~30 KB block. MSVC does NOT accept a string literal of
    // more than 16380 bytes (error C2026, "string too big, trailing characters
    // truncated"), so the whole literal does not compile on Windows even though on
    // Linux/GCC there is no such limit. It goes split into chunks in an array and is joined
    // here. Each chunk is well under the limit and the result is the
    // SAME qss as always, byte for byte, so --selftest-theme still sees
    // the whole sheet. Do NOT put this block back into a single literal.
    static const char *const kTrozos[] = {
        R"QSS(QWidget {
    background-color: transparent;
    color: #ECECEC;
    font-size: 13px;
}
QMainWindow { background-color: #0F0F0F; }

/* Tooltips: they are framework windows and by default are painted with the
   SYSTEM THEME palette (outside the app's control), so the text could come out
   unreadable depending on the desktop. App style: popover background,
   kText text and border, like the rest of the surfaces. QToolTip is a class,
   not an id: Qt applies it to the tooltip automatically.

   The declarations live in nt::tooltipRule() (theme.h) so that the control bar of
   the player, which needs the rule on its own widget, uses the SAME values: see
   the comment where m_ctrlBar sets its sheet. */
QToolTip {
@TOOLTIP_DECLS@
}

/* (No rules for shellPanel/contentHost: no setObjectName in the code used
   them, they were leftovers of the reverted split layout. Verified by grep.) */

QWidget#navBottom {
    background-color: #101010;
    /* Top hairline: separates the bar from the content (and from the mini
       player, which already has its own) without adding a widget. */
    border-top: 1px solid @BORDER@;
}

/* Bottom navigation bar buttons. The icons (PNG) are handled by NavButton
   per state; here only text and background are colored. The accent
   (NeverTube red by default) appears only in active or interaction
   states. */
QToolButton#navBtn {
    background: transparent;
    border: none;
    border-radius: 12px;
    /* Each button is a 104x52 capsule (mainwindow.cpp setupNav) and the
       content icon 26 + gap + 11px text line needs ~46: with 3px
       above and below 52 fits. Lowering the height clips the text or the
       icon, it does not shrink them. */
    padding: 3px 0;
    color: #8F8F8F;
    font-size: 11px;
}
QToolButton#navBtn:hover { background: @HOVER@; color: #ECECEC; }
/* IMPORTANT ORDER: :checked comes AFTER :focus. They have the same
   specificity and the last rule wins; with :focus last, focus overrode the
   accent fill and the freshly clicked tab came out gray with the accent
   text over #242424 = 3.92:1 (below the AA minimum of 4.5:1). */
QToolButton#navBtn:focus { background: @HOVER@; color: #ECECEC; }
/* The BOX that indicates position (the active tab) is a SHADE VARIATION
   of the icon's red, not a solid fill: the background mixes the accent with
   the bar at 25% (@ACCENT_A25@ = #4A0C13 over #101010, from #FA011B). The text
   goes up to the hover variant (#FF1B35) because over the tint the full accent
   falls to 3.76:1 and hover gives 4.04:1; the icon (PNG pre-painted in accent)
   keeps ITS color — the box is "in tune" with the icon, it does not replace it.
   More fill (A30 = #560C13) shows more red but leaves the text at 3.49:1, so A30
   only on hover, which is transient. */
QToolButton#navBtn:checked { background: @ACCENT_A25@; color: @ACCENT_HOVER@; }
/* Active on hover/focus: the fill goes up one step (A30) and the text stays
   in the hover variant; it is the same gesture as the old checked:hover state. */
QToolButton#navBtn:checked:hover { background: @ACCENT_A30@; color: @ACCENT_HOVER@; }
QToolButton#navBtn:checked:focus { background: @ACCENT_A25@; color: @ACCENT_HOVER@; }
/* When pressed (any tab) the fill returns to the subtle 10% tint:
   it is transient and so the pressed icon (red) reads over the background. */
QToolButton#navBtn:pressed { background: @ACCENT_A10@; color: @ACCENT_HOVER@; }
QToolButton#navBtn:disabled { color: @DISABLED@; }

QLabel { background: transparent; }
QLabel#sectionTitle { font-size: 16px; font-weight: 600; color: #ECECEC; }
/* Video title on the Player page: one point larger than the section
   titles (it is the page's H1). */
QLabel#watchTitle { font-size: 17px; font-weight: 600; color: @TEXT@; }
QLabel#sectionSub { font-size: 12px; color: #8F8F8F; }
/* Empty state of a Library section: box with a dashed border and the
   text centered, instead of a lone line of gray text. */
QLabel#emptyState {
    color: @METATEXT@; font-size: 13px;
    background: #181818; border: 1px dashed @BORDER@; border-radius: 10px;
    padding: 18px;
}

QPushButton {
    background-color: #1F1F1F;
    border: 1px solid @BORDER@;
    border-radius: 8px;
    padding: 7px 14px;
    color: #ECECEC;
}
QPushButton:hover { background-color: @HOVER@; }
QPushButton:pressed { background-color: @SELECTION@; }
/* Focus by border COLOR, not by thickness: the button already reserves 1px, so
   adding a 2 px ring would shift the content on every state change. */
QPushButton:focus { border-color: @ACCENT@; }
/* The disabled button only lost its color: it kept background and border, so
   it still read as clickable. Here it lowers emphasis in the three channels. */
QPushButton:disabled {
    color: @DISABLED@;
    background-color: @BORDER@;
    border-color: @BORDER@;
}
/* BRAND CTA (accent buttons): FLAT accent fill — without the previous 3D
   gloss — with a 1 px white hairline at ~15 % so the button reads
   over the dark background (pattern of current design systems: a flat
   accent button blends into the background without that edge). Crisp states:
   hover lighter, pressed darker. */
QPushButton#primary {
    background: @ACCENT@; border: 1px solid rgba(255,255,255,38);
    color: #FFFFFF; font-weight: 600;
}
QPushButton#primary:hover { background: @ACCENT_HOVER@; }
QPushButton#primary:pressed { background: @ACCENT_PRESSED@; }
QPushButton#primary:disabled {
    color: @DISABLED_ON_ACCENT@; background: @ACCENT_MUTED@;
    border-color: @ACCENT_MUTED@;
}
QPushButton#ghost { background: transparent; border: 1px solid @BORDER@; border-radius: 6px; }
QPushButton#ghost:hover { background: #1F1F1F; }
QPushButton#ghost:pressed { background: @HOVER@; }
QPushButton#ghost:focus { border-color: @ACCENT@; }
/* Ghost with icon ONLY (the search clear X). The global padding
   (7px 14px) leaves 2x2 px of content inside the button's 28 px, so
   without this the icon overflows. It is a property selector so the button
   keeps inheriting the rest of #ghost. */
QPushButton#ghost[iconOnly="true"] { padding: 0; }
QPushButton#iconBtn { background: transparent; border: none; border-radius: 6px; padding: 6px; }
QPushButton#iconBtn:hover { background: @HOVER@; }
QPushButton#iconBtn:pressed { background: @SELECTION@; }
/* iconBtn has no border, so focus goes by background (same as navBtn). */
QPushButton#iconBtn:focus { background: @HOVER@; color: @ACCENT@; }
QPushButton#iconBtn:disabled { color: @DISABLED@; }

QLineEdit, QComboBox {
    background-color: #1F1F1F;
    border: 1px solid @BORDER@;
    border-radius: 10px;
    padding: 8px 12px;
    /* Selecting text in the search field goes in the app's gray, like the rest
       of the app, not in the accent: in red it looked like an error. It is the
       same token as the menus (kSelection) and the selected text stays in kText
       so that it remains readable (11:1). */
    selection-background-color: @SELECTION@;
    selection-color: @TEXT@;
    color: #ECECEC;
}
QLineEdit:focus, QComboBox:focus { border-color: @ACCENT@; }
QLineEdit:disabled, QComboBox:disabled { color: @DISABLED@; background-color: @BORDER@; }

/* Non-modal popover (e.g. create playlist): floating panel that does not
   block the rest of the page. */
QFrame#popupDialog {
    background-color: @POPOVER@;
    border: 1px solid @BORDER@;
    border-radius: 12px;
}
/* The confirmation dialog (nt::confirm) is NOT a QDialog: it is a QWidget child
   of the window (a window of its own is a toplevel and steals focus and the
   pointer from the user). That is why the QDialog { background: transparent }
   rule is no longer needed, it existed only so that its frame showed with
   rounded corners. */
QLabel#popupTitle { font-size: 15px; font-weight: 600; color: #ECECEC; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox::down-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid #8F8F8F;
    margin-right: 8px;
}
QComboBox QAbstractItemView {
    background-color: #1F1F1F;
    border: 1px solid @BORDER@;
    selection-background-color: @SELECTION@;
    color: #ECECEC;
    outline: 0;
}

)QSS",
        R"QSS(QScrollArea { border: none; background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: @HANDLE@; border-radius: 5px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: @HANDLE_HOVER@; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: @HANDLE@; border-radius: 5px; min-width: 24px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical,
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }

QSlider::groove:horizontal { height: 4px; background: @HANDLE@; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @ACCENT@; border-radius: 2px; }
QSlider::handle:horizontal {
    width: 12px; height: 12px; margin: -4px 0;
    background: #FFFFFF; border-radius: 6px;
}
QSlider::handle:horizontal:hover { background: @ACCENT@; }
QSlider:disabled::sub-page:horizontal { background: @SELECTION@; }
QSlider:disabled::handle:horizontal { background: @DISABLED@; }

QProgressBar { border: none; background: @BORDER@; border-radius: 3px; text-align: center; color: transparent; }
QProgressBar::chunk { background: @ACCENT@; border-radius: 3px; }
/* Mini player progress bar: 2 px (thinner than the global one), with radius 1
   so the chunk does not peek through the corners. */
QProgressBar#miniProgress { background: @HANDLE@; border: none; border-radius: 2px; }
/* Mini fill in the ACCENT: follows the color from settings (the QSS is
   regenerated when it changes, like the rest of the theme). Gray track. The
   height (4 px) is set in MiniPlayerBar/setupMiniBar. */
QProgressBar#miniProgress::chunk { background: @ACCENT@; border-radius: 2px; }

QToolButton { background: transparent; border: none; border-radius: 6px; padding: 5px; }
QToolButton:hover { background: @HOVER@; }
QToolButton:pressed { background: @SELECTION@; }
QToolButton:focus { background: @HOVER@; }

/* The #cards object is set by VideoListView, which derives from QListView (not
   from QListWidget): with the selector "QListWidget#cards" the rule did NOT
   apply (verified) and the view kept the default frame of
   QAbstractScrollArea. That frame left the viewport 2 px shorter than the
   content of the Home sections (fixed height = rows * cell height),
   i.e. an INTERNAL scroll range of 2 px: rolling the wheel over it, the
   section gave a "small vertical scroll". With the correct selector the range
   is 0 at any size and setting. */
QListView#cards {
    background: transparent;
    border: none;
    outline: 0;
}
QListView#cards::item { background: transparent; border: none; }
QListView#cards::item:selected { background: transparent; }

QMenu { background-color: #1F1F1F; border: 1px solid @BORDER@; }
QMenu::item { padding: 7px 24px; }
QMenu::item:selected { background-color: @SELECTION@; }
QMenu::item:disabled { color: @DISABLED@; }

QStatusBar { background: #101010; color: #8F8F8F; border-top: 1px solid #1F1F1F; }

/* The mini player used kPanel (#181818), the same as the bottom nav, so
   the two bars merged into a single line. kElevated separates them. */
/* The mini player shares the tone of the bottom bar (kTop #101010), not the
   kElevated of before: the two bars read as a single piece. */
QWidget#miniBar { background-color: #101010; border-top: 1px solid @BORDER@; }
/* Hover of the whole bar: lightens the background to signal that it is clickable
   (expands to the Player). The property is set by MiniPlayerBar. #181818 (the
   previous tone) looked pale next to #101010: it was lowered to #141414, a subtle
   lift that still signals hover without breaking the single piece with the nav. */
QWidget#miniBar[hover="true"] { background-color: #141414; }
/* Mini bar buttons: denser than those of the player row
   (padding 4 instead of 6), because the mini is a compact surface. */
QWidget#miniBar QPushButton#iconBtn { padding: 4px; }
QWidget#watchBody { background-color: #0F0F0F; }
QWidget#cardPanel { background-color: #181818; border: 1px solid @BORDER@; border-radius: 10px; }

/* Player controls box. The 12 px separator that makes it float is not from
   QSS but from the layout (gl1 in watch_page.cpp); here only the borders go.
   The top one is white at 28 % and the rest in the border gray:
   separately none reads against every video (1.33:1 the gray on dark,
   1.04:1 the white on light), together there is always an edge that delimits it. */
QWidget#playerOverlay {
    background-color: @OVERLAY_BG@;
    border: 1px solid @BORDER@;
    border-top: 1px solid @OVERLAY_EDGE@;
    border-radius: 12px;
}
/* Split clock of the row: elapsed to the left of the slider and duration to
   the right (see WatchPage). Secondary text, below the icons. */
QLabel#ovPos, QLabel#ovDur { color: @TEXTSOFT@; font-size: 12px; }
/* In a live stream (dynamic property live): the "EN VIVO" mark of the clock goes
   in the accent and the negative timer writes whatever WatchPage indicates. */
QLabel#ovPos[live="true"] { color: @ACCENT@; }
/* Subtle vertical separator between the subgroups of the row (transport / volume
   / actions): 1x20 px, with the same light edge as the panel border. */
QFrame#ovSep { background: @OVERLAY_EDGE@; border: none; }

/* --- Player Page --------------------------------------------------------
   The two overlays (spinner and error) live inside m_overlay, over the
   video, so both are transparent: any background would cover the frame. */
QLabel#ovSpinner {
    background: transparent; color: @MUTED@; font-size: 13px;
}
QLabel#ovError {
    background: transparent; color: @TEXTSOFT@;
    font-size: 14px; padding: 16px;
}
/* Info column: metadata and description. */
QLabel#watchMeta { color: @METATEXT@; font-size: 12px; }
QLabel#watchDesc { color: @METATEXT@; font-size: 12px; background: transparent; }
/* Player description card: groups the text, which is shown
   COMPLETE (no collapse and no "…more"). */
QFrame#watchDescCard {
    background: #181818; border: 1px solid @BORDER@; border-radius: 10px;
}
/* Bottom gradient of the collapsed description. The Channel page uses it (its
   description IS trimmed to 3 lines with "…more"); the Player no longer uses it. */
QFrame#descFade {
    border: none;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                stop:0 rgba(24,24,24,0), stop:1 #181818);
}
/* Title of the "A continuacion" block and counter to its right. */
QLabel#queueTitle { font-weight: 600; font-size: 13px; color: @TEXT@; }
QLabel#queueCount {
    color: @METATEXT@; font-size: 11px;
    background: #1F1F1F; border: 1px solid @BORDER@; border-radius: 9px;
    padding: 1px 8px;
}

/* "…more"/"…less" button of the collapsed description (Channel page: its
   description IS trimmed). Discreet link, not a capsule. */
QPushButton#descToggle {
    background: transparent; border: none; padding: 0;
    color: @MUTED@; font-size: 12px; font-weight: 600; text-align: left;
}
QPushButton#descToggle:hover { color: @TEXT@; }
/* Tertiary action at the end of a section header (Library: "Ver
   todo", "Administrar"): discreet link, not the #ghost capsule with a frame. They
   were five bordered capsules aligned to the right of each title, which competed
   with the section content. Without a border in the base, so the global :focus
   is only seen if the button actually gets focus (same as #descToggle). */
QPushButton#sectionLink {
    background: transparent; border: none; padding: 4px 8px;
    color: @MUTED@; font-size: 12px; font-weight: 600;
}
QPushButton#sectionLink:hover { color: @TEXT@; }
/* Without a border there is nowhere to draw the focus ring, so with the keyboard
   the link is marked with lighter text. That is what hover does, so that
   "tabbing" through the Library looks the same as moving the mouse. */
QPushButton#sectionLink:focus { color: @TEXT@; }
/* "Volver" of the four surfaces with back navigation (Channel, History,
   Playlist and the Player). It is a LINK, not a capsule: same criteria as
   #sectionLink, because "back" is a navigation action, not a control of
   the page. Before it was #ghost (a bordered capsule) in the three pages and a
   floating button over the video in the Player, which also covered the image.
   The icon is drawn by nt::icon (QPainter), not a glyph of the text: the "◀" of
   "Volver" was a character that does not exist in the app's font. */
QPushButton#backLink {
    background: transparent; border: none; padding: 4px 8px;
    color: @MUTED@; font-size: 13px; font-weight: 600; text-align: left;
}
QPushButton#backLink:hover { color: @TEXT@; }
/* Without a border there is nowhere to draw the focus ring (like #sectionLink):
   the keyboard is marked with the lighter text, same as hover. */
QPushButton#backLink:focus { color: @TEXT@; }
/* Channel button below the title: icon on the left and text on the right.
   It is not #ghost because it needs 8px padding instead of 6px. */
QPushButton#channelBtn {
    background: transparent; border: none; border-radius: 8px;
    text-align: left; padding: 8px;
    color: @TEXT@; font-weight: 600; font-size: 13px;
}
QPushButton#channelBtn:hover { background: #1F1F1F; }
/* Player action buttons: icon only, in a circular pill. The state is
   decided by the boolean property `active`, like #subscribe with its own; the
   icon color is painted by updateActionButtons (the QSS does not tint QIcons).
   WITHOUT FRAME (user rule, same as Me gusta / No me gusta): the border goes
   transparent and the :focus stays transparent, because the
   global QPushButton:focus paints it in the accent and pressing showed a red ring. */
QPushButton#actionChip {
    background: #1F1F1F; border: 1px solid transparent; border-radius: 22px;
    padding: 0;
}
QPushButton#actionChip:hover { background: @HOVER@; }
QPushButton#actionChip:focus { border-color: transparent; }
QPushButton#actionChip[active="true"] { background: @BORDER@; }
QPushButton#actionChip[active="true"]:hover { background: @SELECTION@; }

)QSS",
        R"QSS(/* Me gusta / No me gusta segmented group: the outer corners
   rounded and a hairline between the two. The group background is the chip's. */
QWidget#actionSeg { background: #1F1F1F; border-radius: 22px; }
QPushButton#actionSegL, QPushButton#actionSegR {
    background: transparent; border: none; padding: 0;
}
QPushButton#actionSegL:hover, QPushButton#actionSegR:hover { background: @HOVER@; }
QPushButton#actionSegL {
    border-top-left-radius: 22px; border-bottom-left-radius: 22px;
    border-right: 1px solid @BORDER@;
}
QPushButton#actionSegR {
    border-top-right-radius: 22px; border-bottom-right-radius: 22px;
}
QPushButton#actionSegL[active="true"], QPushButton#actionSegR[active="true"] {
    background: @BORDER@;
}
QPushButton#actionSegL[active="true"]:hover,
QPushButton#actionSegR[active="true"]:hover { background: @SELECTION@; }

/* Quality menu: SAME border language as the player box
   (#playerOverlay) — translucent overlay background, @BORDER@ border and light
   top edge (@OVERLAY_EDGE@), radius 12. Before it was #1F1F1F with a @SELECTION@
   border and radius 6, and it clashed with the player. */
QFrame#qualityMenu {
    background-color: @OVERLAY_BG@;
    border: 1px solid @BORDER@;
    border-top: 1px solid @OVERLAY_EDGE@;
    border-radius: 12px;
}
QFrame#qualityMenu QPushButton {
    background: transparent; color: #ECECEC; border: none;
    text-align: left; padding: 6px 8px; border-radius: 6px;
}
QFrame#qualityMenu QPushButton:hover { background: @SELECTION@; }

/* Row of a previously searched query (History tab of the search page).
   It is a flat QPushButton because it needs text-align:left with an icon on the
   left, which the base QPushButton rule does not give. */
QPushButton#historyRow {
    text-align: left; background: transparent; border: none;
    color: @TEXTSOFT@; padding: 7px 4px;
}
QPushButton#historyRow:hover { background: #1F1F1F; border-radius: 6px; }

/* Search suggestions dropdown. It reuses the color variables
   of the popover (#202020) and the historyRow hover so that it does not introduce
   new tones. suggestRow is the clickable row; suggestDivider separates the two
   groups (what YouTube suggests on top, what you already searched below). */
QFrame#suggestPanel {
    background-color: @POPOVER@; border: 1px solid @BORDER@; border-radius: 10px;
}
/* Label of the second group, in uppercase with the line beside it (CSS ::after
   does not exist in QSS, so the line is a sibling QFrame: suggestDividerRule).
   The text is set to uppercase in code, not here. */
QWidget#suggestDivider { background: transparent; }
QLabel#suggestGroupTitle {
    color: @MUTED@; font-size: 9px; font-weight: 600;
}
QFrame#suggestDividerRule { color: @BORDER@; background: @BORDER@; border: none; }
/* Dropdown row. It is a QWidget (not a button) because it needs partial
   bold, which Qt does not draw in a button. The hover is from the mouse and the
   [highlight] highlight is from the keyboard: they are different states and the
   second has to be able to move with the arrows without the mouse overriding it.
   The LEFT border is always reserved (3 px, transparent if not
   highlighted) so that the accent bar does not push the text when it appears. */
QWidget#suggestRow {
    background: transparent; border: none; border-left: 3px solid transparent;
    border-radius: 6px; color: @TEXTSOFT@;
}
QWidget#suggestRow:hover { background: @SELECTION@; }
/* Keyboard highlight: accent bar on the left + background with an accent
   veil. Before it was #1F1F1F, darker than the popover (#202020): nothing was
   visible. The text stays in kTextSoft/kMuted, which over the veil still give
   8:1 and 4:1, so the child labels' colors do not need touching. */
QWidget#suggestRow[highlight="true"] {
    background: @ACCENT_A16@; border-left: 3px solid @ACCENT@;
}
/* Entity detail ("Cantante y compositor peruano") on the right, in
   faint, so that the query leads. */
QLabel#suggestDetail { color: @MUTED@; font-size: 12px; }
/* "Tab" badge of the history rows: it is the key with which they are taken.
   With a box, like the <kbd> of the mockup. */
QLabel#suggestBadge {
    color: @MUTED@; font-size: 11px; border: 1px solid @BORDER@; border-radius: 4px;
}
/* The dropdown gets a scrollbar when the rows exceed the height cap. The
   surface goes transparent so the panel background shows, not the default
   gray of the viewport. */
QScrollArea#suggestScroll { background: transparent; border: none; }
QWidget#suggestRows { background: transparent; }

)QSS",
        R"QSS(/* --- Settings --------------------------------------------------
   Three header levels, and on purpose they are NOT reused among themselves:
     pageHeading           18 px, title of the whole page
     sectionTitle          16 px, section title (shared with the other
                           pages, which already used it)
     settingsSectionTitle  11 px in small caps with tracking, header of a
                           Settings section ("INTERFAZ", "DATOS"...)
   And the settings rows: label on the left with a minimum width so that
   all the menus fit, value on the right. */
QLabel#pageHeading { font-size: 18px; font-weight: 700; }
QLabel#settingsSectionTitle {
    font-size: 11px; font-weight: 600; color: @MUTED@;
    letter-spacing: 0.5px;
}
QLabel#settingsRowLabel { color: @TEXT@; min-width: 200px; }
QLabel#settingsRowValue { color: @MUTED@; }
QLabel#settingsNote { font-size: 10px; color: @MUTED@; }
QLabel#settingsVersion { font-size: 12px; color: @TEXT@; }
/* Card that groups the rows of a Settings section: it replaces the flat
   list of separate rows split by hairlines. */
QFrame#settingsCard {
    background: #181818; border: 1px solid @BORDER@; border-radius: 10px;
}
QLabel#popupText { color: @MUTED@; }
/* The Settings scroll area paints no background: the page already has its own. */
QScrollArea#settingsScroll { background: transparent; }

/* Settings action buttons. #actionBtn and #dataActionBtn are the same
   aspect with a slightly larger padding (150 px minimum-width rows versus those
   of the DATA section); they are kept separate so as not to change the box. */
QPushButton#actionBtn {
    font-size: 12px; padding: 7px 14px; background-color: #1F1F1F;
    border: 1px solid @BORDER@; border-radius: 8px; color: @TEXT@;
    /* Common minimum width: aligns the controls column inside the card. */
    min-width: 150px;
}
QPushButton#dataActionBtn {
    font-size: 12px; padding: 8px 16px; background-color: #1F1F1F;
    border: 1px solid @BORDER@; border-radius: 8px; color: @TEXT@;
}
/* "Restablecer aplicacion": same background as the previous ones and only the
   border and the text in accent, so that it reads as destructive without being a
   primary button. The accent comes from @ACCENT@, so it repaints itself. */
QPushButton#accentOutline {
    font-size: 12px; padding: 8px 16px; background-color: #1F1F1F;
    border: 1px solid @ACCENT@; border-radius: 8px;
    color: @ACCENT@; font-weight: 600;
}

/* Subscribe button of the channel card: both states in the global QSS, with
   the boolean property `subscribed` as selector. The accent comes from
   @ACCENT@, so a color change repaints it without touching the C++.
   It does NOT set `border: none`: the setStyleSheet calls it replaces did not
   touch the border, so the button kept the 1px @BORDER@ of the base rule. In
   the subscribed state it is not noticeable (border and background are the same
   gray), but in the accent one it is visible. */
QPushButton#subscribe {
    background: @ACCENT@; border: 1px solid rgba(255,255,255,38);
    border-radius: 18px; padding: 0 18px;
    color: #FFFFFF; font-weight: 600; font-size: 13px;
}
QPushButton#subscribe:hover { background: @ACCENT_HOVER@; }
QPushButton#subscribe[subscribed="true"] {
    background: @BORDER@; border-color: @BORDER@; color: @TEXT@;
}
QPushButton#subscribe[subscribed="true"]:hover { background: @SELECTION@; }

/* Compact channel rows (channel_row) */
QLabel#channelRowTitle { color: @TEXT@; background: transparent; }
QLabel#channelRowMeta  { color: @MUTED@; background: transparent; }

/* Placeholder and page titles */
QLabel#pageTitle { font-size: 18px; font-weight: 700; }
QLabel#placeholderHint { color: @MUTED@; font-size: 14px; padding: 40px; }
/* NOTE: a QLabel#sectionTitle does NOT go here. It was here (22px/700, part of
   the migration to global QSS) and since it comes AFTER the line 93 rule it WON
   by order, raising the section titles of ALL pages from 16 to 22 px
   (Home, Discover, Search, channel, history, library, playlist). The design
   is 16 px: the comment in settings_page.cpp says so ("A proposito NO es
   #sectionTitle (16 px)"). With a duplicated selector the QSS warns of nothing
   and the override is silent, so the #sectionTitle rule lives in a single place. */

/* --- Channel Page ---------------------------------------------------------
   Before it was the ONLY thing in the app without style: the three tabs were a
   QTabWidget and there is no QTabWidget/QTabBar rule in this QSS, so they came
   out with the look of the desktop theme and the "pane" painted a nearly black
   column (measured #020303) below the list, darker than the page
   background. Now the tabs are buttons inside a capsule with their own
   style, like the bottom navigation.
   The card groups avatar, name, subscribers, actions and description in a
   box (#181818), like #settingsCard and #watchDescCard. */
QFrame#channelCard {
    background: #181818; border: 1px solid @BORDER@; border-radius: 12px;
}
/* Channel name: it is the H1 of the page, one point above the 18 px of
   #pageHeading because it is the datum you enter into. */
QLabel#channelName { font-size: 19px; font-weight: 700; color: @TEXT@; background: transparent; }
QLabel#channelMeta { color: @METATEXT@; font-size: 12px; background: transparent; }
QLabel#channelDesc { color: @METATEXT@; font-size: 12px; background: transparent; }
/* Card avatar (80x80, radius 40). The real circle is made by
   nt::circularAvatar (crops the pixmap): this radius only rounds the background. */
QLabel#channelAvatar { border-radius: 40px; background: transparent; }
/* Tabs capsule: the background is the card's, with a hairline border
   like the rest of the surfaces. */
QWidget#channelTabs { background: #181818; border: 1px solid @BORDER@; border-radius: 9px; }
QPushButton#tabBtn {
    background: transparent; border: none; border-radius: 6px;
    padding: 6px 16px; color: @MUTED@; font-size: 12px; font-weight: 600;
}
/* Focus is NOT marked: they are tabs, not action buttons, and the accent ring
   when pressing them would read as an error (same criteria as #iconBtn). */
QPushButton#tabBtn:focus { border-color: transparent; }
QPushButton#tabBtn:hover { background: #1F1F1F; color: @TEXT@; }
/* The selected one carries the accent in the text, like the bottom nav. The
   background cannot be distinguished from the capsule: both are #181818. */
QPushButton#tabBtn[selected="true"] { color: @ACCENT_HOVER@; }
/* Video counter of the active tab: same pill as #queueCount. */
QLabel#tabCount {
    color: @METATEXT@; font-size: 11px;
    background: #1F1F1F; border: 1px solid @BORDER@; border-radius: 9px;
    padding: 1px 8px;
}

)QSS",
        R"QSS(/* Accent chip ("Ir a Buscar" of the Home empty state): same brand
   CTA as #primary — flat fill + hairline + crisp states. */
QPushButton#accentChip {
    background: @ACCENT@; border: 1px solid rgba(255,255,255,38);
    border-radius: 8px;
    color: #FFFFFF; font-size: 12px; padding: 8px 18px;
}
QPushButton#accentChip:hover { background: @ACCENT_HOVER@; }
QPushButton#accentChip:pressed { background: @ACCENT_PRESSED@; }

/* Home empty state (goes over a popupDialog) */
QLabel#emptyStateTitle {
    color: @TEXT@; font-size: 15px; font-weight: 700;
    border: none; background: transparent;
}
QLabel#emptyStateBody {
    color: @METATEXT@; font-size: 12px;
    border: none; background: transparent;
}

/* Library rows (playlists, history: same item card) */
QLabel#libraryItemTitle {
    color: @TEXT@; font-weight: 600; font-size: 14px;
    background: transparent;
}
QLabel#libraryItemMeta {
    color: @MUTED@; font-size: 12px; background: transparent;
}

/* "Detectando hardware" banner (hwProbePopup). It was entirely in an
   inline setStyleSheet with rgba(18,18,18,0.96) and #3a3a3a, which are values
   that the global QSS did not have: that is why it was another surface that the
   accent change did not cover. */
QWidget#hwProbePopup {
    background-color: @POPOVER@;
    border: 1px solid @HANDLE@;
    border-radius: 10px;
}
QLabel#hwPopupTitle { color: #FFFFFF; font-size: 16px; font-weight: 600; }
QLabel#hwPopupBody { color: @TEXTSOFT@; font-size: 12px; }

/* Mini player. The slot itself (miniVideoHost) needs no rule: it inherits the
   transparent background of the global QWidget, which is correct, because
   underneath goes the reparented live PlayerWidget. */
QLabel#miniThumb {
    background-color: @BORDER@; border-radius: 6px;
    color: @MUTED@; font-size: 10px;
}
QLabel#miniTime { color: @MUTED@; font-size: 11px; }
QLabel#miniTitle {
    color: @TEXT@; font-size: 12px; font-weight: 600;
    background: transparent; border: none;
}

/* Video area. Pure black, NOT kBg (#0F0F0F): it is the background of the
   letterbox bands when the video does not fill the box, and it has to be truly
   black so that the frame is not noticeable. It was in an inline setStyleSheet. */
QWidget#playerArea { background-color: #000000; }
)QSS",
    };

    QString qss;
    qss.reserve(30627);
    for (const char *t : kTrozos)
        qss += QString::fromUtf8(t);

    // The placeholders are substituted on EVERY call: a hot accent change
    // (Settings → Color de acento) regenerates the stylesheet right away.
    // The neutral ones are placeholders too: the QSS used to write #2A2A2A for
    // borders and #232323 for hover while theme.h had kBorder #262626 and
    // kHover #242424, so the same role had two values depending on where it was
    // painted from. Now there is a single source.
    return qss.replace(QStringLiteral("@ACCENT@"), accentHex())
              .replace(QStringLiteral("@TOOLTIP_DECLS@"), tooltipDeclarations())
              .replace(QStringLiteral("@ACCENT_PRESSED@"), accentPressedHex())
              .replace(QStringLiteral("@ACCENT_HOVER@"), accentHoverHex())
              .replace(QStringLiteral("@ACCENT_A10@"), accentAlpha(26))
              .replace(QStringLiteral("@ACCENT_A16@"), accentAlpha(41))
              .replace(QStringLiteral("@ACCENT_A25@"), accentAlpha(64))
              .replace(QStringLiteral("@ACCENT_A30@"), accentAlpha(77))
              .replace(QStringLiteral("@ACCENT_MUTED@"),
                       accentPressed(kAccent).darker(160).name())
              .replace(QStringLiteral("@DISABLED_ON_ACCENT@"),
                       QColor(0xFF, 0xFF, 0xFF).name())
              .replace(QStringLiteral("@BORDER@"), kBorder.name())
              .replace(QStringLiteral("@HOVER@"), kHover.name())
              .replace(QStringLiteral("@DISABLED@"), kDisabled.name())
              .replace(QStringLiteral("@SELECTION@"), kSelection.name())
              .replace(QStringLiteral("@HANDLE@"), kHandle.name())
              .replace(QStringLiteral("@HANDLE_HOVER@"), kHandleHover.name())
              .replace(QStringLiteral("@POPOVER@"), kPopover.name())
              .replace(QStringLiteral("@MUTED@"), kMuted.name())
              .replace(QStringLiteral("@TEXT@"), kText.name())
              .replace(QStringLiteral("@METATEXT@"), kMetaText.name())
              .replace(QStringLiteral("@TEXTSOFT@"), kTextSoft.name())
              .replace(QStringLiteral("@OVERLAY_BG@"), overlayBgRgba())
              .replace(QStringLiteral("@OVERLAY_EDGE@"), overlayEdgeRgba());
}

bool themeSelfTest()
{
    int fail = 0;
    const auto chk = [&fail](bool ok, const char *what) {
        std::fprintf(stderr, "  [%s] %s\n", ok ? "OK  " : "FAIL", what);
        if (!ok)
            ++fail;
    };

    QString qss = styleSheet();
    // Comments stripped: a /* ... */ can contain braces or selector
    // names and would pollute the count.
    while (true) {
        const int a = qss.indexOf(QStringLiteral("/*"));
        if (a < 0)
            break;
        const int b = qss.indexOf(QStringLiteral("*/"), a + 2);
        if (b < 0) {
            qss.remove(a, qss.size() - a);
            break;
        }
        qss.remove(a, b + 2 - a);
    }

    QHash<QString, int> veces;
    // selector -> body, to be able to check the content of a concrete rule
    // (not only that the selector exists).
    QVector<QPair<QString, QString>> cuerpos;
    int reglas = 0;
    int i = 0;
    while (i < qss.size()) {
        const int abre = qss.indexOf(QLatin1Char('{'), i);
        if (abre < 0)
            break;
        const int cierra = qss.indexOf(QLatin1Char('}'), abre + 1);
        if (cierra < 0)
            break;
        const QString sel = qss.mid(i, abre - i).simplified();
        if (!sel.isEmpty()) {
            ++reglas;
            veces[sel] += 1;
            cuerpos.append(qMakePair(sel, qss.mid(abre + 1, cierra - abre - 1)));
        }
        i = cierra + 1;
    }

    // If the parser breaks (e.g. because of a QSS formatting change) the number
    // of rules collapses and the two checks below would pass on empty.
    chk(reglas > 100 && veces.size() > 50,
        "el QSS se ha parseado (mas de 100 reglas y 50 selectores)");

    QStringList repetidos;
    for (auto it = veces.constBegin(); it != veces.constEnd(); ++it)
        if (it.value() > 1)
            repetidos << it.key();
    repetidos.sort();
    for (const QString &r : repetidos)
        std::fprintf(stderr, "    [info] selector repetido en el QSS: %s\n",
                     qPrintable(r));
    chk(repetidos.isEmpty(),
        "ningun selector se declara dos veces (el ultimo gana y no avisa)");

    // Guard for the concrete regression: #sectionTitle is 16 px by design (the
    // comment in settings_page.cpp says so) and may only be declared once.
    // It was duplicated at 22 px and raised the section titles of ALL the
    // pages (Home, Descubre, Buscar, canal, historial, biblioteca, playlist).
    chk(veces.value(QStringLiteral("QLabel#sectionTitle")) == 1,
        "QLabel#sectionTitle se declara exactamente una vez");

    // --- #name selectors no widget ever reaches -------------------------------
    // A rule whose #name does not appear as a literal ANYWHERE in src/
    // cannot paint anything: nobody gives the widget that objectName. So it
    // painted and went unnoticed. That is what would have caught QPushButton#danger.
    //
    // Literals of ALL of src/ are looked at, not only the arguments of
    // setObjectName: there are helpers that receive the name as a parameter and apply
    // it internally, and with the strict criterion #actionSegL and #actionSegR
    // came out as dead without being so (makeSeg sets them in watch_page.cpp). With
    // a lax criterion the opposite case can happen (the same name really being cited
    // elsewhere and covering up a dead selector), but that is a false
    // negative: it warns in CI and someone looks. A false positive instead
    // teaches you to ignore the test, which is worse.
    QStringList nombresQss;
    for (const QString &sel : veces.keys()) {
        for (int i = 0; i < sel.size(); ++i) {
            if (sel.at(i) != QLatin1Char('#'))
                continue;
            int j = i + 1;
            QString n;
            while (j < sel.size() && (sel.at(j).isLetterOrNumber()
                                      || sel.at(j) == QLatin1Char('_'))) {
                n += sel.at(j);
                ++j;
            }
            // Qt ids start with a letter; "#1" is not an object selector.
            if (!n.isEmpty() && n.at(0).isLetter() && !nombresQss.contains(n))
                nombresQss << n;
            i = j - 1;
        }
    }
    nombresQss.sort();
    chk(nombresQss.size() > 40, "se han extraido los ids del QSS (mas de 40)");

    {
        // Source tree root: this file is <root>/src/ui/theme.cpp.
        QDir root = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath();
        root.cdUp();
        root.cdUp();
        const QString srcDir = root.filePath(QStringLiteral("src"));
        if (!QFileInfo(srcDir).isDir()) {
            std::fprintf(stderr,
                         "  [info] sin arbol de fuentes junto al binario: no se "
                         "comprueban los selectores sin widget\n");
        } else {
            QStringList literales;
            QStringList pila{srcDir};
            int ficheros = 0;
            while (!pila.isEmpty()) {
                const QString dir = pila.takeLast();
                const QFileInfoList entradas = QDir(dir).entryInfoList(
                    QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
                for (const QFileInfo &fi : entradas) {
                    if (fi.isDir()) {
                        pila << fi.filePath();
                        continue;
                    }
                    const QString sufijo = fi.suffix();
                    if (sufijo != QLatin1String("cpp") && sufijo != QLatin1String("h"))
                        continue;
                    QFile f(fi.filePath());
                    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
                        continue;
                    ++ficheros;
                    // The block state does NOT cross from one file to the next: an unclosed
                    // "/*" would leave the whole next file marked as
                    // comment and its literals unseen (a silent
                    // hole, which is exactly what this test avoids).
                    bool enBloque = false;
                    const QStringList lineas =
                        QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
                    for (const QString &ln : lineas)
                        literales += literalesDe(sinComentarios(ln, enBloque));
                }
            }
            chk(ficheros > 20 && literales.size() > 1000,
                "se ha recorrido el arbol de fuentes (literales incluidos)");
            QStringList muertos;
            for (const QString &n : nombresQss)
                if (!literales.contains(n))
                    muertos << n;
            for (const QString &n : muertos)
                std::fprintf(stderr,
                             "  [info] selector sin widget: #%s  (el literal \"%s\" "
                             "no aparece en src/)\n", qPrintable(n), qPrintable(n));
            chk(muertos.isEmpty(),
                "ningun selector #nombre se queda sin widget");
        }
    }

    // Regression of the two things the user stopped noticing. The QSS does not
    // cover it (the Player description is a selectable QLabel and paints with
    // QPalette::Highlight), so without this the selected text came out in the
    // desktop theme's color.
    QPalette pal;
    applyTextSelectionPalette(pal);
    chk(pal.color(QPalette::Highlight) == textSelectionBg(),
        "la seleccion de texto va en el gris de la app (kSelection)");
    chk(pal.color(QPalette::HighlightedText) == textSelectionFg(),
        "el texto seleccionado se queda en kText (11:1 sobre kSelection)");
    chk(qss.contains(QStringLiteral("selection-background-color: ")
                     + kSelection.name()),
        "el QSS selecciona texto en gris, no en otro color");
    chk(!qss.contains(QStringLiteral("selection-background-color: ")
                      + accentHex()),
        "ningun texto seleccionado se pinta en el acento (parecia un error)");

    // The suggestion row highlighted by the keyboard has to CARRY the accent:
    // it was lost because it was #1F1F1F, darker than the #202020 popover.
    const int hl = qss.indexOf(QStringLiteral("QWidget#suggestRow[highlight=\"true\"]"));
    chk(hl > 0, "existe la regla de la sugerencia resaltada");
    if (hl > 0) {
        const int a = qss.indexOf(QLatin1Char('{'), hl);
        const int b = qss.indexOf(QLatin1Char('}'), a);
        const QString bloque = qss.mid(a, b - a);
        chk(bloque.contains(accentHex()),
            "la sugerencia resaltada lleva el acento (barra y velo)");
    }

    // USER RULE: media buttons carry no visible frame (the
    // #iconBtn of the player row and of the mini bar, the #actionChip and
    // the resolution menu items). "none" or "transparent" is accepted:
    // the latter keeps the 2 px so that focus, which this app signals by
    // painting the border color, does not displace the content.
    const QStringList multimedia{
        QStringLiteral("QPushButton#iconBtn"),
        QStringLiteral("QPushButton#actionChip"),
        QStringLiteral("QFrame#qualityMenu QPushButton")};
    for (const QString &sel : multimedia) {
        // ALL states of the button, not just the base rule: the frame when
        // pressing came from :focus (which also inherits the global
        // QPushButton:focus), and a check only on the base let it through.
        int estados = 0;
        QString culpable;
        for (const auto &par : cuerpos) {
            if (!par.first.startsWith(sel))
                continue;
            // Whatever comes after the name must be a pseudo-state or
            // a property, never another name: "#iconBtn" is not "#iconBtnFoo".
            const QChar sig = par.first.size() > sel.size()
                                  ? par.first.at(sel.size())
                                  : QChar();
            if (!sig.isNull() && sig != QLatin1Char(':') && sig != QLatin1Char('[')
                && !sig.isSpace())
                continue;
            ++estados;
            const QString &cuerpo = par.second;
            const bool transparente =
                cuerpo.contains(QStringLiteral("border: none"))
                || cuerpo.contains(QStringLiteral("border: 1px solid transparent"))
                || cuerpo.contains(QStringLiteral("border-color: transparent"));
            if (!cuerpo.contains(QStringLiteral("border")) || transparente)
                continue;
            culpable = par.first;
        }
        chk(estados > 0, qPrintable(QStringLiteral("existe la regla %1").arg(sel)));
        chk(culpable.isEmpty(),
            qPrintable(QStringLiteral("%1 no declara un borde visible en ningun "
                                      "estado (marco en: %2)")
                           .arg(sel,
                                culpable.isEmpty() ? QStringLiteral("-") : culpable)));
    }

    // The Channel tabs: they cannot leave the selected one unmarked (the
    // QTabWidget had no rule at all and looked like the desktop), and focus
    // must not light up an accent ring when pressing them.
    {
        const QString sel = QStringLiteral("QPushButton#tabBtn");
        chk(veces.value(sel) == 1, "QPushButton#tabBtn se declara exactamente una vez");
        const int act = qss.indexOf(sel + QStringLiteral("[selected=\"true\"]"));
        chk(act > 0, "existe la regla de la pestana seleccionada del canal");
        if (act > 0) {
            const int a = qss.indexOf(QLatin1Char('{'), act);
            const int b = qss.indexOf(QLatin1Char('}'), a);
            chk(qss.mid(a, b - a).contains(accentHex().toLower())
                    || qss.mid(a, b - a).contains(accentHoverHex()),
                "la pestana seleccionada del canal lleva el acento");
        }
        const int foc = qss.indexOf(sel + QStringLiteral(":focus"));
        chk(foc > 0, "las pestanas del canal anulan el :focus");
        if (foc > 0) {
            const int a = qss.indexOf(QLatin1Char('{'), foc);
            const int b = qss.indexOf(QLatin1Char('}'), a);
            chk(qss.mid(a, b - a).contains(QStringLiteral("transparent")),
                "el :focus de las pestanas del canal no pinta marco");
        }
    }

    // The control bar of the player carries its OWN copy of the tooltip rule,
    // because the tooltip windows under it do not receive the global sheet and
    // came out with a transparent background (measured: alpha 0 against 255
    // everywhere else). If the two copies drift, the tooltips of the bar change
    // colour silently. These checks tie them together and pin the opaque value.
    chk(qss.contains(QStringLiteral("QToolTip"))
            && qss.contains(tooltipDeclarations()),
        "la hoja global trae la regla del tooltip con las declaraciones compartidas");
    chk(tooltipRule() == QStringLiteral("QToolTip { %1 }").arg(tooltipDeclarations()),
        "tooltipRule() es la regla completa, con selector");
    // The opacity is what the bug was about, and QColor::name() drops the alpha, so
    // the token is checked as a value and not as text: a translucent kPopover
    // would put back exactly the bug this guard exists for.
    chk(kPopover.alpha() == 255, "kPopover es opaco (alpha 255)");
    chk(tooltipDeclarations().contains(kPopover.name()),
        "la regla del tooltip usa kPopover (opaco)");
    chk(!tooltipDeclarations().contains(QStringLiteral("rgba")),
        "la regla del tooltip no usa rgba (seria transparente otra vez)");

    std::fprintf(stderr, "THEME %s (%d reglas, %d selectores)\n",
                 fail == 0 ? "OK" : "FAIL", reglas, (int)veces.size());
    return fail == 0;
}

// OFFLINE GLYPHS selftest. The UI paints its text with the system default
// font, and that font does not have all characters: "✕" (U+2715),
// "◀" (U+25C0), "▾" (U+25BE) and "✔" (U+2714) are NOT in Liberation Sans, the
// one the system resolves. Qt replaces them with the missing-glyph marker,
// which measured on screen is 2x5 px: the search clear button was an
// empty frame with a dot, and the Settings dropdowns likewise.
//
// So that it does not depend on a list that goes stale when text is added, the
// literals of the SOURCE TREE itself are walked and every non-ASCII
// character is checked against the app's real font: a glyph that does not exist
// shows up in CI instead of on the user's screen. Offline and deterministic.
bool glyphSelfTest()
{
    int fail = 0;
    const auto chk = [&fail](bool ok, const char *what) {
        std::fprintf(stderr, "  [%s] %s\n", ok ? "OK  " : "FAIL", what);
        if (!ok)
            ++fail;
    };

    const QFont appFont = QGuiApplication::font();
    const QFontMetrics fm(appFont);
    std::fprintf(stderr, "GLYFTS fuente de la app: %s (%d px)\n",
                 qPrintable(appFont.family()), appFont.pixelSize());

    // WARNING: QFontMetrics::inFont() is NOT good for this, it consults the system
    // FALLBACK and returns true if ANY installed font has the character (here there are 25
    // covering U+2715), while the app kept painting the missing-glyph
    // marker. What decides is whether the FONT FILE of the app
    // font has it, and that is answered by QRawFont. If the font cannot be opened (a
    // composite system font, for instance), it falls back to inFont and warns: it is
    // more permissive, but better than checking nothing.
    const QRawFont raw = QRawFont::fromFont(appFont);
    const bool usarRaw = raw.isValid();
    if (!usarRaw)
        std::fprintf(stderr,
                     "GLYFTS (aviso: no se pudo abrir el fichero de la fuente; "
                     "se usa inFont, que consulta el fallback)\n");
    const auto tieneGlifo = [&fm, &raw, usarRaw](QChar c) {
        return usarRaw ? raw.supportsCharacter(c.unicode()) : fm.inFont(c);
    };

    // Root of the tree: this file is <root>/src/ui/theme.cpp.
    QDir root = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath();
    root.cdUp();
    root.cdUp();
    const QString srcDir = root.filePath(QStringLiteral("src"));
    if (!QFileInfo(srcDir).isDir()) {
        std::fprintf(stderr,
                     "GLYFTS (sin arbol de fuentes junto al binario: omitido)\n");
        return true;
    }

    // char -> first use ("file:line"), for diagnostics.
    QHash<uint, QString> usos;
    int ficheros = 0;
    int literales = 0;
    QStringList pila{srcDir};
    bool enBloque = false;
    while (!pila.isEmpty()) {
        const QString dir = pila.takeLast();
        QFileInfoList entradas =
            QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                    QDir::Name);
        for (const QFileInfo &fi : entradas) {
            if (fi.isDir()) {
                pila << fi.filePath();
                continue;
            }
            const QString sufijo = fi.suffix();
            if (sufijo != QLatin1String("cpp") && sufijo != QLatin1String("h"))
                continue;
            QFile f(fi.filePath());
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            ++ficheros;
            // The block-comment state must NOT carry from one file to
            // the next: an unclosed "/*" at the end would leave the following one entirely
            // "in block" and its literals would not be checked (a silent
            // hole, which is exactly what this test avoids).
            enBloque = false;
            const QStringList lineas =
                QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
            for (int ln = 0; ln < lineas.size(); ++ln) {
                const QString codigo = sinComentarios(lineas.at(ln), enBloque);
                const QStringList lits = literalesDe(codigo);
                literales += lits.size();
                for (const QString &lit : lits) {
                    if (lit.contains(QLatin1String("\\u"))
                        || lit.contains(QLatin1String("\\x")))
                        continue; // explicit codepoint: cannot be checked
                    for (const QChar &c : lit) {
                        if (c.unicode() < 128)
                            continue;
                        if (!usos.contains(c.unicode())) {
                            usos.insert(c.unicode(),
                                        QStringLiteral("%1:%2")
                                            .arg(fi.filePath())
                                            .arg(ln + 1));
                        }
                    }
                }
            }
        }
    }
    chk(ficheros > 20 && literales > 200,
        "se ha recorrido el arbol de fuentes (literales incluidos)");
    chk(!usos.isEmpty(), "hay caracteres no ASCII que comprobar");

    int ausentes = 0;
    QList<uint> claves = usos.keys();
    std::sort(claves.begin(), claves.end());
    for (uint cp : claves) {
        const QChar c = QChar(ushort(cp));
        if (tieneGlifo(c))
            continue;
        ++ausentes;
        std::fprintf(stderr, "  [FAIL] U+%04X %s  NO esta en %s (primer uso: %s)\n",
                     cp, qPrintable(QString(c)), qPrintable(appFont.family()),
                     qPrintable(usos.value(cp)));
    }
    chk(ausentes == 0, "todo glifo no ASCII de la UI existe en la fuente");

    std::fprintf(stderr, "GLYFTS %s (%d ficheros, %d literales, %d glifos)\n",
                 fail == 0 ? "OK" : "FAIL", ficheros, literales, (int)usos.size());
    return fail == 0;
}

} // namespace nt
