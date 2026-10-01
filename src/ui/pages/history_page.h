#pragma once

#include <QVector>
#include <QWidget>

#include "../../models.h"

class HistoryRepo;
class VideoCacheRepo;
class VideoListView;
class ThumbnailCache;
class WatchLaterRepo;
class PlaylistsRepo;

class QLabel;
class QPushButton;

class HistoryPage : public QWidget
{
    Q_OBJECT

public:
    explicit HistoryPage(HistoryRepo *history, VideoCacheRepo *cache,
                         ThumbnailCache *thumbs, WatchLaterRepo *watchLater,
                         PlaylistsRepo *playlists, QWidget *parent = nullptr);

    void reload();

signals:
    void backRequested();
    void historyChanged();
    void resumeRequested(const QVector<VideoInfo> &videos, int startIndex,
                         qlonglong startPositionMs);
    void channelRequested(const QString &channelId);

private:
    void showMoreMenu(const VideoInfo &v, const QPoint &globalPos);
    void applyProgress();

    HistoryRepo *m_history;
    VideoCacheRepo *m_cache;
    ThumbnailCache *m_thumbs;
    WatchLaterRepo *m_watchLater;
    PlaylistsRepo *m_playlists;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    VideoListView *m_list = nullptr;
    QLabel *m_empty = nullptr;
    // Contenedor con stretch que comparten la lista y la caja de estado vacío
    // (ver el comentario del constructor).
    QWidget *m_area = nullptr;
    QVector<VideoInfo> m_videos;
};
