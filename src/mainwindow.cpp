#include "mainwindow.h"

#include <QButtonGroup>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QMouseEvent>

#include "models.h"
#include "playerwidget.h"
#include "ui/icons.h"
#include "ui/pages/home_page.h"
#include "ui/pages/placeholder_page.h"
#include "ui/pages/search_page.h"
#include "ui/pages/watch_page.h"
#include "ui/theme.h"
#include "ui/thumbnail_cache.h"
#include "yt.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_db()
    , m_yt(new Yt(this))
    , m_thumbs(new ThumbnailCache(this))
    , m_playerWidget(new PlayerWidget(this))
{
    m_db.open();
    m_player.setPlayer(m_playerWidget);

    const int qh = m_settings.getInt(QStringLiteral("quality"));
    if (qh > 0)
        m_player.setMaxHeight(qh);
    m_player.setMaxHeight(m_player.maxHeight());

    setWindowTitle(QStringLiteral("NeverTube"));
    resize(1280, 800);
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect avail = screen->availableGeometry();
        const int w = qMin(1280, avail.width());
        const int h = qMin(800, avail.height());
        resize(w, h);
        move(avail.x() + (avail.width() - w) / 2,
             avail.y() + (avail.height() - h) / 2);
    }

    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_pages = new QStackedWidget(central);
    m_home = new HomePage(m_yt, &m_subs, m_thumbs, this);
    m_search = new SearchPage(m_yt, &m_settings, m_thumbs, this);
    m_watch = new WatchPage(m_yt, &m_player, m_playerWidget, &m_videoCache,
                            &m_fav, &m_settings, m_thumbs, this);
    m_library = new PlaceholderPage(QStringLiteral("Biblioteca"), this);
    m_settingsPage = new PlaceholderPage(QStringLiteral("Ajustes"), this);

    m_pages->addWidget(m_home);
    m_pages->addWidget(m_search);
    m_pages->addWidget(m_watch);
    m_pages->addWidget(m_library);
    m_pages->addWidget(m_settingsPage);

    connect(m_home, &HomePage::openRequested, this, &MainWindow::openWatch);
    connect(m_search, &SearchPage::openRequested, this, &MainWindow::openWatch);
    connect(m_watch, &WatchPage::backRequested, this,
            &MainWindow::showPreviousTab);
    connect(m_watch, &WatchPage::channelRequested, this,
            [this](const QString &, const QString &) {
                statusBar()->showMessage(
                    QStringLiteral("Canal: próximamente"), 2500);
            });

    root->addWidget(m_pages, 1);
    setupMiniBar();
    root->addWidget(m_miniBar);
    setupNav();
    root->addWidget(m_nav);

    setCentralWidget(central);
    setStyleSheet(nt::styleSheet());
    statusBar()->setStyleSheet(
        QStringLiteral("background:#181818; color:#8F8F8F; "
                       "border-top:1px solid #262626; font-size:11px;"));

    goHome();
    m_home->reload();
    connect(&m_player, &PlayerController::phaseChanged, this,
            [this](const QString &) { updateMiniBar(); });
    connect(&m_player, &PlayerController::currentChanged, this,
            [this]() { updateMiniBar(); });
    connect(&m_player, &PlayerController::positionChanged, this,
            [this](double pos, double dur) {
                if (m_miniProgress) {
                    m_miniProgress->setRange(0, dur > 0 ? qRound(dur) : 0);
                    m_miniProgress->setValue(qRound(pos));
                }
            });
}

MainWindow::~MainWindow() = default;

void MainWindow::setupNav()
{
    m_nav = new QWidget(this);
    m_nav->setObjectName(QStringLiteral("navBottom"));
    m_nav->setFixedHeight(64);

    auto *lay = new QHBoxLayout(m_nav);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto *group = new QButtonGroup(m_nav);
    group->setExclusive(true);

    auto addBtn = [&](nt::Icon ic, const QString &text, int index) {
        QPushButton *b = makeNavButton(ic, text, index);
        group->addButton(b, index);
        lay->addWidget(b, 1);
        m_navButtons.append(b);
    };
    addBtn(nt::Icon::Home, QStringLiteral("Inicio"), 0);
    addBtn(nt::Icon::Search, QStringLiteral("Buscar"), 1);
    addBtn(nt::Icon::Library, QStringLiteral("Biblioteca"), 2);
    addBtn(nt::Icon::Settings, QStringLiteral("Ajustes"), 3);

    connect(group, QOverload<int>::of(&QButtonGroup::idClicked), this,
            &MainWindow::onNavButton);
}

QPushButton *MainWindow::makeNavButton(nt::Icon icon, const QString &text,
                                       int index)
{
    auto *b = new QPushButton(m_nav);
    b->setObjectName(QStringLiteral("navBtn"));
    b->setCheckable(true);
    b->setProperty("navIcon", QVariant::fromValue(int(icon)));
    b->setProperty("navText", text);
    b->setIcon(nt::icon(icon, nt::kMuted, 20));
    b->setIconSize(QSize(20, 20));
    b->setText(text);
    b->setToolTip(text);
    return b;
}

void MainWindow::onNavButton(int index)
{
    m_lastTab = m_currentTab;
    switch (index) {
    case 0:
        m_currentTab = 0;
        break;
    case 1:
        m_currentTab = 1;
        break;
    case 2:
        m_currentTab = 3;
        break;
    case 3:
        m_currentTab = 4;
        break;
    default:
        return;
    }
    m_pages->setCurrentIndex(m_currentTab);
    if (m_currentTab == 3)
        statusBar()->showMessage(QStringLiteral("Biblioteca: próximamente"), 2000);
    else if (m_currentTab == 4)
        statusBar()->showMessage(QStringLiteral("Ajustes: próximamente"), 2000);
}

