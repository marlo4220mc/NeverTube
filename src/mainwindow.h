#pragma once

#include <QList>
#include <QMainWindow>
#include <QProgressBar>

#include "db/app_database.h"
#include "db/repositories.h"
#include "player_controller.h"
#include "ui/icons.h"

class HomePage;
class SearchPage;
class WatchPage;
class PlaceholderPage;
class PlayerWidget;
class ThumbnailCache;
class Yt;

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

private slots:
    void openWatch(const QVector<VideoInfo> &videos, int startIndex);
    void goHome();
    void goSearch();
    void goLibrary();
    void goSettings();
    void showPreviousTab();
    void onNavButton(int index);

private:
    void setupNav();
    void setupMiniBar();
    void updateMiniBar();
    void updateMiniIcons();
    QPushButton *makeNavButton(nt::Icon icon, const QString &text, int index);

    AppDatabase m_db;
    Yt *m_yt = nullptr;
    ThumbnailCache *m_thumbs = nullptr;
    PlayerWidget *m_playerWidget = nullptr;
    PlayerController m_player{m_yt, m_playerWidget, &m_history, this};

    VideoCacheRepo m_videoCache{m_db};
    HistoryRepo m_history{m_db};
    FavoritesRepo m_fav{m_db};
    SubscriptionsRepo m_subs{m_db};
    PlaylistsRepo m_playlists{m_db};
    SettingsRepo m_settings{m_db};

    HomePage *m_home = nullptr;
    SearchPage *m_search = nullptr;
    WatchPage *m_watch = nullptr;
    PlaceholderPage *m_library = nullptr;
    PlaceholderPage *m_settingsPage = nullptr;

    QWidget *m_nav = nullptr;
    QList<QPushButton *> m_navButtons;
    QStackedWidget *m_pages = nullptr;

    QWidget *m_miniBar = nullptr;
    QStackedWidget *m_miniStack = nullptr;
    QProgressBar *m_miniProgress = nullptr;
    QLabel *m_miniTitle = nullptr;
    QPushButton *m_miniPlayBtn = nullptr;
    QPushButton *m_miniMuteBtn = nullptr;

    int m_lastTab = 0;
    int m_currentTab = 0;
};
