#pragma once

#include <QWidget>
#include <QVector>

#include "../../models.h"
#include "../icons.h"
#include "../live_detect.h"

class VideoCacheRepo;
class FavoritesRepo;
class WatchLaterRepo;
class PlaylistsRepo;
class StarredRepo;
class SubscriptionsRepo;
class SettingsRepo;
class PlayerController;
class PlayerWidget;
class ThumbnailCache;
class Yt;

class QFrame;
class QLabel;
class QListWidget;
class QMenu;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTimer;
class QEvent;
class SeekSlider;
class QSlider;
class VideoListView;
class PlayerContainer;

class WatchPage : public QWidget
{
    Q_OBJECT

public:
    explicit WatchPage(Yt *yt, PlayerController *player,
                       PlayerWidget *playerWidget,
                       VideoCacheRepo *cache, FavoritesRepo *favorites,
                       StarredRepo *starred, WatchLaterRepo *watchLater,
                       SubscriptionsRepo *subs, SettingsRepo *settings,
                       ThumbnailCache *thumbs, PlaylistsRepo *playlists,
                       QWidget *parent = nullptr);

    void setWatchData(const QVector<VideoInfo> &queue, int startIndex);
    // Generates the RDAMVM mix queue for the given video (explicit click from
    // outside "A continuación": search, home, channel). Not auto-generated.
    void triggerRadio(const QString &videoId);

    // Mini player with live video: moves the PlayerWidget (and its overlay) to the
    // mini bar's `host` widget while keeping it visible/rendering, so that
    // mpv does not accumulate frames nor the decoder reconfigure. attachPlayer()
    // puts it back into the page's video area. Reparenting within the same
    // window preserves the GL context and the mpv_render_context.
    void detachPlayerTo(QWidget *host);
    void attachPlayer();
    bool playerDetached() const { return m_playerDetached; }

signals:
    void backRequested();
    void channelRequested(const QString &channelId, const QString &name);
    void fullscreenToggled(bool on);

private slots:
    void onCurrentChanged();
    void onQueueChanged();
    void onPhaseChanged(const QString &phase);
    void onPositionChanged(double position, double duration);
    // Volume/mute changed through any path (keyboard, slider, mute
    // button, setting): syncs the overlay slider live.
    void onVolumeChanged();
    void onDetailReady(const VideoInfo &video, const QString &desc,
                       qint64 likes, const QVector<FormatInfo> &formats);
    void onDetailFailed(const QString &videoId);
    void onChannelInfo(const ChannelInfo &channel);
    void onAvatarReady(const QString &key);
    void onQualityChanged(int index);
    void onVideoSizeChanged(int width, int height);
    void onPlayPause();
    void onToggleMute();
    void onNextUpActivated(int index, const VideoInfo &video);
    void onRelatedReady(const QString &videoId, const QVector<VideoInfo> &videos);
    void onRadioReady(const QString &videoId, const QVector<VideoInfo> &videos);
    void onCurrentDetailReady(const QString &videoId, const QString &title,
                              const QString &description);
    // The backend reported the media's real duration: it is saved into the video
    // cache (the one "Continuar viendo" reads), filling the gap for
    // videos that arrived from search/home/channel without a duration.
    void onDurationKnown(double seconds);

public slots:
    void toggleFullscreen();
    // Re-applies the overlay slider's range/value from the controller
    // (e.g. when the "Maximum volume" setting changes in Settings).
    void refreshVolumeSlider();
    // Re-applies styles/icons that depend on the accent color (Subscribe
    // button, active actions, description links, overlay subs icon). Called
    // from MainWindow when the accent changes in Settings, so everything follows
    // the color without rebuilding the page.
    void applyAccent();

