#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QMainWindow>
#include <QProgressBar>
#include <QSet>

#include "db/app_database.h"
#include "db/repositories.h"
#include "player_controller.h"
#include "ui/icons.h"

class HomePage;
class ChannelPage;
class SearchPage;
class WatchPage;
class LibraryPage;
class PlaylistPage;
class HistoryPage;
class PlaceholderPage;
class PlayerWidget;
class ThumbnailCache;
class Yt;

class QKeyEvent;
class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;

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
    void onNavButton(int index);
    void onRadioReadyDiscover(const QString &videoId,
                              const QVector<VideoInfo> &videos);

private:
    void setupNav();
    void setupMiniBar();
    void updateMiniPlayer();
    void updateMiniBar();
    void updateMiniIcons();
    void setMiniThumb();
    void goToPlayerPage();
    bool handleShortcut(QKeyEvent *event);
    QPushButton *makeNavButton(nt::Icon icon, const QString &text, int index);
    void openWatchAt(const QVector<VideoInfo> &videos, int startIndex,
                     qlonglong startPositionMs);

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

    // Semillas de vídeo reproduciéndose para las que ya se pidió su radio (y
    // aún no llegó) desde el hook de reproducción confirmada: evita re-pedir la
    // misma radio al repetirse phase "playing" (reanudar/pausar). La entrada se
    // elimina al guardar la radio o si la respuesta llega vacía (reintento).
    QSet<QString> m_radioSeedsRequested;

    HomePage *m_home = nullptr;
    SearchPage *m_search = nullptr;
    ChannelPage *m_channel = nullptr;
    WatchPage *m_watch = nullptr;
    LibraryPage *m_library = nullptr;
    PlaylistPage *m_playlist = nullptr;
    HistoryPage *m_historyPage = nullptr;
    PlaceholderPage *m_settingsPage = nullptr;

    QWidget *m_nav = nullptr;
    QList<QPushButton *> m_navButtons;
    QStackedWidget *m_pages = nullptr;

    QWidget *m_miniBar = nullptr;
    QLabel *m_miniThumb = nullptr;
    QLabel *m_miniTitle = nullptr;
    QLabel *m_miniTime = nullptr;
    QProgressBar *m_miniProgress = nullptr;
    QPushButton *m_miniPlayBtn = nullptr;
    QPushButton *m_miniMuteBtn = nullptr;
    QPushButton *m_miniCloseBtn = nullptr;

    int m_lastTab = 0;
    int m_currentTab = 0;
    QElapsedTimer m_elapsed;
};
