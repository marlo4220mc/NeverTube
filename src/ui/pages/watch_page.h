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

class QFrame;
class QLabel;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTimer;
class QEvent;
class SeekSlider;
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
    // Genera la cola del mix RDAMVM para el vídeo dado (click explícito desde
    // fuera de "A continuación": search, home, channel). No se auto-genera.
    void triggerRadio(const QString &videoId);

signals:
    void backRequested();
    void channelRequested(const QString &channelId, const QString &name);
    void fullscreenToggled(bool on);

private slots:
    void onCurrentChanged();
    void onQueueChanged();
    void onPhaseChanged(const QString &phase);
    void onPositionChanged(double position, double duration);
    void onDetailReady(const VideoInfo &video, const QString &desc,
                       qint64 likes, const QVector<FormatInfo> &formats);
    void onChannelInfo(const ChannelInfo &channel);
    void onAvatarReady(const QString &key);
    void onQualityChanged(int index);
    void onVideoSizeChanged(int width, int height);
    void onPlayPause();
    void onToggleMute();
    void onNextUpActivated(int index, const VideoInfo &video);
    void onRelatedReady(const QString &videoId, const QVector<VideoInfo> &videos);
    void onRadioReady(const QString &videoId, const QVector<VideoInfo> &videos);

public slots:
    void toggleFullscreen();

private:
    void updateQueue();
    void updateInfo();
    void updateChannelRow();
    void enterFullscreen();
    void leaveFullscreen();
    int qualityIndexForHeight(int height) const;
    int idxToQuality(int idx) const;
    void setQualityIndex(int index);
    void toggleQualityMenu();
    void showOverlayControls();
    void hideOverlayControls();
    void updateOverlayIcons();
    bool isVideoClick(const QObject *watched) const;

    bool eventFilter(QObject *watched, QEvent *event) override;

    Yt *m_yt;
    PlayerController *m_player;
    PlayerWidget *m_playerWidget = nullptr;
    VideoCacheRepo *m_cache;
    FavoritesRepo *m_favs;
    SettingsRepo *m_settings;
    ThumbnailCache *m_thumbs;

    QPushButton *m_back;
    QStackedWidget *m_overlay;
    QLabel *m_errorLabel;
    QWidget *m_topScroll = nullptr;
    QWidget *m_bottomWidget = nullptr;
    QWidget *m_belowWidget = nullptr;
    bool m_inFullscreen = false;
    QLabel *m_title;
    QLabel *m_meta;
    QPushButton *m_channelBtn;
    QString m_channelAvatarKey;
    QPushButton *m_quality;
    QFrame *m_qualityMenu;
    QWidget *m_qualityMenuAnchor;
    int m_qualityIndex = 2;
    bool m_qualityMenuActive = false;
    QLabel *m_descLabel;
    VideoListView *m_nextUpList;

    // Controles flotantes sobre el video (overlay).
    QWidget *m_ctrlBar = nullptr;
    QPushButton *m_ovPlayBtn = nullptr;
    QPushButton *m_ovNextBtn = nullptr;
    SeekSlider *m_ovSeek = nullptr;
    QLabel *m_ovTime = nullptr;
    QPushButton *m_ovMuteBtn = nullptr;
    QSlider *m_ovVolume = nullptr;
    QPushButton *m_ovSubsBtn = nullptr;
    QPushButton *m_ovFsBtn = nullptr;
    QTimer *m_overlayTimer = nullptr;
    QPoint m_lastMousePos = QPoint(-100000, -100000);
    double m_volume = 100.0;
    bool m_subsVisible = false;

    QVector<VideoInfo> m_queue;
    int m_queueStart = -1;
    QVector<VideoInfo> m_nextUp;
    QString m_description;
    qint64 m_likeCount = 0;
    bool m_fetchingDetail = false;
    // Siembra de "A continuación": id del vídeo cuyo /next ya se pidió (un
    // solo pedido por vídeo; los nuevos vídeos que llegan de los relacionados
    // disparan su propio /next cuando la cola vuelve a acercarse al final).
    QString m_relatedVideoId;
};