    // Re-applies the composed labels (title, channel/subscribe buttons, meta line,
    // queue counter, quality tooltip) in the active language. Called by MainWindow
    // when the language changes; the static chrome is live-bound.
    void retranslate();

private:
    void updateQueue();
    void updateInfo();
    // Meta line (views · date) alone, without re-requesting the channel avatar.
    void updateMeta();
    // Writes the gear's tooltip: the active resolution if it is known, the plain
    // "Calidad" key otherwise. Shared by the constructor, setQualityIndex,
    // onVideoSizeChanged and retranslate.
    void refreshQualityTooltip();
    void updateChannelRow();
    void updateSubscribeButton(bool subscribed);
    void updateSubscription(const VideoInfo &v, bool subscribe);
    void updateActionButtons();
    void enterFullscreen();
    void leaveFullscreen();
    int qualityIndexForHeight(int height) const;
    int idxToQuality(int idx) const;
    void setQualityIndex(int index);
    void toggleQualityMenu();
    // Connected to PlayerWidget::qualityLimitsChanged: hides the resolution
    // menu items above the file's real ceiling. Ignores
    // values <= 0 (incomplete load) so as not to break the menu.
    void applyQualityLimits(int maxVideoHeight);
    // Connected to Yt::streamHeightsReady: stores the maximum height the video
    // offers (adaptiveFormats) so Auto does not hide real qualities the
    // loaded stream lacks (e.g. Auto asks 360 for a 1080 video).
    void onStreamHeights(const QString &videoId, const QVector<int> &heights);
    // Paints the video description as rich HTML (links + line
    // breaks) and stores the plain text. Empty = hides the description. It is
    // ALWAYS shown complete (no collapse, no "…más").
    void applyDescription(const QString &description);
    // True if the given video is the one being played (ignores responses
    // arriving late, for a video already replaced).
    bool isCurrentVideo(const QString &videoId) const;
    void showPlaylistMenu();
    // Options menu ("⋯") of an "A continuación" row: "Quitar de la
    // cola" (Remove from queue; not offered for the video being played) plus the
    // standard actions shared with the other row-bearing surfaces.
    void showQueueRowMenu(int index, const VideoInfo &video,
                          const QPoint &globalPos);
    // "Nueva playlist": nt::inputText (the app's dialog, the same as
    // Library, History and Playlist). If a video is passed, it is added to the
    // new playlist.
    void promptNewPlaylist(const QString &videoId);
    void showOverlayControls();
    void hideOverlayControls();
    void updateOverlayIcons();
    bool isVideoClick(const QObject *watched) const;
    bool isControlClick(const QObject *watched) const;

    bool eventFilter(QObject *watched, QEvent *event) override;

    Yt *m_yt;
    PlayerController *m_player;
    PlayerWidget *m_playerWidget = nullptr;
    PlayerContainer *m_playerContainer = nullptr;
    bool m_playerDetached = false;
    VideoCacheRepo *m_cache;
    FavoritesRepo *m_favs;
    StarredRepo *m_starred;
    WatchLaterRepo *m_watchLater;
    SubscriptionsRepo *m_subs;
    SettingsRepo *m_settings;
    ThumbnailCache *m_thumbs;
    PlaylistsRepo *m_playlists;
    bool m_liked = false;
    bool m_disliked = false;
    bool m_isStarred = false;
    bool m_watchLaterMarked = false;

