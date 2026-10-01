#pragma once

#include <QString>

#include <functional>

class QMenu;
class PlaylistsRepo;
class WatchLaterRepo;

struct VideoInfo;

namespace nt {

// Acciones estándar del botón de opciones "⋯" de una fila de vídeo. Las usan
// TODAS las superficies con fila (Búsqueda, Canal, Playlist, Historial,
// Biblioteca y "A continuación"), para que el mismo vídeo ofrezca las mismas
// opciones en todas partes y el menú no se escriba una vez por página.
//
// Lo que la superficie aporta son las tres piezas que son SUyas: el popup de
// "Nueva playlist" (cada uno tiene el suyo), la navegación al canal (la resuelve
// la ventana) y el refresco tras escribir en la BD (Biblioteca recarga su
// sección). Vacía = la acción no aparece.
struct RowMenuHooks {
    std::function<void(const QString &videoId)> onNewPlaylist;
    std::function<void(const QString &channelId,
                       const QString &channelName)> onChannel;
    std::function<void()> onChanged;
};

// Añade al menú las acciones estándar del vídeo: ver más tarde, añadir a
// playlist (con "+ Nueva playlist"), compartir y ver canal. No mete separador
// inicial ni final: cada superficie antepone su acción propia ("Quitar del
// historial", "Quitar de la cola"…) y decide dónde cortar.
//
// withWatchLater=false omite la acción de ver más tarde, para la superficie que
// ya ofrece su propia versión de ella (la sección "Ver más tarde" de Biblioteca,
// que la pone como "Quitar de Ver más tarde").
void addRowMenuActions(QMenu *menu, const VideoInfo &video,
                       WatchLaterRepo *watchLater, PlaylistsRepo *playlists,
                       const RowMenuHooks &hooks = {},
                       bool withWatchLater = true);

} // namespace nt
