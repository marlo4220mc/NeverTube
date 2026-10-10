#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QMainWindow>
#include <QProgressBar>
#include <QSet>
#include <QToolButton>

#include "db/app_database.h"
#include "db/repositories.h"
#include "player_controller.h"
#include "ui/icons.h"
#include "ui/live_detect.h"

class HomePage;
class ChannelPage;
class SearchPage;
class WatchPage;
class LibraryPage;
class PlaylistPage;
class HistoryPage;
class SettingsPage;
class PlayerWidget;
class ThumbnailCache;
class Yt;
class HwProbe;

class QKeyEvent;
class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
class QFrame;
class QTimer;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void searchFor(const QString &query);
    void playVideo(const QString &url);
    void openChannel(const QString &channelId);
    void openLibrary();
    void openPlaylist(const QString &playlistId, const QString &title);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void openWatch(const QVector<VideoInfo> &videos, int startIndex);
    void openWatchResume(const QVector<VideoInfo> &videos, int startIndex,
                         qlonglong startPositionMs);
    void goHome();
    void goSearch();
    void goLibrary();
    void goSettings();
    void showPreviousTab();
    void openHistory();
    void resetAppData();
    void onNavButton(int index);
    void onRadioReadyDiscover(const QString &videoId,
                              const QVector<VideoInfo> &videos);
    // Gated-session recovery exhausted: restarts the app with the last video
    // (restarting gets out of the gated session). The app's only auto-restart,
    // limited to this error and with an anti-loop guard.
    void onRecoveryExhausted();
    void onRecoveryHealthy();
    // Options menu ("⋯") of a video row on the surfaces that do not
    // have the repos (Search, Channel, Playlist): the window does, so the
    // menu is built here, once, with the same actions as
    // Library, History and "A continuación".
    void showVideoRowMenu(const VideoInfo &video, const QPoint &globalPos);