void MainWindow::setupMiniBar()
{
    m_miniBar = new QWidget(this);
    m_miniBar->setObjectName(QStringLiteral("miniBar"));
    m_miniBar->setFixedHeight(64);

    auto *lay = new QVBoxLayout(m_miniBar);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_miniStack = new QStackedWidget(m_miniBar);

    auto *thin = new QWidget;
    auto *thinLay = new QHBoxLayout(thin);
    thinLay->setContentsMargins(12, 8, 12, 8);
    thinLay->setSpacing(10);
    m_miniTitle = new QLabel(QStringLiteral("Nada en reproducción"), thin);
    m_miniTitle->setStyleSheet(
        QStringLiteral("color:#8F8F8F; font-size:12px;"));
    m_miniPlayBtn = new QPushButton(thin);
    m_miniPlayBtn->setObjectName(QStringLiteral("iconBtn"));
    m_miniMuteBtn = new QPushButton(thin);
    m_miniMuteBtn->setObjectName(QStringLiteral("iconBtn"));
    m_miniPlayBtn->setIcon(nt::icon(nt::Icon::Play, nt::kText, 18));
    m_miniMuteBtn->setIcon(nt::icon(nt::Icon::Mute, nt::kMuted, 18));
    thinLay->addWidget(m_miniTitle, 1);
    thinLay->addWidget(m_miniMuteBtn);
    thinLay->addWidget(m_miniPlayBtn);

    auto *expanded = new QWidget;
    auto *expLay = new QVBoxLayout(expanded);
    expLay->setContentsMargins(12, 6, 12, 6);
    expLay->setSpacing(2);
    auto *row = new QHBoxLayout;
    row->setSpacing(10);
    m_miniProgress = new QProgressBar(expanded);
    m_miniProgress->setTextVisible(false);
    m_miniProgress->setFixedHeight(4);
    auto *playBtn = new QPushButton(expanded);
    playBtn->setObjectName(QStringLiteral("iconBtn"));
    playBtn->setIcon(nt::icon(nt::Icon::Play, nt::kText, 18));
    row->addWidget(m_miniProgress, 1);
    row->addWidget(playBtn);
    expLay->addLayout(row);

    m_miniStack->addWidget(thin);
    m_miniStack->addWidget(expanded);
    lay->addWidget(m_miniStack);

    connect(m_miniPlayBtn, &QPushButton::clicked, this, [this]() {
        m_player.playPause();
        updateMiniIcons();
    });
    connect(m_miniMuteBtn, &QPushButton::clicked, this, [this]() {
        m_player.toggleMute();
        updateMiniIcons();
    });
    connect(playBtn, &QPushButton::clicked, this, [this]() {
        m_player.playPause();
        m_lastTab = m_currentTab;
        m_pages->setCurrentIndex(2);
    });

    updateMiniBar();
}

void MainWindow::updateMiniBar()
{
    const bool active = m_player.isActive();
    m_miniStack->setCurrentIndex(active ? 1 : 0);
    if (active) {
        m_miniTitle->setText(m_player.current().title);
        m_miniTitle->setStyleSheet(
            QStringLiteral("color:#ECECEC; font-size:12px;"));
    }
    updateMiniIcons();
}

void MainWindow::updateMiniIcons()
{
    m_miniPlayBtn->setIcon(nt::icon(
        m_player.isPaused() ? nt::Icon::Play : nt::Icon::Pause,
        nt::kText, 18));
    m_miniMuteBtn->setIcon(nt::icon(
        m_player.muted() ? nt::Icon::Unmute : nt::Icon::Mute,
        nt::kMuted, 18));
}

void MainWindow::goHome()
{
    if (m_navButtons.value(0))
        m_navButtons[0]->setChecked(true);
}

void MainWindow::goSearch()
{
    if (m_navButtons.value(1))
        m_navButtons[1]->setChecked(true);
}

void MainWindow::goLibrary()
{
    if (m_navButtons.value(2))
        m_navButtons[2]->setChecked(true);
}

void MainWindow::goSettings()
{
    if (m_navButtons.value(3))
        m_navButtons[3]->setChecked(true);
}

void MainWindow::showPreviousTab()
{
    const int nav = m_lastTab == 3 ? 2 : m_lastTab == 4 ? 3 : m_lastTab;
    m_pages->setCurrentIndex(m_lastTab);
    if (m_navButtons.value(nav))
        m_navButtons[nav]->setChecked(true);
    m_currentTab = m_lastTab;
}

void MainWindow::openWatch(const QVector<VideoInfo> &videos, int startIndex)
{
    m_player.playQueue(videos, startIndex);
    m_watch->setWatchData(m_player.queue(), m_player.index());
    m_lastTab = m_currentTab;
    m_pages->setCurrentIndex(2);
}

void MainWindow::searchFor(const QString &query)
{
    m_search->startSearch(query);
    goSearch();
}

void MainWindow::playVideo(const QString &url)
{
    const QString id = Yt::videoIdFromUrl(url);
    if (id.isEmpty())
        return;
    VideoInfo v;
    v.id = id;
    openWatch(QVector<VideoInfo>{v}, 0);
}
