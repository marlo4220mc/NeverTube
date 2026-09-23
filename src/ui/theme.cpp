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
QWidget#shellPanel { background-color: #101010; border-right: 1px solid #262626; }
QWidget#contentHost { background-color: #0F0F0F; }

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
    padding: 4px 0;
color: #8F8F8F;
    font-size: 12px;
}
QToolButton#navBtn:hover { color: #ECECEC; }
QToolButton#navBtn:pressed { color: @ACCENT_PRESSED@; }
QToolButton#navBtn:checked { color: @ACCENT@; }
QToolButton#navBtn:checked:hover { color: @ACCENT@; }
QToolButton#navBtn:disabled { color: #6a6a6a; }

QLabel { background: transparent; }
QLabel#sectionTitle { font-size: 16px; font-weight: 600; color: #ECECEC; }
QLabel#sectionSub { font-size: 12px; color: #8F8F8F; }
QLabel#muted { color: #8F8F8F; }

QPushButton {
    background-color: #1F1F1F;
    border: 1px solid #2A2A2A;
    border-radius: 8px;
    padding: 7px 14px;
    color: #ECECEC;
}
QPushButton:hover { background-color: #2A2A2A; }
QPushButton:pressed { background-color: #232323; }
QPushButton:disabled { color: #6a6a6a; }
QPushButton#primary { background-color: @ACCENT@; border-color: @ACCENT@; color: #FFFFFF; font-weight: 600; }
QPushButton#primary:hover { background-color: @ACCENT_PRESSED@; }
QPushButton#ghost { background: transparent; border: 1px solid #2A2A2A; border-radius: 6px; }
QPushButton#ghost:hover { background: #1F1F1F; }
QPushButton#iconBtn { background: transparent; border: none; border-radius: 6px; padding: 6px; }
QPushButton#iconBtn:hover { background: #232323; }
QPushButton#danger { color: @ACCENT@; }

QLineEdit, QComboBox {
    background-color: #1F1F1F;
    border: 1px solid #2A2A2A;
    border-radius: 10px;
    padding: 8px 12px;
    selection-background-color: @ACCENT@;
    color: #ECECEC;
}
QLineEdit:focus, QComboBox:focus { border-color: @ACCENT@; }

/* Popover no modal (p. ej. crear playlist): panel flotante que no bloquea
   el resto de la página. */
QFrame#popupDialog {
    background-color: #202020;
    border: 1px solid #2A2A2A;
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
    border: 1px solid #2A2A2A;
    selection-background-color: #2D2D2D;
    color: #ECECEC;
    outline: 0;
}

QScrollArea { border: none; background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #2F2F2F; border-radius: 5px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: #3A3A3A; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle:horizontal { background: #2F2F2F; border-radius: 5px; min-width: 24px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical,
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }

QSlider::groove:horizontal { height: 4px; background: #2F2F2F; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @ACCENT@; border-radius: 2px; }
QSlider::handle:horizontal {
    width: 12px; height: 12px; margin: -4px 0;
    background: #FFFFFF; border-radius: 6px;
}
QSlider::handle:horizontal:hover { background: @ACCENT@; }

QProgressBar { border: none; background: #2A2A2A; border-radius: 3px; text-align: center; color: transparent; }
QProgressBar::chunk { background: @ACCENT@; border-radius: 3px; }

QToolButton { background: transparent; border: none; border-radius: 6px; padding: 5px; }
QToolButton:hover { background: #232323; }

QListWidget#cards {
    background: transparent;
    border: none;
    outline: 0;
}
QListWidget#cards::item { background: transparent; border: none; }
QListWidget#cards::item:selected { background: transparent; }

QMenu { background-color: #1F1F1F; border: 1px solid #2A2A2A; }
QMenu::item { padding: 7px 24px; }
QMenu::item:selected { background-color: #2D2D2D; }

QStatusBar { background: #101010; color: #8F8F8F; border-top: 1px solid #1F1F1F; }

QWidget#miniBar { background-color: #181818; border-top: 1px solid #262626; }
QWidget#watchBody { background-color: #0F0F0F; }
QWidget#cardPanel { background-color: #181818; border: 1px solid #262626; border-radius: 10px; }
)QSS");
    // Los placeholders se sustituyen en CADA llamada: un cambio de acento en
    // caliente (Ajustes → Color de acento) regenera el stylesheet al momento.
    return qss.replace(QStringLiteral("@ACCENT@"), accentHex())
              .replace(QStringLiteral("@ACCENT_PRESSED@"), accentPressedHex());
}

} // namespace nt