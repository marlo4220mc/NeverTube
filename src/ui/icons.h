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

// App logo (the icon from the .qrc) scaled with smoothing, for brand surfaces
// (empty states, Home). Returns a null pixmap if the resource is missing, so
// the caller can skip it.
QPixmap appLogo(int size);

// Rich text for the empty-state boxes: the app logo above the text. QLabel
// detects the HTML and paints the .qrc image; the color and alignment are
// still set by the widget's QSS.
QString emptyStateHtml(const QString &text);

// Circular avatar: scales the image to the requested size, crops the center
// and masks it with a circle. The channel gate (Channel page card and
// Library/Search card) is a QLabel with a `border-radius` in the QSS, and that
// only rounds the BACKGROUND: the pixmap was painted square and additionally
// stretched with setScaledContents. With this the circle is real and does not
// distort the avatar. Returns a null pixmap if the input is unusable, so the
// user icon can be painted instead.
QPixmap circularAvatar(const QPixmap &src, int size);

} // namespace nt