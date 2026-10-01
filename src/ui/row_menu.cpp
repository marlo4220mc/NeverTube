#include "row_menu.h"

#include "../db/repositories.h"
#include "../models.h"

#include <QAction>
#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QMenu>
#include <QTimer>
#include <QToolTip>

namespace {

// Añade la acción y la ejecuta en el bucle de eventos siguiente, no dentro del
// triggered(): así el menú ya está cerrado cuando la acción escribe en la BD o
// recarga la sección, que es lo que evita que el popup se quede colgando al
// vaciarse la lista de la que nació.
QAction *addAction(QMenu *menu, const QString &text,
                   std::function<void()> fn)
{
    QAction *act = menu->addAction(text);
    QObject::connect(act, &QAction::triggered, menu,
                     [fn]() { QTimer::singleShot(0, fn); });
    return act;
}

} // namespace

void nt::addRowMenuActions(QMenu *menu, const VideoInfo &video,
                           WatchLaterRepo *watchLater, PlaylistsRepo *playlists,
                           const RowMenuHooks &hooks, bool withWatchLater)
{
    if (!menu || video.id.isEmpty())
        return;

    if (withWatchLater && watchLater) {
        const bool marked = watchLater->isWatchLater(video.id);
        addAction(menu,
                  marked ? QStringLiteral("Quitar de Ver más tarde")
                         : QStringLiteral("Guardar en Ver más tarde"),
                  [watchLater, id = video.id, hooks]() {
                      watchLater->toggle(id);
                      if (hooks.onChanged)
                          hooks.onChanged();
                  });
    }

    if (playlists) {
        QMenu *plMenu = new QMenu(menu);
        plMenu->setTitle(QStringLiteral("Añadir a playlist"));
        for (const PlaylistItem &p : playlists->all()) {
            const bool contains = playlists->contains(p.id, video.id);
            // Sin glifo de check delante: "✔" (U+2714) no está en la fuente por
            // defecto y salía como un punto. El "(ya está)" ya lo dice.
            QAction *a = plMenu->addAction(
                contains ? QStringLiteral("%1 (ya está)").arg(p.title) : p.title);
            a->setEnabled(!contains);
            QObject::connect(a, &QAction::triggered, plMenu,
                             [playlists, pid = p.id, id = video.id, hooks]() {
                                 playlists->addVideo(pid, id);
                                 if (hooks.onChanged)
                                     hooks.onChanged();
                             });
        }
        if (hooks.onNewPlaylist) {
            plMenu->addSeparator();
            QAction *newAct =
                plMenu->addAction(QStringLiteral("+ Nueva playlist"));
            QObject::connect(newAct, &QAction::triggered, plMenu,
                             [hooks, id = video.id]() {
                                 if (hooks.onNewPlaylist)
                                     hooks.onNewPlaylist(id);
                             });
        }
        menu->addMenu(plMenu);
    }

    addAction(menu, QStringLiteral("Compartir"), [id = video.id]() {
        const QString url =
            QStringLiteral("https://www.youtube.com/watch?v=%1").arg(id);
        QGuiApplication::clipboard()->setText(url);
        // Sin widget padre: la acción corre cuando el menú ya está cerrado, y
        // el QMenu de la superficie es un objeto local que se destruye; el
        // tooltip se cuelga del widget con el foco.
        QToolTip::showText(QCursor::pos(), QStringLiteral("Enlace copiado"));
    });

    if (!video.channelId.isEmpty() && hooks.onChannel) {
        addAction(menu, QStringLiteral("Ver canal"),
                  [hooks, id = video.channelId, name = video.channel]() {
                      hooks.onChannel(id, name);
                  });
    }
}
