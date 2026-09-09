#pragma once

#include <QWidget>
#include <QVector>

#include "../../models.h"

class VideoCacheRepo;
class FavoritesRepo;
class SettingsRepo;
class PlayerController;
class PlayerWidget;
class ThumbnailCache;
class Yt;

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QSlider;
class VideoListView;

class WatchPage : public QWidget
{
    Q_OBJECT

public:
    explicit WatchPage(Yt *yt, PlayerController *player,
                       PlayerWidget *playerWidget,
                       VideoCacheRepo *cache, FavoritesRepo *favorites,
                       SettingsRepo *settings, ThumbnailCache *thumbs,
                       QWidget *parent = nullptr);

    void setWatchData(const QVector<VideoInfo> &queue, int startIndex);

signals:
    void backRequested();
    void channelRequested(const QString &channelId, const QString &name);

private slots:
    void onCurrentChanged();
    void onQueueChanged();
    void onPhaseChanged(const QString &phase);
    void onPositionChanged(double position, double duration);
    void onDetailReady(const VideoInfo &video, const QString &desc,
                       qint64 likes, const QVector<FormatInfo> &formats);
    void onQualityChanged(int index);
    void onPlayPause();
    void onToggleMute();
    void onFavToggle();
    void onNextUpActivated(int index, const VideoInfo &video);

private:
    void updateQueue();
    void updateFavIcon();
    void updateInfo();

    Yt *m_yt;
    PlayerController *m_player;
    VideoCacheRepo *m_cache;
    FavoritesRepo *m_favs;
    SettingsRepo *m_settings;
    ThumbnailCache *m_thumbs;

    QPushButton *m_back;
    QStackedWidget *m_overlay;
    QLabel *m_title;
    QLabel *m_meta;
    QPushButton *m_channelBtn;
    QPushButton *m_playPauseBtn;
    QPushButton *m_nextBtn;
    QPushButton *m_muteBtn;
    QPushButton *m_favBtn;
    QComboBox *m_quality;
    QSlider *m_slider;
    QLabel *m_time;
    QLabel *m_descLabel;
    QPushButton *m_descToggle;
    VideoListView *m_nextUpList;

    QVector<VideoInfo> m_queue;
    int m_queueStart = -1;
    QVector<VideoInfo> m_nextUp;
    QString m_description;
    qint64 m_likeCount = 0;
    bool m_descExpanded = false;
    bool m_fetchingDetail = false;
};