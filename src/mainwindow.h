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
#include "updater/update_check.h"

class HomePage;
class ChannelPage;
class SearchPage;
class WatchPage;
class LibraryPage;
class PlaylistPage;
class HistoryPage;
class DownloadsPage;

namespace dl {
class DownloadManager;
}
namespace nt {
class SystemMediaControls;
}
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
    // Opens the "Descargas" tab.
    void openDownloads();
    // Plays a local file (a download), the same path as the button of the
    // Downloads page.
    void playLocal(const QString &path);
    // The download button of the Player, found by its name. It is how the
    // integration test presses it (there is no other way to click in a
    // headless test).
    QAbstractButton *downloadButton();
    // The repository of the downloads (the integration test reads it).
    DownloadRepo *downloadsRepo() { return &m_downloads; }
    // The settings (the folder of the downloads, the codec, ...).
    SettingsRepo *settingsRepo() { return &m_settings; }
    // The downloader (the queue, the folder and the connections).
    dl::DownloadManager *downloadManager() { return m_downloadManager; }
    // Revives the downloads that a previous session left half way (the rows
    // that say "pending" and the ones that were running when the app closed).
    //
    // It is called ONCE by main.cpp when the app really opens, NOT by the
    // constructor: building a MainWindow (the integration test does) must not
    // start the downloads that the person left waiting, with network and
    // files, behind a test that is supposed to be offline.
    void resumePendingDownloads();
    // Pauses and continues a download (what the buttons of the page of
    // Descargas do). The argument may be the video id or the KEY of the row
    // ("<videoId>-audio" for an mp3): the row that exists is the one used.
    void pauseDownload(const QString &idOrKey);
    void resumeDownload(const QString &idOrKey);
    // Queues a download by id (the same path the button of the Player uses).
    void queueDownload(const QString &videoId);
    // Queues the download of a video (the same path as the button of the
    // Player). Exposed so it can be driven from the command line.
    //
    // `height` (0 = the best) and `audioOnly` are what the dialog of the
    // download would ask for; with them the dialog is skipped, which is how
    // the download is driven from the command line.
    void downloadVideo(const QString &videoId, int height = 0,
                       bool audioOnly = false);
    void openPlaylist(const QString &playlistId, const QString &title);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
#if defined(Q_OS_WIN)
    bool nativeEvent(const QByteArray &eventType, void *message, long *result) override;
#endif

private slots:
    void openWatch(const QVector<VideoInfo> &videos, int startIndex);
    void openWatchResume(const QVector<VideoInfo> &videos, int startIndex,
                         qlonglong startPositionMs);
    void goHome();
    void goSearch();
    void goLibrary();
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
    void updateDownloadPlaybackState();
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
    void setupSystemMedia();
    void updateSystemMedia();
    void openWatchAt(const QVector<VideoInfo> &videos, int startIndex,
                     qlonglong startPositionMs);

    // Video codec profile (av1-hw|vp9-hw|h264-hw|soft-all). Applies the
    // profile to Yt + buffer wait (and the 1080p cap in soft-all) and, if persist,
    // stores it in settings.
    void applyCodecProfile(const QString &profile, bool persist);
    // The folder of the downloads (the one of Ajustes, or the default one).
    void applyDownloadFolder();
    // Launches the offline HwProbe scan; the blocking dialog only appears if
    // it is slow (basic PC) or if the user tries to play while it scans.
    void startHwProbe();
    void showHwPopup();
    void centerHwPopup();

    // --- Updater ---------------------------------------------------------
    // Wires the "Buscar actualizaciones" button and schedules the ONE
    // automatic check of the session (never repeated: the timestamp of the
    // last one is persisted and a session that already checked does not ask
    // GitHub again).
    void setupUpdater();
    // `manual` = the person asked from Ajustes: it always answers (even with
    // "there is nothing new" or an error). The automatic one is silent unless
    // there is a newer version, and it is never shown on top of a video.
    void checkForUpdates(bool manual);
    void showUpdateDialog(bool waitForIdle);
    void onUpdateCheckFinished(const nt::UpdateRelease &release);
    void onUpdateCheckFailed(const QString &message);
    // Runs the verified installer and closes: the app cannot be running while
    // its own files are being replaced. Nothing is force-killed and no data is
    // deleted (the DB writes are immediate and the downloads keep their state).
    void onUpdateInstallRequested(const QString &installerPath);

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
    DownloadRepo m_downloads{m_db};

    nt::UpdateChecker *m_updateChecker = nullptr;
    nt::UpdateRelease m_updateRelease;
    bool m_updateCheckManual = false;
    bool m_updateDialogPending = false;

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

    // Video opened from a YouTube link pasted in the search box: until its
    // title arrives (the player resolves it) the address is still in the field
    // and the search history does not name it yet. Empty = nothing pending.
    QString m_searchUrlId;

    HomePage *m_home = nullptr;
    SearchPage *m_search = nullptr;
    ChannelPage *m_channel = nullptr;
    WatchPage *m_watch = nullptr;
    LibraryPage *m_library = nullptr;
    PlaylistPage *m_playlist = nullptr;
    HistoryPage *m_historyPage = nullptr;
    DownloadsPage *m_downloadsPage = nullptr;
    dl::DownloadManager *m_downloadManager = nullptr;
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

    nt::SystemMediaControls *m_smc = nullptr;
};
