#pragma once

#include <QIcon>

class QColor;
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
    Star,
    StarFilled,
    Thumb,
    Queue,
    Download,
    Refresh,
    Clear,
    User,
    ListAdd,
    QueueList,
    Back,
};

QIcon icon(Icon kind, const QColor &color, int size = 20);

} // namespace nt