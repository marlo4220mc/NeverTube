#pragma once

#include <QHash>
#include <QWidget>
#include <QVector>

#include "../../models.h"

class QLabel;
class QPushButton;
class QVBoxLayout;
class QScrollArea;
class FavoritesRepo;
class HistoryRepo;
class StarredRepo;
class WatchLaterRepo;
class PlaylistsRepo;
class SubscriptionsRepo;
class ThumbnailCache;
class VideoCacheRepo;
class VideoListView;
class Yt;

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

signals:
    void channelRequested(const QString &channelId, const QString &channelName);
    void openRequested(const QVector<VideoInfo> &videos, int startIndex);
    void resumeRequested(const QVector<VideoInfo> &videos, int startIndex,
                         qlonglong startPositionMs);
    void playlistRequested(const QString &playlistId, const QString &title);
    void historyAllRequested();

private:
    void rebuild();
    void rebuildPlaylists();
    void rebuildFavorites();
    void rebuildStarred();
    void rebuildWatchLater();
    void rebuildHistory();
    void onChannelInfo(const ChannelInfo &channel);
    void createPlaylist();

    Yt *m_yt;
    FavoritesRepo *m_favs;
    StarredRepo *m_starred;
    WatchLaterRepo *m_watchLater;
    HistoryRepo *m_history;
    SubscriptionsRepo *m_subs;
    VideoCacheRepo *m_cache;
    ThumbnailCache *m_thumbs;
    PlaylistsRepo *m_playlists;
    QHash<QString, QLabel *> m_avatarLabels;
    QLabel *m_channelHint;
    QVBoxLayout *m_listLay;
    QVBoxLayout *m_playlistLay = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_body = nullptr;
    VideoListView *m_favList = nullptr;
    QLabel *m_favEmpty = nullptr;
    QLabel *m_favTitle = nullptr;
    QVector<VideoInfo> m_favVideos;
    QLabel *m_starTitle = nullptr;
    VideoListView *m_starList = nullptr;
    QLabel *m_starEmpty = nullptr;
    QVector<VideoInfo> m_starVideos;
    QLabel *m_laterTitle = nullptr;
    VideoListView *m_laterList = nullptr;
    QLabel *m_laterEmpty = nullptr;
    QVector<VideoInfo> m_laterVideos;
    QLabel *m_historyTitle = nullptr;
    VideoListView *m_historyList = nullptr;
    QLabel *m_historyEmpty = nullptr;
    QPushButton *m_historyAllBtn = nullptr;
    QVector<VideoInfo> m_historyVideos;
};