private:
    void setupNav();
    void setupMiniBar();
    void updateMiniPlayer();
    // Persists player volume and mute in the settings DB
    // (keys "volume" / "volume_muted"), to re-apply them on the
    // next startup. Reads the controller state; called after every
    // change made by the user (keyboard, mini bar or overlay).
    void persistVolume();
    void applyMiniVideoMode();
    // "Tamaño de las miniaturas" (Ajustes → Interfaz): the only place that applies
    // the setting, both at startup (with the stored value) and live
    // (SettingsPage::thumbSizeChanged). The value maps to TWO effects,
    // because grids and lists are not sized the same way: list views
    // scale their row with a factor, and the Home grids (which are
    // width-sized with fixed columns) change their number of
    // columns. See SettingsPage::thumbListScale/thumbGridColumns.
    void applyThumbSize(const QString &value);
    // UI language change from Settings: applies the catalog, refreshes the
    // shell text that is not a binding (the mini player, which depends on the
    // playback state) and rebuilds the Settings page deferred, so the menu
    // that emitted the signal is not destroyed while delivering it.
    void applyLanguage(const QString &code);
    void updateMiniBar();
    void updateMiniIcons();
    void setMiniThumb();
    // Elides the mini title to one line with ellipsis at the real width.
    void updateMiniTitleElide();
    // Composes the mini's second line: "Canal · m:ss / m:ss".
    void updateMiniTimeText();
    void goToPlayerPage();
    bool handleShortcut(QKeyEvent *event);
    void openWatchAt(const QVector<VideoInfo> &videos, int startIndex,
                     qlonglong startPositionMs);

    // Video codec profile (av1-hw|vp9-hw|h264-hw|soft-all). Applies the
    // profile to Yt + buffer wait (and the 1080p cap in soft-all) and, if persist,
    // stores it in settings.
    void applyCodecProfile(const QString &profile, bool persist);
    // Launches the offline HwProbe scan; the blocking dialog only appears if
    // it is slow (basic PC) or if the user tries to play while it scans.
    void startHwProbe();
    void showHwPopup();
    void centerHwPopup();

    AppDatabase m_db;
    Yt *m_yt = nullptr;
    ThumbnailCache *m_homeThumbs = nullptr;
    ThumbnailCache *m_globalThumbs = nullptr;
    ThumbnailCache *m_libraryThumbs = nullptr;
    PlayerWidget *m_playerWidget = nullptr;

    VideoCacheRepo m_videoCache{m_db};
    DiscoverRepo m_discoverRadios{m_db};
    HistoryRepo m_history{m_db, &m_discoverRadios};
    PlayerController m_player{m_yt, m_playerWidget, &m_history, this};

    FavoritesRepo m_fav{m_db};
    StarredRepo m_starred{m_db};
    WatchLaterRepo m_watchLater{m_db};
    SubscriptionsRepo m_subs{m_db};
    PlaylistsRepo m_playlists{m_db};
    SettingsRepo m_settings{m_db};
    HomeCacheRepo m_homeCache{m_db};

    // Auto-restart for gated sessions: in-memory guard against double
    // restarts of the same episode, and position (ms) to resume at in the new
    // instance (read from settings at startup; see onRecoveryExhausted).
    bool m_gateRestarting = false;
    qlonglong m_gateResumeMs = 0;

    // Video seeds already playing for which the radio was requested (and has
    // not arrived yet) from the confirmed-playback hook: avoids re-requesting
    // the same radio when the "playing" phase repeats (resume/pause). The entry is
    // removed when the radio is saved or if the response arrives empty (retry).
    QSet<QString> m_radioSeedsRequested;

    HomePage *m_home = nullptr;
    SearchPage *m_search = nullptr;
    ChannelPage *m_channel = nullptr;
    WatchPage *m_watch = nullptr;
    LibraryPage *m_library = nullptr;
    PlaylistPage *m_playlist = nullptr;
    HistoryPage *m_historyPage = nullptr;
    SettingsPage *m_settingsPage = nullptr;

    QWidget *m_nav = nullptr;
    QList<QToolButton *> m_navButtons;
    QStackedWidget *m_pages = nullptr;

    QWidget *m_miniBar = nullptr;
    // Slot in the mini bar where the live PlayerWidget is embedded (reparent),
    // with the static thumbnail as fallback when there is no video.
    QWidget *m_miniVideoHost = nullptr;
    bool m_miniDetached = false;
    QLabel *m_miniThumb = nullptr;
    QLabel *m_miniTitle = nullptr;
    QLabel *m_miniTime = nullptr;
    QString m_miniTitleFull;
    QString m_miniChannel;
    double m_miniPos = 0.0;
    double m_miniDur = 0.0;
    // Visible state of live mode in the mini. The DETECTION lives in
    // nt::LiveDetector (same rule as the overlay); it is reset when
    // switching videos.
    bool m_miniLive = false;
    nt::LiveDetector m_miniLiveDet;
    QProgressBar *m_miniProgress = nullptr;
    QPushButton *m_miniPlayBtn = nullptr;
    QPushButton *m_miniNextBtn = nullptr;
    QPushButton *m_miniMuteBtn = nullptr;
    QPushButton *m_miniCloseBtn = nullptr;

    int m_lastTab = 0;
    int m_currentTab = 0;
    // No hardware decoding at all (legacy profile + h264
    // also in software). Triggers the 1080p cap in settings/player and
    // the initial buffer wait by % for 1080p.
    bool m_noHw = false;
    QElapsedTimer m_elapsed;

    // Profile descent ladder driven by playback (codec/HW evidence,
    // never network): its own budget, separate from the session recovery one,
    // and anti-bounce so the same episode does not descend several times. The descent
    // is EPHEMERAL (not persisted): a one-off failure must not "condemn" the
    // machine to a lower profile (Settings -> "Volver a detectar" restores it).
    int m_descendAttempts = 0;
    qint64 m_lastDescendMs = 0;
    // Profile ACTIVE in memory (may differ from the persisted one if the ladder
    // descended ephemerally). The ladder compares against this, not against
    // settings: otherwise, after an in-memory descent it would still see the old
    // profile and descend again in error.
    QString m_activeProfile;

    // Hardware video probe (HwProbe) and its dialog. While m_hwProbing
    // is active nothing plays: the request is queued and runs when it finishes.
    HwProbe *m_hwProbe = nullptr;
    bool m_hwProbing = false;
    QFrame *m_hwPopup = nullptr;
    QTimer *m_hwPopupTimer = nullptr;
    bool m_hasPendingWatch = false;
    QVector<VideoInfo> m_pendingWatch;
    int m_pendingIndex = 0;
    qlonglong m_pendingPos = 0;
};
