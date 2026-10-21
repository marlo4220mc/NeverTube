#pragma once

#include <QVector>

struct VideoInfo;

// Un Short (un reel) es un video VERTICAL: su miniatura es mas alta que
// ancha (1080x1920 contra 1280x720 de uno normal).
//
// La app los saca de las listas: un Short no se reproduce bien en el player
// (es un formato vertical pensado para el movil) y no es lo que la persona
// quiere en "A continuación", en "Continuar viendo" ni en "Descubre".
namespace reels {

// ¿La miniatura es vertical? Se mira la MAYOR (la de más pixeles): la
// última del array es la más grande, y es la que trae la proporción real.
//
// Sin datos: false (no se descarta nada por ignorancia).
bool thumbnailIsVertical(int width, int height);

// ¿Es un Short? La señal vertical de la miniatura.
bool isVertical(const VideoInfo &video);

// Saca los Shorts de una lista. Los que quedan mantienen su orden.
QVector<VideoInfo> withoutShorts(const QVector<VideoInfo> &videos);

} // namespace reels
