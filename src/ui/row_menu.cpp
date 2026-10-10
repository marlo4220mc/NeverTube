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

// Adds the action and runs it on the next event loop iteration, not inside
// triggered(): that way the menu is already closed when the action writes to
// the DB or reloads the section, which is what keeps the popup from hanging
// around as the list it came from is emptied.
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
            // No check glyph in front: "✔" (U+2714) is not in the default
            // font and showed up as a dot. The "(ya está)" already says it.
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
        // No parent widget: the action runs when the menu is already closed,
        // and the surface's QMenu is a local object that is destroyed; the
        // tooltip hangs off the focused widget.
        QToolTip::showText(QCursor::pos(), QStringLiteral("Enlace copiado"));
    });

    if (!video.channelId.isEmpty() && hooks.onChannel) {
        addAction(menu, QStringLiteral("Ver canal"),
                  [hooks, id = video.channelId, name = video.channel]() {
                      hooks.onChannel(id, name);
                  });
    }
}
