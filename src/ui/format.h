#pragma once

#include <QString>

struct VideoInfo;

QString fmtTime(double seconds);
QString durationLabel(int seconds);
QString compactCount(qint64 count);
QString publishedLabel(const QString &publishedAt);
QString viewsLabel(qint64 count);

// Línea de meta de un vídeo: "canal  •  N vistas  •  hace N d", con lo que
// haya (los tres campos no siempre están: un vídeo de la caché puede no
// tener fecha, una búsqueda puede no traer visitas). Es la MISMA en todas las
// filas de la app (rejilla del Home, listas de Búsqueda/Canal/Playlist/
// Historial/"A continuación" y las secciones de Biblioteca), así que el estilo
// no depende de la página: una sola función, no una copia por superficie.
QString metaLine(const VideoInfo &v);