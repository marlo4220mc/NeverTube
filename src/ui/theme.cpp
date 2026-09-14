#include "theme.h"

namespace nt {

QString styleSheet()
{
    return QStringLiteral(R"QSS(
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
    border-top: 1px solid #262626;
}
QPushButton#navBtn {
    background: transparent;
    border: none;
    border-radius: 8px;
    padding: 6px 0;
    margin: 4px 4px;
    color: #8F8F8F;
    font-size: 11px;
}
QPushButton#navBtn:hover { background-color: #1F1F1F; color: #ECECEC; }
QPushButton#navBtn:checked { color: #FF0033; }
QPushButton#navBtn:checked:hover { background-color: #1F1F1F; color: #FF0033; }

QToolButton#navBtn {
    background: transparent;
    border: none;
    border-radius: 8px;
    padding: 4px 0;
    margin: 4px 4px;
    color: #8F8F8F;
    font-size: 11px;
}
QToolButton#navBtn:hover { background-color: #1F1F1F; color: #ECECEC; }
QToolButton#navBtn:checked { color: #FF0033; }
QToolButton#navBtn:checked:hover { background-color: #1F1F1F; color: #FF0033; }

QLabel { background: transparent; }
QLabel#sectionTitle { font-size: 16px; font-weight: 600; color: #ECECEC; }
QLabel#sectionSub { font-size: 12px; color: #8F8F8F; }
QLabel#muted { color: #8F8F8F; }
QLabel#chName { font-size: 12px; color: #ECECEC; line-height: 1.2; }
QLabel#chAvatar { background: transparent; }

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
QPushButton#primary { background-color: #FF0033; border-color: #FF0033; color: #FFFFFF; font-weight: 600; }
QPushButton#primary:hover { background-color: #E0002E; }
QPushButton#ghost { background: transparent; border: 1px solid #2A2A2A; border-radius: 6px; }
QPushButton#ghost:hover { background: #1F1F1F; }
QPushButton#iconBtn { background: transparent; border: none; border-radius: 6px; padding: 6px; }
QPushButton#iconBtn:hover { background: #232323; }
QPushButton#danger { color: #FF0033; }

QLineEdit, QComboBox {
    background-color: #1F1F1F;
    border: 1px solid #2A2A2A;
    border-radius: 10px;
    padding: 8px 12px;
    selection-background-color: #FF0033;
    color: #ECECEC;
}
QLineEdit:focus, QComboBox:focus { border-color: #FF0033; }
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
QSlider::sub-page:horizontal { background: #FF0033; border-radius: 2px; }
QSlider::handle:horizontal {
    width: 12px; height: 12px; margin: -4px 0;
    background: #FFFFFF; border-radius: 6px;
}
QSlider::handle:horizontal:hover { background: #FF0033; }

QProgressBar { border: none; background: #2A2A2A; border-radius: 3px; text-align: center; color: transparent; }
QProgressBar::chunk { background: #FF0033; border-radius: 3px; }

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
}

} // namespace nt