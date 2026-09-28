#include "theme.h"

#include <QColor>

namespace nt {

// Fuente única de la paleta de acentos (labels de Ajustes, hex persistido y
// sufijo de los PNG pre-pintados de la navegación). El defecto es el rojo
// NeverTube histórico (#FF0033). Mantener en sync con tools/gen_nav_accent.cpp
// solo en el sentido de que regenerar los PNG tras cambiar AQUÍ es obligatorio.
QVector<AccentOption> accentOptions()
{
    return {
        {QStringLiteral("Rojo (por defecto)"), QStringLiteral("#FF0033"),
         QStringLiteral("rojo")},
        {QStringLiteral("Azul"), QStringLiteral("#1E88E5"),
         QStringLiteral("azul")},
        {QStringLiteral("Verde"), QStringLiteral("#22C55E"),
         QStringLiteral("verde")},
        {QStringLiteral("Naranja"), QStringLiteral("#F97316"),
         QStringLiteral("naranja")},
        {QStringLiteral("Púrpura"), QStringLiteral("#A855F7"),
         QStringLiteral("purpura")},
        {QStringLiteral("Cian"), QStringLiteral("#22D3EE"),
         QStringLiteral("cian")},
        {QStringLiteral("Rosa"), QStringLiteral("#EC4899"),
         QStringLiteral("rosa")},
    };
}

QString accentSuffix()
{
    const QString cur = kAccent.name(); // "#rrggbb" minúscula
    const auto opts = accentOptions();
    for (const AccentOption &o : opts)
        if (QColor(o.hex).name() == cur)
            return o.suffix;
    return opts.value(0).suffix;
}

QString styleSheet()
{
    QString qss = QStringLiteral(R"QSS(
QWidget {
    background-color: transparent;
    color: #ECECEC;
    font-size: 13px;
}
QMainWindow { background-color: #0F0F0F; }

/* (Sin reglas para shellPanel/contentHost: ningún setObjectName del código las
   usaba, eran restos del layout split revertido. Verificado por grep.) */

QWidget#navBottom {
    background-color: #101010;
}

/* Botones de la barra de navegación inferior. Los iconos (PNG) los gestiona
   NavButton por estado; aquí solo se colorean texto y fondo. El acento
   (rojo NeverTube por defecto) aparece únicamente en estados activos o de
   interacción. */
QToolButton#navBtn {
    background: transparent;
    border: none;
    border-radius: 8px;
    /* 2px (no 4): el alto de la barra es 60 (mainwindow.cpp setupNav) y el
       contenido icono 33 + hueco + línea de texto 12px necesita ~53; con 4px
       por arriba y por abajo solo caben 52 y el texto se recortaría. */
    padding: 2px 0;
color: #8F8F8F;
    font-size: 12px;
}
QToolButton#navBtn:hover { color: #ECECEC; }
QToolButton#navBtn:pressed { color: @ACCENT_PRESSED@; }
/* La pestaña activa se marca con un relleno translúcido del acento, no solo
   con el color: el color solo daba 4,8:1 contra el fondo y no indicaba posición
   en la barra. El relleno va en el widget (no en el icono) para no tener que
   regenerar los PNG pre-pintados de resources/nav. Sin borde: el alto de 60 px
   de la barra no tiene holgura para una barra de 2 px sin recortar el texto. */
QToolButton#navBtn:checked { color: @ACCENT@; background: @ACCENT_A10@; }
QToolButton#navBtn:checked:hover { color: @ACCENT@; background: @ACCENT_A16@; }
/* Foco por color de fondo, no por borde: navBtn no tiene borde y añadir uno
   (aunque fuese transparente) cambiaría la caja de contenido y el alto útil. */
QToolButton#navBtn:focus { color: @ACCENT@; background: @HOVER@; }
QToolButton#navBtn:disabled { color: @DISABLED@; }

QLabel { background: transparent; }
QLabel#sectionTitle { font-size: 16px; font-weight: 600; color: #ECECEC; }
QLabel#sectionSub { font-size: 12px; color: #8F8F8F; }
QLabel#muted { color: #8F8F8F; }

QPushButton {
    background-color: #1F1F1F;
    border: 1px solid @BORDER@;
    border-radius: 8px;
    padding: 7px 14px;
    color: #ECECEC;
}
QPushButton:hover { background-color: @HOVER@; }
QPushButton:pressed { background-color: @SELECTION@; }
/* Foco por COLOR de borde, no por grosor: el botón ya reserva 1px, así que
   añadir un anillo de 2 px desplazaría el contenido en cada cambio de estado. */
QPushButton:focus { border-color: @ACCENT@; }
/* El botón deshabilitado solo perdía el color: conservaba fondo y borde, así
   que seguía leyéndose como pulsable. Aquí baja emphasis en los tres canales. */
QPushButton:disabled {
    color: @DISABLED@;
    background-color: @BORDER@;
    border-color: @BORDER@;
}
QPushButton#primary { background-color: @ACCENT@; border-color: @ACCENT@; color: #FFFFFF; font-weight: 600; }
/* Faltaba :pressed. Como :hover ya apuntaba a @ACCENT_PRESSED@, hover y
   pressed se veían idénticos y el segundo nunca se veía. Ahora hover va a la
   variante clara del acento y pressed a la oscura. */
QPushButton#primary:hover { background-color: @ACCENT_HOVER@; }
QPushButton#primary:pressed { background-color: @ACCENT_PRESSED@; }
QPushButton#primary:disabled {
    color: @DISABLED_ON_ACCENT@; background-color: @ACCENT_MUTED@;
    border-color: @ACCENT_MUTED@;
}
QPushButton#ghost { background: transparent; border: 1px solid @BORDER@; border-radius: 6px; }
QPushButton#ghost:hover { background: #1F1F1F; }
QPushButton#ghost:pressed { background: @HOVER@; }
QPushButton#ghost:focus { border-color: @ACCENT@; }
QPushButton#iconBtn { background: transparent; border: none; border-radius: 6px; padding: 6px; }
QPushButton#iconBtn:hover { background: @HOVER@; }
QPushButton#iconBtn:pressed { background: @SELECTION@; }
/* iconBtn no tiene borde, así que el foco va por fondo (igual que navBtn). */
QPushButton#iconBtn:focus { background: @HOVER@; color: @ACCENT@; }
QPushButton#iconBtn:disabled { color: @DISABLED@; }
QPushButton#danger { color: @ACCENT@; }

QLineEdit, QComboBox {
    background-color: #1F1F1F;
    border: 1px solid @BORDER@;
    border-radius: 10px;
    padding: 8px 12px;
    selection-background-color: @ACCENT@;
    color: #ECECEC;
}
QLineEdit:focus, QComboBox:focus { border-color: @ACCENT@; }
QLineEdit:disabled, QComboBox:disabled { color: @DISABLED@; background-color: @BORDER@; }

/* Popover no modal (p. ej. crear playlist): panel flotante que no bloquea
   el resto de la página. */
QFrame#popupDialog {
    background-color: @POPOVER@;
    border: 1px solid @BORDER@;
    border-radius: 12px;
}
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

