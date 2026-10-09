#pragma once

#include <QIcon>

class QColor;
namespace nt {

// Iconos del reproductor y del mini player. La geometría es de Material
// Symbols (google/material-design-icons, Apache-2.0) y se dibuja con QPainter,
// como el resto de la app: vectorial, nítida a cualquier tamaño, recoloreable
// en caliente (acento) sin retint de bitmaps y sin meter PNGs en el .qrc ni
// enlazar Qt5::Svg (que añadiría Qt5Svg.dll a los dos empaquetados de Windows).
//
// El transporte (Play/Pause/Prev/Next) usa la variante rellena de Material,
// como el reproductor de YouTube; el resto van en contorno.
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

// Devuelve el icono pintado en `color` a `size` px. Replica el contrato de
// nt::icon(); el resultado se cachea por (icono, tamaño, color).
QIcon playerIcon(PlayerIcon kind, const QColor &color, int size = 20);

} // namespace nt
