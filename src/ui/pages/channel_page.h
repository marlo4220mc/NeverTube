#pragma once

#include <QWidget>
#include <QVector>

#include "../../models.h"

class QLabel;
class PlayerController;
class ThumbnailCache;
class VideoListView;
class Yt;
class QTabWidget;
class QPushButton;

class ChannelPage : public QWidget
{
    Q_OBJECT

public:
    explicit ChannelPage(Yt *yt, ThumbnailCache *thumbs,
                         QWidget *parent = nullptr);

    void openChannel(const QString &channelId);

signals:
    void backRequested();
    void openRequested(const QVector<VideoInfo> &videos, int startIndex);

private slots:
    void onChannelInfo(const ChannelInfo &channel);
    void onVideos(const QVector<VideoInfo> &videos);
    void onTabVideos(const QString &tab, const QVector<VideoInfo> &videos);

private:
    Yt *m_yt;
    ThumbnailCache *m_thumbs;
    QLabel *m_title;
    QLabel *m_meta;
    QLabel *m_description;
    QTabWidget *m_tabs;
    QVector<VideoListView *> m_lists;
    QStringList m_tabKeys;
    QVector<QPushButton *> m_moreButtons;
    QString m_channelId;
    QString m_channelName;
    QVector<QVector<VideoInfo>> m_videos;
};
