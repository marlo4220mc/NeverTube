#include "mainwindow.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include <QMouseEvent>

#include <functional>

#include "models.h"
#include "playerwidget.h"
#include "../src/debug.h"
#include "ui/format.h"
#include "ui/icons.h"
#include "ui/pages/home_page.h"
#include "ui/pages/channel_page.h"
#include "ui/pages/library_page.h"
#include "ui/pages/playlist_page.h"
#include "ui/pages/settings_page.h"
#include "ui/pages/history_page.h"
#include "ui/pages/search_page.h"
#include "ui/pages/watch_page.h"
#include "ui/theme.h"
#include "ui/thumbnail_cache.h"
#include "yt.h"

namespace {

// Botón de la barra de navegación inferior. Usa los PNG adjuntos por estado
// (inicio/buscar/biblioteca/ajustes × inactivo/activo/hover/presionado/
// deshabilitado). El QSS no puede cambiar qproperty-icon dinámicamente, así
// que el icono se elige aquí según el estado real del botón.
class NavButton : public QToolButton
{
public:
    using QToolButton::QToolButton;

    // Estado visible (prioridad alta→baja): disabled > pressed > hover >
    // checked > normal. El rojo NeverTube está en los propios PNG.
    void updateIcon()
    {
        const QString key = property("navKey").toString();
        if (key.isEmpty())
            return;
        QString state = QStringLiteral("inactive");
        if (!isEnabled())
            state = QStringLiteral("disabled");
        else if (m_pressed)
            state = QStringLiteral("pressed");
        else if (underMouse())
            state = QStringLiteral("hover");
        else if (isChecked())
            state = QStringLiteral("active");
        setIcon(QIcon(
            QStringLiteral(":/nav/%1_%2.png").arg(key, state)));
        setIconSize(QSize(33, 33));
    }

protected:
    void enterEvent(QEvent *event) override
    {
        QToolButton::enterEvent(event);
        updateIcon();
    }
    void leaveEvent(QEvent *event) override
    {
        QToolButton::leaveEvent(event);
        updateIcon();
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        QToolButton::mousePressEvent(event);
        if (event->button() == Qt::LeftButton) {
            m_pressed = true;
            updateIcon();
        }
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QToolButton::mouseReleaseEvent(event);
        m_pressed = false;
        updateIcon();
    }
    void checkStateSet() override
    {
        QToolButton::checkStateSet();
        updateIcon();
    }
    void changeEvent(QEvent *event) override
    {
        QToolButton::changeEvent(event);
        if (event->type() == QEvent::EnabledChange)
            updateIcon();
    }

private:
    bool m_pressed = false;
};

// Barra del mini player: cualquier click en zona no-botón emite la vuelta a la
// página Player. Los botones internos consumen su propio click y no llegan aquí
// (la propagación de eventos de ratón no aceptados sube al widget padre).
class MiniPlayerBar : public QWidget
{
public:
    using QWidget::QWidget;
    std::function<void()> onActivate;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && onActivate)
            onActivate();
        QWidget::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            && onActivate) {
            onActivate();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }
};

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_db()
    , m_yt(new Yt(this))
    , m_homeThumbs(new ThumbnailCache(QStringLiteral("home"), 64 * 1024 * 1024, this))
    , m_globalThumbs(new ThumbnailCache(QStringLiteral("global"), 32 * 1024 * 1024, this))
    , m_libraryThumbs(new ThumbnailCache(QStringLiteral("library"), 32 * 1024 * 1024, this))
    , m_playerWidget(new PlayerWidget(this))
{
    m_db.open();
    m_yt->setVisitorStorage(&m_settings);
    m_elapsed.start();
    m_player.setPlayer(m_playerWidget);
    // Filtro a nivel de aplicación: recibe todos los KeyPress aunque el foco
    // esté en un widget profundo (QScrollArea, listas…), no solo en MainWindow.
    QCoreApplication::instance()->installEventFilter(this);

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
    m_home = new HomePage(m_yt, &m_subs, &m_history, &m_discoverRadios,
                          &m_videoCache, m_homeThumbs, &m_settings,
                          &m_homeCache, this);
    m_search = new SearchPage(m_yt, &m_settings, m_globalThumbs, this);
    m_channel = new ChannelPage(m_yt, m_globalThumbs, &m_subs, this);
    m_watch = new WatchPage(m_yt, &m_player, m_playerWidget, &m_videoCache,
                            &m_fav, &m_starred, &m_watchLater, &m_subs,
                            &m_settings, m_globalThumbs, &m_playlists, this);
    m_library = new LibraryPage(m_yt, &m_fav, &m_starred, &m_watchLater,
                                &m_history, &m_subs, &m_videoCache, m_libraryThumbs,
                                &m_playlists, this);
    m_playlist = new PlaylistPage(&m_playlists, &m_videoCache, m_libraryThumbs, this);
    m_historyPage = new HistoryPage(&m_history, &m_videoCache, m_libraryThumbs, this);
    m_settingsPage = new SettingsPage(&m_settings, this);

    m_pages->addWidget(m_home);
    m_pages->addWidget(m_search);
    m_pages->addWidget(m_channel);
    m_pages->addWidget(m_watch);
    m_pages->addWidget(m_library);
    m_pages->addWidget(m_settingsPage);
    m_pages->addWidget(m_playlist);
    m_pages->addWidget(m_historyPage);

    connect(m_home, &HomePage::openRequested, this, &MainWindow::openWatch);
    connect(m_home, &HomePage::resumeRequested, this,
            &MainWindow::openWatchResume);
    connect(m_search, &SearchPage::openRequested, this, &MainWindow::openWatch);
    connect(m_search, &SearchPage::channelRequested, this,
            [this](const QString &channelId) {
                m_lastTab = m_currentTab;
                m_currentTab = 2;
                m_channel->openChannel(channelId);
                m_pages->setCurrentWidget(m_channel);
            });
    connect(m_channel, &ChannelPage::backRequested, this,
            &MainWindow::showPreviousTab);
    connect(m_channel, &ChannelPage::openRequested, this,
            &MainWindow::openWatch);
    connect(m_watch, &WatchPage::backRequested, this,
            &MainWindow::showPreviousTab);
    connect(m_watch, &WatchPage::fullscreenToggled, this,
            [this](bool on) {
                if (on) {
                    m_nav->hide();
                    statusBar()->hide();
                    showFullScreen();
                } else {
                    m_nav->show();
                    statusBar()->show();
                    showNormal();
                    updateMiniPlayer();
                }
            });
    connect(m_watch, &WatchPage::channelRequested, this,
            [this](const QString &channelId, const QString &) {
                m_lastTab = m_currentTab;
                m_currentTab = 2;
                m_channel->openChannel(channelId);
                m_pages->setCurrentWidget(m_channel);
            });
    connect(m_library, &LibraryPage::channelRequested, this,
            [this](const QString &channelId, const QString &) {
                m_lastTab = m_currentTab;
                m_currentTab = 2;
                m_channel->openChannel(channelId);
                m_pages->setCurrentWidget(m_channel);
            });
    connect(m_library, &LibraryPage::openRequested, this,
            &MainWindow::openWatch);
    connect(m_library, &LibraryPage::resumeRequested, this,
            &MainWindow::openWatchResume);
    connect(m_library, &LibraryPage::playlistRequested, this,
            [this](const QString &playlistId, const QString &title) {
                openPlaylist(playlistId, title);
            });
    connect(m_playlist, &PlaylistPage::backRequested, this,
            &MainWindow::showPreviousTab);
    connect(m_playlist, &PlaylistPage::openRequested, this,
            &MainWindow::openWatch);
    connect(m_library, &LibraryPage::historyAllRequested, this,
            &MainWindow::openHistory);
    connect(m_historyPage, &HistoryPage::backRequested, this,
            &MainWindow::showPreviousTab);
    connect(m_historyPage, &HistoryPage::resumeRequested, this,
            &MainWindow::openWatchResume);
    connect(m_historyPage, &HistoryPage::historyChanged, this, [this]() {
        if (m_home)
            m_home->invalidateDiscover();
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
            [this](const QString &) { updateMiniPlayer(); });
    connect(&m_player, &PlayerController::phaseChanged, this,
            [this](const QString &phase) {
                // "Generación al reproducir": al confirmarse la reproducción
                // (mpv -> playing) la semilla del vídeo actual da la radio que
                // se guarda para Descubre. Solo en modo NO Radio: en modo
                // Radio la radio de "A continuación" ya la pidió
                // WatchPage::triggerRadio y este radioReady también se guarda
                // (onRadioReadyDiscover), evitando duplicar la cola.
                if (phase != QLatin1String("playing"))
                    return;
                const VideoInfo cur = m_player.current();
                if (cur.id.isEmpty() || cur.id.size() != 11)
                    return;
                if (nextMode() == NextMode::Radio)
                    return;
                if (m_discoverRadios.hasRadio(cur.id))
                    return;
                if (m_radioSeedsRequested.contains(cur.id))
                    return;
                m_radioSeedsRequested.insert(cur.id);
                NT_DEBUG("E2E discover play-seed=%s\n",
                         cur.id.toUtf8().constData());
                m_yt->radioVideos(cur.id);
            });
    connect(&m_player, &PlayerController::currentChanged, this,
            [this]() {
                updateMiniPlayer();
                m_home->setCurrentVideoId(m_player.current().id);
                // Cuando cambia el video, copiar a cache del Home si es necesario
            });
    connect(&m_player, &PlayerController::metadataChanged, this,
            [this]() { updateMiniBar(); });
    connect(&m_player, &PlayerController::positionChanged, this,
            [this](double pos, double dur) {
                if (m_miniProgress) {
                    m_miniProgress->setRange(0, dur > 0 ? qRound(dur) : 0);
                    m_miniProgress->setValue(qRound(pos));
                }
                if (m_miniTime) {
                    m_miniTime->setText(
                        QStringLiteral("%1 / %2")
                            .arg(fmtTime(pos))
                            .arg(fmtTime(dur)));
                }
            });
    connect(m_pages, &QStackedWidget::currentChanged, this,
            [this](int) { updateMiniPlayer(); });
    connect(m_yt, &Yt::radioReady, this, &MainWindow::onRadioReadyDiscover);
    connect(m_homeThumbs, &ThumbnailCache::ready, this, [this](const QString &key) {
        const QString cur = QStringLiteral("v_%1").arg(m_player.current().id);
        if (key.startsWith(QLatin1String("v_")) && key == cur)
            setMiniThumb();
    });
    connect(m_globalThumbs, &ThumbnailCache::ready, this, [this](const QString &key) {
        const QString cur = QStringLiteral("v_%1").arg(m_player.current().id);
        if (key.startsWith(QLatin1String("v_")) && key == cur)
            setMiniThumb();
    });
}

MainWindow::~MainWindow() = default;

bool MainWindow::handleShortcut(QKeyEvent *event)
{
    // No robar teclas cuando se está escribiendo en un campo de texto.
    if (const auto *fw = focusWidget()) {
        if (qobject_cast<const QLineEdit *>(fw)
            || qobject_cast<const QTextEdit *>(fw)
            || qobject_cast<const QPlainTextEdit *>(fw))
            return false;
    }

    const Qt::KeyboardModifiers mods = event->modifiers()
        & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    if (mods)
        return false;

    switch (event->key()) {
    case Qt::Key_Space:
        // El espacio nunca debe activar un botón con foco: está reservado
        // para play/pausa. Si el foco está en un botón, lo consumimos igual
        // (con o sin reproductor) para que no se pulse con la barra.
        if (qobject_cast<QAbstractButton *>(focusWidget())) {
            if (m_player.isActive())
                m_player.playPause();
            return true;
        }
        if (!m_player.isActive())
            return false;
        m_player.playPause();
        return true;
    case Qt::Key_K:
        if (!m_player.isActive())
            return false;
        m_player.playPause();
        return true;
    case Qt::Key_Left:
        if (!m_player.isActive())
            return false;
        m_player.seek(m_player.position() - 5.0);
        return true;
    case Qt::Key_Right:
        if (!m_player.isActive())
            return false;
        m_player.seek(m_player.position() + 5.0);
        return true;
    case Qt::Key_Up:
        if (!m_player.isActive())
            return false;
        m_player.setVolume(qMin(m_player.volume() + 5.0, 100.0));
        updateMiniIcons();
        return true;
    case Qt::Key_Down:
        if (!m_player.isActive())
            return false;
        m_player.setVolume(qMax(m_player.volume() - 5.0, 0.0));
        updateMiniIcons();
        return true;
    case Qt::Key_M:
        if (!m_player.isActive())
            return false;
        m_player.toggleMute();
        updateMiniIcons();
        return true;
    case Qt::Key_F:
        m_watch->toggleFullscreen();
        return true;
    case Qt::Key_Escape:
        if (!isFullScreen())
            return false;
        m_watch->toggleFullscreen();
        return true;
    default:
        return false;
    }
}

void MainWindow::keyPressEvent(QKeyEvent *event)
{
    if (!handleShortcut(event))
        QMainWindow::keyPressEvent(event);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress && watched->isWidgetType()) {
        if (auto *ke = static_cast<QKeyEvent *>(event))
            if (handleShortcut(ke))
                return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::setupNav()
{
    m_nav = new QWidget(this);
    m_nav->setObjectName(QStringLiteral("navBottom"));
    m_nav->setFixedHeight(66);

    auto *lay = new QHBoxLayout(m_nav);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto *group = new QButtonGroup(m_nav);
    group->setExclusive(true);

    auto addBtn = [&](const QString &key, const QString &text, int index) {
        auto *b = new NavButton(m_nav);
        b->setObjectName(QStringLiteral("navBtn"));
        b->setCheckable(true);
        b->setProperty("navKey", key);
        b->setText(text);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        b->setIconSize(QSize(33, 33));
        b->updateIcon();
        group->addButton(b, index);
        lay->addWidget(b, 1);
        m_navButtons.append(b);
    };
    addBtn(QStringLiteral("home"), QStringLiteral("Inicio"), 0);
    addBtn(QStringLiteral("search"), QStringLiteral("Buscar"), 1);
    addBtn(QStringLiteral("library"), QStringLiteral("Biblioteca"), 2);
    addBtn(QStringLiteral("settings"), QStringLiteral("Ajustes"), 3);

    connect(group, QOverload<int>::of(&QButtonGroup::idClicked), this,
            &MainWindow::onNavButton);
}

void MainWindow::onNavButton(int index)
{
    m_lastTab = m_currentTab;
    switch (index) {
    case 0:
        m_currentTab = 0;
        m_home->refreshDynamic();
        break;
    case 1:
        m_currentTab = 1;
        break;
    case 2:
        m_currentTab = 4;
        break;
    case 3:
        m_currentTab = 5;
        break;
    default:
        return;
    }
    m_pages->setCurrentIndex(m_currentTab);
    if (m_currentTab == 4)
        m_library->reload();
    else if (m_currentTab == 5) {
        m_settingsPage->setVisible(true);
    }
}

void MainWindow::setupMiniBar()
{
    auto *bar = new MiniPlayerBar(this);
    m_miniBar = bar;
    m_miniBar->setObjectName(QStringLiteral("miniBar"));
    m_miniBar->setCursor(Qt::PointingHandCursor);
    m_miniBar->setFocusPolicy(Qt::TabFocus);
    bar->onActivate = [this]() { goToPlayerPage(); };

    auto *lay = new QVBoxLayout(m_miniBar);
    lay->setContentsMargins(12, 8, 12, 6);
    lay->setSpacing(4);

    auto *row = new QHBoxLayout;
    row->setSpacing(10);

    m_miniThumb = new QLabel(m_miniBar);
    m_miniThumb->setFixedSize(112, 63);
    m_miniThumb->setAlignment(Qt::AlignCenter);
    m_miniThumb->setStyleSheet(
        QStringLiteral("background:#262626; border-radius:6px; color:#8F8F8F; "
                       "font-size:10px;"));
    m_miniThumb->setTextInteractionFlags(Qt::NoTextInteraction);
    row->addWidget(m_miniThumb);

    auto *textBox = new QVBoxLayout;
    textBox->setSpacing(1);
    m_miniTitle = new QLabel(QStringLiteral("Nada en reproducción"), m_miniBar);
    m_miniTitle->setStyleSheet(
        QStringLiteral("color:#ECECEC; font-size:12px; font-weight:600;"));
    m_miniTitle->setTextInteractionFlags(Qt::NoTextInteraction);
    m_miniTime = new QLabel(QStringLiteral("0:00 / 0:00"), m_miniBar);
    m_miniTime->setStyleSheet(QStringLiteral("color:#8F8F8F; font-size:11px;"));
    m_miniTime->setTextInteractionFlags(Qt::NoTextInteraction);
    textBox->addWidget(m_miniTitle);
    textBox->addWidget(m_miniTime);
    row->addLayout(textBox, 1);

    m_miniPlayBtn = new QPushButton(m_miniBar);
    m_miniPlayBtn->setObjectName(QStringLiteral("iconBtn"));
    m_miniPlayBtn->setCursor(Qt::PointingHandCursor);
    m_miniPlayBtn->setIcon(nt::icon(nt::Icon::Play, nt::kText, 20));
    m_miniPlayBtn->setToolTip(QStringLiteral("Reproducir / Pausar (K)"));
    row->addWidget(m_miniPlayBtn);

    m_miniMuteBtn = new QPushButton(m_miniBar);
    m_miniMuteBtn->setObjectName(QStringLiteral("iconBtn"));
    m_miniMuteBtn->setCursor(Qt::PointingHandCursor);
    m_miniMuteBtn->setIcon(nt::icon(nt::Icon::Mute, nt::kMuted, 16));
    m_miniMuteBtn->setToolTip(QStringLiteral("Silenciar (M)"));
    row->addWidget(m_miniMuteBtn);

    m_miniCloseBtn = new QPushButton(m_miniBar);
    m_miniCloseBtn->setObjectName(QStringLiteral("iconBtn"));
    m_miniCloseBtn->setCursor(Qt::PointingHandCursor);
    m_miniCloseBtn->setIcon(nt::icon(nt::Icon::Clear, nt::kMuted, 16));
    m_miniCloseBtn->setToolTip(QStringLiteral("Cerrar y detener"));
    row->addWidget(m_miniCloseBtn);

    lay->addLayout(row);

    m_miniProgress = new QProgressBar(m_miniBar);
    m_miniProgress->setTextVisible(false);
    m_miniProgress->setFixedHeight(3);
    lay->addWidget(m_miniProgress);

    connect(m_miniPlayBtn, &QPushButton::clicked, this, [this]() {
        m_player.playPause();
        updateMiniIcons();
    });
    connect(m_miniMuteBtn, &QPushButton::clicked, this, [this]() {
        m_player.toggleMute();
        updateMiniIcons();
    });
    connect(m_miniCloseBtn, &QPushButton::clicked, this, [this]() {
        m_player.stop();
        updateMiniPlayer();
    });

    updateMiniPlayer();
}

// Regla de visibilidad del mini player: nunca a la vez con la página Player.
// Se muestra al salir de Player con reproducción activa/pausada y se oculta al
// volver a Player o si se detiene la reproducción (isActive()==false).
void MainWindow::updateMiniPlayer()
{
    const bool watching = m_pages && m_pages->currentIndex() == 3;
    const bool show = m_player.isActive() && !watching;
    NT_DEBUG("E2E miniPlayer wall=%lld show=%d page=%d active=%d phase=%s\n",
             static_cast<long long>(m_elapsed.elapsed()),
             show ? 1 : 0, m_pages ? m_pages->currentIndex() : -1,
             m_player.isActive() ? 1 : 0,
             m_player.phase().toUtf8().constData());
    if (m_miniBar)
        m_miniBar->setVisible(show);
    if (m_home)
        m_home->setMiniPlayerVisible(show);
    if (show)
        updateMiniBar();
}

// Al pulsar en cualquier zona del mini bar (excepto sus botones) se vuelve al
// Player completo sin volver a resolver el stream: la reproducción ya sigue
// viva en el mismo PlayerController durante la navegación.
void MainWindow::goToPlayerPage()
{
    m_lastTab = m_currentTab;
    m_currentTab = 3;
    m_pages->setCurrentIndex(3);
}

void MainWindow::updateMiniBar()
{
    if (!m_miniBar || !m_miniBar->isVisible())
        return;
    const VideoInfo v = m_player.current();
    if (v.id.isEmpty()) {
        m_miniTitle->setText(QStringLiteral("Nada en reproducción"));
        m_miniThumb->setText(QString());
        m_miniProgress->setRange(0, 0);
        return;
    }
    QString line = v.title;
    if (!line.isEmpty() && !v.channel.isEmpty())
        line += QStringLiteral("\n");
    if (!v.channel.isEmpty())
        line += v.channel;
    m_miniTitle->setText(line);

    QString thumbUrl = v.thumbnailUrl;
    if (thumbUrl.isEmpty() && v.id.size() == 11)
        thumbUrl = QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg")
                       .arg(v.id);
    m_globalThumbs->request(QStringLiteral("v_%1").arg(v.id), thumbUrl);
    setMiniThumb();
    updateMiniIcons();
}

void MainWindow::setMiniThumb()
{
    if (!m_miniThumb)
        return;
    const QString key = QStringLiteral("v_%1").arg(m_player.current().id);
    QPixmap pm = m_homeThumbs->pixmap(key);
    if (pm.isNull())
        pm = m_globalThumbs->pixmap(key);
    if (pm.isNull()) {
        m_miniThumb->setText(QStringLiteral("sin miniatura"));
        return;
    }
    m_miniThumb->setText(QString());
    m_miniThumb->setStyleSheet(
        QStringLiteral("background:transparent; border-radius:6px;"));
    m_miniThumb->setPixmap(
        pm.scaled(m_miniThumb->size(), Qt::KeepAspectRatioByExpanding,
                  Qt::SmoothTransformation));
}

void MainWindow::updateMiniIcons()
{
    if (!m_miniPlayBtn || !m_miniMuteBtn)
        return;
    m_miniPlayBtn->setIcon(nt::icon(
        m_player.isPaused() ? nt::Icon::Play : nt::Icon::Pause,
        nt::kText, 20));
    m_miniMuteBtn->setIcon(nt::icon(
        m_player.muted() ? nt::Icon::Unmute : nt::Icon::Mute,
        nt::kMuted, 16));
}

void MainWindow::goHome()
{
    if (m_navButtons.value(0))
        m_navButtons[0]->setChecked(true);
}

void MainWindow::goSearch()
{
    m_currentTab = 1;
    m_pages->setCurrentIndex(1);
    if (m_navButtons.value(1))
        m_navButtons[1]->setChecked(true);
}

void MainWindow::goLibrary()
{
    if (m_navButtons.value(2))
        m_navButtons[2]->setChecked(true);
    m_lastTab = m_currentTab;
    m_currentTab = 4;
    m_pages->setCurrentIndex(4);
    m_library->reload();
}

void MainWindow::openLibrary()
{
    goLibrary();
}

void MainWindow::openPlaylist(const QString &playlistId, const QString &title)
{
    m_lastTab = m_currentTab;
    m_currentTab = 6;
    m_playlist->openPlaylist(playlistId, title);
    m_pages->setCurrentIndex(6);
}

void MainWindow::openHistory()
{
    m_lastTab = m_currentTab;
    m_currentTab = 7;
    m_historyPage->reload();
    m_pages->setCurrentIndex(7);
}

void MainWindow::goSettings()
{
    if (m_navButtons.value(3))
        m_navButtons[3]->setChecked(true);
}

void MainWindow::showPreviousTab()
{
    int dest = m_lastTab;
    if (dest == m_currentTab || dest < 0 || dest >= m_pages->count())
        dest = 0;
    m_pages->setCurrentIndex(dest);
    m_currentTab = dest;
    if (dest == 0)
        m_home->refreshDynamic();
    // Canal (2) y reproductor (3) no tienen botón de nav: no marcar ninguno.
    if (m_currentTab == 2 || m_currentTab == 3)
        return;
    const int nav = m_currentTab == 4 ? 2 : m_currentTab == 5 ? 3 : m_currentTab;
    if (nav < m_navButtons.size())
        m_navButtons[nav]->setChecked(true);
}

void MainWindow::openWatch(const QVector<VideoInfo> &videos, int startIndex)
{
    openWatchAt(videos, startIndex, 0);
}

// Captura TODA radio que llegue (del triggerRadio de "A continuación" en modo
// Radio o de la generación por marcha del reproductor) y la guarda como fuente
// de Descubre. Idempotente por semilla: una radio por vídeo reproducido. Los
// metadatos de sus vídeos se meten en el caché "videos" para que Descubre
// pueda resolverlos sin red.
void MainWindow::onRadioReadyDiscover(const QString &videoId,
                                      const QVector<VideoInfo> &videos)
{
    if (videoId.isEmpty() || videoId.size() != 11)
        return;
    if (videos.isEmpty()) {
        m_radioSeedsRequested.remove(videoId);
        return;
    }
    if (m_discoverRadios.hasRadio(videoId))
        return;
    QVector<VideoInfo> valid;
    for (const VideoInfo &v : videos) {
        if (v.id.size() != 11)
            continue;
        m_videoCache.upsert(v);
        valid.append(v);
    }
    m_radioSeedsRequested.remove(videoId);
    if (valid.isEmpty())
        return;
    m_discoverRadios.saveRadio(videoId, valid);
    // El sistema ya generó radios alguna vez: Descubre vacío (p. ej. tras
    // "Borrar todo" el historial) no vuelve a auto-sembrarse solo.
    m_settings.set(QStringLiteral("discover_seeded"), QStringLiteral("1"));
    NT_DEBUG("E2E discover radio guardada seed=%s n=%d total=%d\n",
             videoId.toUtf8().constData(), (int)valid.size(),
             m_discoverRadios.count());
    if (m_home)
        m_home->refreshDynamic();
}

void MainWindow::openWatchResume(const QVector<VideoInfo> &videos,
                                 int startIndex, qlonglong startPositionMs)
{
    openWatchAt(videos, startIndex, startPositionMs);
}

void MainWindow::openWatchAt(const QVector<VideoInfo> &videos, int startIndex,
                             qlonglong startPositionMs)
{
    // Deja de sembrar radios de Descubre: al abrir un vídeo la radio de
    // "A continuación" pasa a ser el único consumidor de fetchRadio()
    // (single-flight en Innertube).
    if (m_home)
        m_home->interruptDiscover();
    const NextMode mode = nextMode();
    if (mode == NextMode::Radio) {
        // Modo radio: abrir solo con el vídeo clicado y generar la cola del
        // mix RDAMVM para "A continuación". Click explícito desde fuera de la
        // lista (search, home, channel) dispara la radio; dentro de la lista
        // no se regenera.
        m_player.playQueue(QVector<VideoInfo>{videos.at(startIndex)}, 0,
                           startPositionMs);
        m_watch->setWatchData(m_player.queue(), m_player.index());
        const QString &id = videos.at(startIndex).id;
        // La radio de este vídeo ya se generó antes: se reusa desde la BD para
        // "A continuación" sin volver a pedirla a YouTube.
        bool reused = false;
        if (id.size() == 11 && m_discoverRadios.hasRadio(id)
            && !m_discoverRadios.radioVideoIdsForSeed(id).isEmpty()) {
            const QStringList saved = m_discoverRadios.radioVideoIdsForSeed(id);
            const QVector<VideoInfo> fromCache = m_videoCache.getMany(saved);
            if (!fromCache.isEmpty()) {
                NT_DEBUG("E2E nextUp radio reuse id=%s n=%d\n",
                         id.toUtf8().constData(), (int)fromCache.size());
                m_player.appendVideos(fromCache);
                reused = true;
            }
        }
        if (!reused)
            m_watch->triggerRadio(id);
    } else {
        m_player.playQueue(videos, startIndex, startPositionMs);
        m_watch->setWatchData(m_player.queue(), m_player.index());
    }
    m_lastTab = m_currentTab;
    m_currentTab = 3;
    m_pages->setCurrentIndex(3);
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

void MainWindow::openChannel(const QString &channelId)
{
    m_lastTab = m_currentTab;
    m_currentTab = 2;
    m_channel->openChannel(channelId);
    m_pages->setCurrentWidget(m_channel);
}
