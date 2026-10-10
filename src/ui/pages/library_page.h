#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

#include "../../models.h"

class QLabel;
class QPushButton;
class QVBoxLayout;
class QHBoxLayout;
class QGridLayout;
class QScrollArea;
class QFrame;
class QAction;
class QMenu;
class FavoritesRepo;
class HistoryRepo;
class StarredRepo;
class WatchLaterRepo;
class PlaylistsRepo;
class SubscriptionsRepo;
class ThumbnailCache;
class VideoCacheRepo;
class Yt;
class ChannelRow;

struct PlaylistItem;

class LibraryPage : public QWidget
{
    Q_OBJECT

public:
    explicit LibraryPage(Yt *yt, FavoritesRepo *favorites, StarredRepo *starred,
                         WatchLaterRepo *watchLater, HistoryRepo *history,
                         SubscriptionsRepo *subs, VideoCacheRepo *cache,
                         ThumbnailCache *thumbs, PlaylistsRepo *playlists,
                         QWidget *parent = nullptr);

    void reload();

    // The thumbnails' progress bar is painted by hand with nt::kAccent (the
    // QSS does not reach it), so rows must be repainted when the accent
    // changes. MainWindow calls it along with the other pages' applyAccent().
    void applyAccent();

    // Creates a new playlist and, if a video is passed, adds it to it.
    // MainWindow calls it from the shared row menu (Search, Channel, Playlist):
    // the dialog is nt::inputText, the same one as every surface, so it is
    // reused instead of having one per page.
    void createPlaylistForVideo(const QString &videoId);

signals:
    void channelRequested(const QString &channelId, const QString &channelName);
    void openRequested(const QVector<VideoInfo> &videos, int startIndex);
    void resumeRequested(const QVector<VideoInfo> &videos, int startIndex,
                         qlonglong startPositionMs);
    void playlistRequested(const QString &playlistId, const QString &title);
    void historyAllRequested();

private:
    // A video list section (row with thumbnail, title and menu).
    struct VideoSection
    {
        QWidget *host = nullptr;
        QVBoxLayout *lay = nullptr;
        QLabel *title = nullptr;
        QLabel *empty = nullptr;
        QPushButton *allBtn = nullptr;
        QVector<VideoInfo> videos;
        bool showAll = false;
    };

    void rebuildSubscriptions();
    void rebuildFavorites();
    void rebuildWatchLater();
    void rebuildHistory();
    void rebuildPlaylists();
    void onChannelInfo(const ChannelInfo &channel);
    // A cached live stream may end between visits: on re-entering the page it
    // is re-checked with /next (it does not touch the player phase), with a
    // kLiveRecheckMs dedup and a per-video in-flight guard.
    void maybeRefreshLiveStatus(const VideoInfo &v);
    void onLiveDetailReady(const VideoInfo &video);
    void addStandardMenuActions(QMenu *menu, const VideoInfo &v,
                                bool withWatchLater);

    void createPlaylist();
    // Create / rename playlist: nt::inputText (the app's dialog), not a
    // popover of its own. promptNewPlaylist also adds the pending video if
    // there is one; promptRenamePlaylist starts from the current name.
    void promptNewPlaylist(const QString &videoId);
    void promptRenamePlaylist(const PlaylistItem &playlist);
    void centerPopup(QFrame *popup);

    void setupManageSubsPopup();
    void openManageSubs();
    void closeManageSubs();
    void rebuildManageSubs();
    void layoutCards(QGridLayout *grid, const QVector<QWidget *> &cards);
    QString subMetaFor(const QString &channelId) const;
    void updateSubsRowGeometry();
    void buildRows(QVBoxLayout *lay, const QVector<VideoInfo> &videos, int visible,
                   const std::function<void(int)> &activate,
                   const std::function<void(QMenu *, const VideoInfo &)> &fillMenu,
                   const QHash<QString, double> &progress = {});

    void resizeEvent(QResizeEvent *event) override;

    Yt *m_yt;
    FavoritesRepo *m_favs;
    StarredRepo *m_starred;
    WatchLaterRepo *m_watchLater;
    HistoryRepo *m_history;
    SubscriptionsRepo *m_subs;
    VideoCacheRepo *m_cache;
    ThumbnailCache *m_thumbs;
    PlaylistsRepo *m_playlists;

    QScrollArea *m_scroll = nullptr;
    QWidget *m_body = nullptr;

    // Subscriptions
    QLabel *m_channelHint = nullptr;
    QLabel *m_subsTitle = nullptr;
    QLabel *m_playlistsTitle = nullptr;
    QScrollArea *m_subScroll = nullptr;
    QWidget *m_subHost = nullptr;
    QHBoxLayout *m_subRowLay = nullptr;
    QHash<QString, QString> m_subTitles;   // channelId → channel title
    QHash<QString, ChannelRow *> m_subMeta;    // channelId → subscription card
    QHash<QString, long long> m_subCounts; // channelId → subscribers (cache)

    QLabel *m_playlistEmpty = nullptr;
    QGridLayout *m_playlistGrid = nullptr;
    QVector<QWidget *> m_playlistCards;
    QVector<PlaylistItem> m_playlistMeta;

    VideoSection m_fav;
    VideoSection m_later;
    VideoSection m_historySec;

    // Live re-checks of History: ids with a /next request in flight and the
    // last check time of each.
    QSet<QString> m_liveRefreshPending;
    QHash<QString, qint64> m_liveRefreshedAt;

    QFrame *m_managePopup = nullptr;
    QVBoxLayout *m_manageLay = nullptr;
};