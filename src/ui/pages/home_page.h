#pragma once

#include <QScrollArea>
#include <QWidget>
#include <QVector>

#include "../../models.h"

class QLabel;
class QPushButton;
class SubscriptionsRepo;
class ThumbnailCache;
class VideoListView;
class Yt;

class HomePage : public QWidget
{
    Q_OBJECT

public:
    explicit HomePage(Yt *yt, SubscriptionsRepo *subs,
                      ThumbnailCache *thumbs, QWidget *parent = nullptr);

    void reload();
    void setYtAvailable(bool ok);

signals:
    void openRequested(const QVector<VideoInfo> &videos, int startIndex);

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onTrending(const QVector<VideoInfo> &videos);
    void onChannelVideos(const QVector<VideoInfo> &videos);
    void onFeedDone();

private:
    void loadTrending();
    void loadFeed();
    void requestNextChannel();
    void refreshBusy();
    void setSectionVisible(QLabel *title, VideoListView *grid, bool visible);
    void adjustGridHeight(VideoListView *grid);

    Yt *m_yt;
    SubscriptionsRepo *m_subs;
    ThumbnailCache *m_thumbs;

    QScrollArea *m_scroll;
    QWidget *m_body;
    QLabel *m_busy;
    QLabel *m_feedTitle;
    QLabel *m_trendTitle;
    VideoListView *m_feedGrid;
    VideoListView *m_trendGrid;
    QLabel *m_banner;

    QVector<VideoInfo> m_trending;
    QVector<VideoInfo> m_feed;
    QStringList m_pendingChannels;
    int m_pendingIdx = 0;
    bool m_loadingHome = false;
    bool m_ytAvailable = true;
    bool m_feedBusy = false;
};