QScrollArea { border: none; background: transparent; }
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

QToolButton { background: transparent; border: none; border-radius: 6px; padding: 5px; }
QToolButton:hover { background: @HOVER@; }
QToolButton:pressed { background: @SELECTION@; }
QToolButton:focus { background: @HOVER@; }

QListWidget#cards {
    background: transparent;
    border: none;
    outline: 0;
}
QListWidget#cards::item { background: transparent; border: none; }
QListWidget#cards::item:selected { background: transparent; }

QMenu { background-color: #1F1F1F; border: 1px solid @BORDER@; }
QMenu::item { padding: 7px 24px; }
QMenu::item:selected { background-color: @SELECTION@; }
QMenu::item:disabled { color: @DISABLED@; }

QStatusBar { background: #101010; color: #8F8F8F; border-top: 1px solid #1F1F1F; }

/* El mini player usaba kPanel (#181818), el mismo que la nav inferior, así que
   las dos barras se fundían en una sola línea. kElevated los separa. */
QWidget#miniBar { background-color: #1F1F1F; border-top: 1px solid @BORDER@; }
QWidget#watchBody { background-color: #0F0F0F; }
QWidget#cardPanel { background-color: #181818; border: 1px solid @BORDER@; border-radius: 10px; }
)QSS");
    // Los placeholders se sustituyen en CADA llamada: un cambio de acento en
    // caliente (Ajustes → Color de acento) regenera el stylesheet al momento.
    // Los neutros también son placeholders: antes el QSS escribía #2A2A2A para
    // bordes y #232323 para hover mientras theme.h tenía kBorder #262626 y
    // kHover #242424, así que un mismo rol tenía dos valores según por dónde se
    // pintara. Ahora hay una sola fuente.
    return qss.replace(QStringLiteral("@ACCENT@"), accentHex())
              .replace(QStringLiteral("@ACCENT_PRESSED@"), accentPressedHex())
              .replace(QStringLiteral("@ACCENT_HOVER@"), accentHoverHex())
              .replace(QStringLiteral("@ACCENT_A10@"), accentAlpha(26))
              .replace(QStringLiteral("@ACCENT_A16@"), accentAlpha(41))
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
              .replace(QStringLiteral("@POPOVER@"), kPopover.name());
}

} // namespace nt