    // Player's "Volver" (Back): floating link OVER the video that only appears with
    // the mouse. Lives on the overlay page (gl) and shares the 2.5 s cycle of
    // the controls; hidden in fullscreen, where only the video should remain.
    QPushButton *m_back;
    QStackedWidget *m_overlay;
    QLabel *m_errorLabel;
    // Single SCROLL for the whole page: video on top + row with description and
    // "A continuación" (right, no vertical limit), moved by the same
    // scroll (YouTube style).
    QScrollArea *m_topScroll = nullptr;
    QWidget *m_bottomWidget = nullptr;
    // Description block (title/meta/channel/actions/description) inside
    // the scroll. Hidden in fullscreen to leave only the video.
    QWidget *m_watchBody = nullptr;
    bool m_inFullscreen = false;
    QLabel *m_title;
    QLabel *m_meta;
    QPushButton *m_channelBtn;
    QString m_channelAvatarKey;
    QString m_channelAvatarUrl;
    QPushButton *m_subscribeBtn;
    QPushButton *m_likeBtn;
    QPushButton *m_dislikeBtn;
    QPushButton *m_shareBtn;
    QPushButton *m_starBtn = nullptr;
    QPushButton *m_watchLaterBtn = nullptr;
    QPushButton *m_playlistBtn = nullptr;
    nt::Icon m_likeIcon = nt::Icon::Thumb;
    nt::Icon m_dislikeIcon = nt::Icon::ThumbDown;
    nt::Icon m_shareIcon = nt::Icon::Share;
    nt::Icon m_starIcon = nt::Icon::Star;
    nt::Icon m_starFilledIcon = nt::Icon::StarFilled;
    nt::Icon m_watchLaterIcon = nt::Icon::WatchLater;
    nt::Icon m_playlistIcon = nt::Icon::ListAdd;
    QPushButton *m_quality;
    QFrame *m_qualityMenu;
    QWidget *m_qualityMenuAnchor;
    // Resolution menu items, aligned with kQualities[]/{360,480,
    // 720,1080,1440,2160}. Individually hidden (not removed) when the
    // loaded file lacks that track: see applyQualityLimits.
    QVector<QPushButton *> m_qualityItems;
    // REAL height of the video being played (mpv video-params/h). Lets the
    // button honestly show the height for sub-360p clips (where
    // kQualities[] has no entry), without lying with the closest 360p.
    int m_videoHeight = 0;
    int m_qualityIndex = 2;
    bool m_qualityMenuActive = false;
    bool m_autoQuality = false;
    int m_autoHeight = 360;
    // Maximum height the video offers according to Innertube (Yt::streamHeightsReady).
    // The menu offers all real qualities even if Auto asked for a
    // low one. 0 = unknown (falls back to the loaded stream's ceiling).
    int m_availableMaxH = 0;
    QLabel *m_descLabel;
    // Card grouping the description. The text is ALWAYS shown complete:
    // the 3-line collapse and the "…más"/"…menos" control were removed.
    QFrame *m_descCard = nullptr;
    VideoListView *m_nextUpList;
    // Queue counter, right-aligned next to the "A continuación" title.
    QLabel *m_queueCount = nullptr;

    // Floating controls over the video (overlay): single row.
    QWidget *m_ctrlBar = nullptr;
    QPushButton *m_ovShuffleBtn = nullptr;
    QPushButton *m_ovPlayBtn = nullptr;
    QPushButton *m_ovNextBtn = nullptr;
    QPushButton *m_ovPrevBtn = nullptr;
    QPushButton *m_ovRepeatBtn = nullptr;
    SeekSlider *m_ovSeek = nullptr;
    // The clock is split: elapsed to the left of the slider, duration to
    // the right (as in the mockup). It used to be a single "0:00 / 0:00".
    QLabel *m_ovPos = nullptr;
    QLabel *m_ovDur = nullptr;
    // Visible state of live mode (bar at the end, "EN VIVO", negative
    // clock). The DETECTION lives in nt::LiveDetector (unstable-duration
    // evidence + unlock); it is reset when the video changes.
    bool m_liveMedia = false;
    nt::LiveDetector m_liveDet;
    // Peak duration seen in live mode: the negative clock is monotonic
    // even if the reported duration fluctuates (DVR window).
    double m_liveDurPeak = 0.0;
    QPushButton *m_ovMuteBtn = nullptr;
    QSlider *m_ovVolume = nullptr;
    QPushButton *m_ovSubsBtn = nullptr;
    QPushButton *m_ovPlaylistBtn = nullptr;
    QPushButton *m_ovFsBtn = nullptr;
    QTimer *m_overlayTimer = nullptr;
    QPoint m_lastMousePos = QPoint(-100000, -100000);
    double m_volume = 100.0;
    bool m_subsVisible = false;
    qint64 m_lastVideoPressMs = -100000;
    QPoint m_lastVideoPressPos = QPoint(-100000, -100000);
    // A control in the box is pressed: the auto-hide timer must not
    // make the bar disappear mid-gesture.
    bool m_ctrlPressed = false;

    QVector<VideoInfo> m_queue;
    int m_queueStart = -1;
    QVector<VideoInfo> m_nextUp;
    QString m_description;
    qint64 m_likeCount = 0;
    bool m_fetchingDetail = false;
    // "A continuación" seeding: id of the video whose /next was already
    // requested (one request per video; new videos arriving from related
    // triggers fire their own /next when the queue nears the end again).
    QString m_relatedVideoId;
};