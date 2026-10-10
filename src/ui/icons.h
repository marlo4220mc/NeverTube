#pragma once

#include <QIcon>
#include <QString>

class QColor;
class QPixmap;
namespace nt {

enum class Icon {
    Home,
    Search,
    Library,
    Settings,
    Play,
    Pause,
    Next,
    Prev,
    Mute,
    Unmute,
    Subtitles,
    Fullscreen,
    Star,
    StarFilled,
    Thumb,
    ThumbDown,
    Share,
    Queue,
    Download,
    Refresh,
    Clear,
    User,
    ListAdd,
    QueueList,
    Back,
    WatchLater,
};

QIcon icon(Icon kind, const QColor &color, int size = 20);

// Logo de la app (el icono del .qrc) escalado con suavizado, para superficies
// de marca (estados vacíos, Home). Devuelve un pixmap nulo si el recurso no
// está, para que el llamante pueda omitirlo.
QPixmap appLogo(int size);

// Texto enriquecido para las cajas de estado vacío: el logo de la app encima
// del texto. QLabel detecta el HTML y pinta la imagen del .qrc; el color y la
// alineación los sigue poniendo el QSS del widget.
QString emptyStateHtml(const QString &text);

// Avatar circular: escala la imagen al lado pedido, recorta el centro y la
// enmascara con un círculo. La puerta de canal (ficha de Canal y tarjeta de
// Biblioteca/Search) es un QLabel con un `border-radius` en el QSS, y eso solo
// redondea el FONDO: el pixmap se pintaba cuadrado y además estirado con
// setScaledContents. Con esto el círculo es real y no deforma el avatar.
// Devuelve un pixmap nulo si la entrada no sirve, para poder pintar el icono
// de usuario en su lugar.
QPixmap circularAvatar(const QPixmap &src, int size);

} // namespace nt