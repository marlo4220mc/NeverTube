#pragma once

#include <QIcon>

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

// Avatar circular: escala la imagen al lado pedido, recorta el centro y la
// enmascara con un círculo. La puerta de canal (ficha de Canal y tarjeta de
// Biblioteca/Search) es un QLabel con un `border-radius` en el QSS, y eso solo
// redondea el FONDO: el pixmap se pintaba cuadrado y además estirado con
// setScaledContents. Con esto el círculo es real y no deforma el avatar.
// Devuelve un pixmap nulo si la entrada no sirve, para poder pintar el icono
// de usuario en su lugar.
QPixmap circularAvatar(const QPixmap &src, int size);

} // namespace nt