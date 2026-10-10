#pragma once

#include <QString>

#include <functional>

class QMenu;
class PlaylistsRepo;
class WatchLaterRepo;

struct VideoInfo;

namespace nt {

// Standard actions of the "⋯" options button on a video row. Used by EVERY
// surface with rows (Search, Channel, Playlist, History, Library and
// "A continuación"), so the same video offers the same options everywhere and
// the menu is not written once per page.
//
// What the surface supplies are the three pieces that are ITS OWN: the "New
// playlist" popup (each one has its own), channel navigation (resolved by the
// window) and the refresh after writing to the DB (Library reloads its
// section). Empty = the action does not appear.
struct RowMenuHooks {
    std::function<void(const QString &videoId)> onNewPlaylist;
    std::function<void(const QString &channelId,
                       const QString &channelName)> onChannel;
    std::function<void()> onChanged;
};

// Adds the standard video actions to the menu: watch later, add to playlist
// (with "+ New playlist"), share and view channel. It does not insert a leading
// or trailing separator: each surface prepends its own action ("Remove from
// history", "Remove from queue"…) and decides where to cut.
//
// withWatchLater=false skips the watch-later action, for the surface that
// already offers its own version of it (the "Watch later" section of Library,
// which labels it "Remove from Watch later").
void addRowMenuActions(QMenu *menu, const VideoInfo &video,
                       WatchLaterRepo *watchLater, PlaylistsRepo *playlists,
                       const RowMenuHooks &hooks = {},
                       bool withWatchLater = true);

} // namespace nt
