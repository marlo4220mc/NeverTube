#pragma once

#include <QIcon>

class QColor;
namespace nt {

// Player and mini player icons. The geometry is Material Symbols
// (google/material-design-icons, Apache-2.0) and it is drawn with QPainter,
// like the rest of the app: vector, sharp at any size, recolorable on the fly
// (accent) without bitmap retint and without putting PNGs in the .qrc nor
// linking Qt5::Svg (which would add Qt5Svg.dll to both Windows packages).
//
// Transport (Play/Pause/Prev/Next) uses Material's filled variant, like the
// YouTube player; the rest are outlined.
enum class PlayerIcon {
    Shuffle,
    Prev,
    Play,
    Pause,
    Next,
    Repeat,
    Volume,
    Muted,
    Captions,
    PlaylistAdd,
    Settings,
    Fullscreen,
    FullscreenExit,
    Close,
};

// Returns the icon painted in `color` at `size` px. Replicates the contract
// of nt::icon(); the result is cached by (icon, size, color).
QIcon playerIcon(PlayerIcon kind, const QColor &color, int size = 20);

} // namespace nt
