#include "mainwindow.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
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

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QProcess>
#include <QTimer>
#include <functional>

#include "models.h"
#include "hwprobe.h"
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
#include "ui/video_list_view.h"
#include "yt.h"
#include "yt_innertube.h"

namespace {

// Versión de la lógica del sondeo HW (HwProbe). Al subir esta versión la app
// re-sondea la GPU UNA vez al arrancar, aunque haya un perfil persistido: así
// un falso positivo escrito por una versión antigua (p.ej. av1-hw del i5 con
// qsv-copy) se autocorrige sin tener que re-sondear en cada arranque (algo que
// penalizaría la máquina de pruebas, donde el sondeo es lento).
constexpr int kCodecProbeVersion = 2;

// Anti-rebote y presupuesto de la escalera de descenso de perfil (evidencia de
// códec/HW). Un mismo episodio no debe degradar varias veces seguidas (el
// decoder puede emitir "software" repetido); y como mucho se descienden 3
// peldaños por sesión (av1-hw -> vp9-hw -> h264-hw -> soft-all).
constexpr qint64 kDescendCooldownMs = 15000;
constexpr int kDescendMaxPerSession = 3;

// Botón de la barra de navegación inferior. Usa los PNG adjuntos por estado
// (inicio/buscar/biblioteca/ajustes × inactivo/activo/hover/presionado/
// deshabilitado). El QSS no puede cambiar qproperty-icon dinámicamente, así
// que el icono se elige aquí según el estado real del botón. Los estados de
// acento (activo/hover/presionado) usan PNGs pre-pintados por color de acento
// (nav/<key>_<state>_<suffix>.png, generados en tools/gen_nav_accent.cpp): sin
// retint en runtime — el cambio de acento en caliente solo re-evalúa el
// sufijo, y la clase de bugs del retint (alfa del "cuadrado negro", acento
// congelado en estático) no puede existir. Inactivo y deshabilitado usan sus
// grises nativos del PNG.
class NavButton : public QToolButton
{
public:
    using QToolButton::QToolButton;

    // Estado visible (prioridad alta→baja): disabled > pressed > hover >
    // checked > normal. El color de acento se aplica en los estados activos.
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
        // Estados de acento: PNG pre-pintado por sufijo del acento vigente;
        // inactivo/deshabilitado usan sus grises nativos sin sufijo.
        const bool accentState = state == QLatin1String("active")
                                 || state == QLatin1String("hover")
                                 || state == QLatin1String("pressed");
        const QString path = accentState
            ? QStringLiteral(":/nav/%1_%2_%3.png")
                  .arg(key, state, nt::accentSuffix())
            : QStringLiteral(":/nav/%1_%2.png").arg(key, state);
        setIcon(QIcon(path));
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

// Migra los nombres de perfil de códec antiguos (detección reactiva) a los del
// sondeo offline de HwProbe. Se persiste la migración para no repetirla.
QString normalizeCodecProfile(const QString &raw)
{
    if (raw == QLatin1String("modern"))
        return QStringLiteral("av1-hw");
    if (raw == QLatin1String("vp9"))
        return QStringLiteral("vp9-hw");
    if (raw == QLatin1String("legacy"))
        return QStringLiteral("h264-hw");
    if (raw == QLatin1String("nohw"))
        return QStringLiteral("soft-all");
    return raw;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_db()
    , m_yt(new Yt(this))
    , m_homeThumbs(new ThumbnailCache(QStringLiteral("home"), 64 * 1024 * 1024, this))
    , m_globalThumbs(new ThumbnailCache(QStringLiteral("global"), 64 * 1024 * 1024, this))
    , m_libraryThumbs(new ThumbnailCache(QStringLiteral("library"), 48 * 1024 * 1024, this))
    , m_playerWidget(new PlayerWidget(this))
{
    m_db.open();
    m_yt->setVisitorStorage(&m_settings);
    // Auto-reinicio por sesión gateada: si esta instancia vino de un reinicio
    // de recuperación (guard armado en settings), el último vídeo debe reanudar
    // donde se cortó. El guard NO se desarma al abrir: solo cuando la
    // reproducción se estabiliza 5 min (recoveryHealthy) o al agotar sin éxito
    // (onRecoveryExhausted), para que un reinicio nunca encadene otro.
    if (m_settings.getInt(QStringLiteral("gate_restart_armed"), 0) == 1)
        m_gateResumeMs = m_settings.getInt(QStringLiteral("gate_restart_pos"), 0);
    connect(&m_player, &PlayerController::recoveryExhausted, this,
            &MainWindow::onRecoveryExhausted);
    connect(&m_player, &PlayerController::recoveryHealthy, this,
            &MainWindow::onRecoveryHealthy);
    // Perfil de códec de vídeo. HwProbe sondea la GPU con micro-clips offline y
    // deja un perfil persistido (av1-hw|vp9-hw|h264-hw|soft-all). Si no hay
    // resultado se aplica soft-all (conservador) sin persistir, y mientras se
    // sondea no se reproduce (la petición se encola). El perfil se decide y
    // persiste SOLO en el arranque: la red reactiva (streamDecodeInfoReady)
    // queda como traza de diagnóstico y ya NO degrada el perfil en caliente
    // (la detección correcta al inicio es responsabilidad de HwProbe).
    const QString rawProfile = m_settings.getString(
        QStringLiteral("codec_profile"), QString());
    const QString savedProfile = normalizeCodecProfile(rawProfile);
    if (!rawProfile.isEmpty() && savedProfile != rawProfile)
        m_settings.set(QStringLiteral("codec_profile"), savedProfile);
#ifdef NEVERTUBE_NO_MPV
    // Build x86 sin mpv (Fase 5): no hay HwProbe (sondea con mpv). El perfil
    // se decide por plataforma — el target x86 va por DXVA2, H.264 es el techo; los
    // av1/vp9 irán a SW y la escalera reactiva (streamDecodeInfoReady ->
    // hwdecCurrent()=="no") ya los descendería en caliente. Sin valor
    // guardado = h264-hw, persistido para que Ajustes lo muestre.
    if (savedProfile.isEmpty()) {
        m_settings.set(QStringLiteral("codec_profile"),
                       QStringLiteral("h264-hw"));
        applyCodecProfile(QStringLiteral("h264-hw"), false);
    } else {
        applyCodecProfile(savedProfile, false);
    }
#else
    // El perfil se re-sondea al arranque si no hay valor persistido o si fue
    // escrito por otra versión del sondeo (codec_probe_v). El valor guardado
    // se usa como fallback mientras se sondea; el veredicto fresco lo
    // reemplaza. No se reproduce hasta tener el veredicto (cola pendiente).
    const int probeVersion = m_settings
                                 .getString(QStringLiteral("codec_probe_v"),
                                            QString())
                                 .toInt();
    if (probeVersion != kCodecProbeVersion || savedProfile.isEmpty()) {
        if (!savedProfile.isEmpty())
            applyCodecProfile(savedProfile, false);
        else
            applyCodecProfile(QStringLiteral("soft-all"), false);
        startHwProbe();
    } else {
        applyCodecProfile(savedProfile, false);
    }
#endif
    connect(m_playerWidget, &PlayerWidget::streamDecodeInfoReady, this,
            [this](const QString &codec, const QString &hwdecBackend) {
                // hwdec-current llega "vacío" en el primer video-params (mpv
                // aún no activó el decodificador) y "no" cuando el decoder se
                // sabe en software; el widget reintenta hasta tenerlo resuelto.
                //
                // Escalera de descenso: si el perfil ACTIVO promete hardware y
                // mpv decodifica en SOFTWARE, el veredicto del arranque no se
                // sostiene en la reproducción -> se baja un peldaño y se
                // RE-RESUELVE con el perfil nuevo (reloadCurrent sin rotar
                // sesión: un problema de códec no es de sesión).
                //
                // Regla dura: NUNCA por red. Un 403, un visitorData vencido o
                // un atasco de caché no cambian hwdec-current; esta señal solo
                // existe con frames decodificados en software. Sin evidencia de
                // códec/HW (arriba: hwdecBackend != "no") no se toca nada.
                if (hwdecBackend != QLatin1String("no")) {
                    // Decodifica en hardware: el perfil acertó -> una racha
                    // sana reinicia el presupuesto del descenso.
                    m_descendAttempts = 0;
                    return;
                }
                const QString lower = hwprobe::lowerProfile(m_activeProfile);
                NT_DEBUG("E2E codec_profile reactivo: perfil=%s cod=%s hw=no "
                         "-> bajar=%s\n",
                         m_activeProfile.toUtf8().constData(),
                         codec.toUtf8().constData(),
                         lower.isEmpty() ? "(nada)" : lower.toUtf8().constData());
                // soft-all es el suelo; un perfil desconocido tampoco se toca.
                if (lower.isEmpty())
                    return;
                if (m_descendAttempts >= kDescendMaxPerSession)
                    return;
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (m_lastDescendMs > 0
                    && now - m_lastDescendMs < kDescendCooldownMs)
                    return; // anti-rebote: el mismo episodio no baja varias veces
                ++m_descendAttempts;
                m_lastDescendMs = now;
                // Efímero: se aplica EN MEMORIA (persist=false). Un fallo
                // puntual no debe "condenar" la máquina a un perfil menor
                // (Ajustes -> "Volver a detectar" lo restaura).
                applyCodecProfile(lower, /*persist=*/false);
                NT_DEBUG("E2E codec_profile descenso=%s intento=%d -> reload\n",
                         lower.toUtf8().constData(), m_descendAttempts);
                m_player.reloadCurrent();
            });
    m_elapsed.start();
    m_player.setPlayer(m_playerWidget);
    // Filtro a nivel de aplicación: recibe todos los KeyPress aunque el foco
    // esté en un widget profundo (QScrollArea, listas…), no solo en MainWindow.
    QCoreApplication::instance()->installEventFilter(this);

    const QString savedQ = m_settings.getString(
        QStringLiteral("quality"), QStringLiteral("auto"));
    if (savedQ == QStringLiteral("auto")) {
        int autoDef = m_settings.getInt(QStringLiteral("quality_auto"), 360);
        m_player.setMaxHeight(autoDef);
    } else {
        int qh = savedQ.toInt();
        if (qh > 0)
            m_player.setMaxHeight(qh);
        else
            m_player.setMaxHeight(720);
    }
    // Sin HW: nunca pedir más de 1080p (a un CPU puro un 1440p+ le revienta).
    if (m_noHw && m_player.maxHeight() > 1080)
        m_player.setMaxHeight(1080);
    m_player.setMaxHeight(m_player.maxHeight());


    // Volumen/silencio persistidos en la sesión anterior.
    // "volume_max" es el ajuste "Volumen máximo" de Ajustes (100/150/175/200);
    // "volume" solo se persiste si ≤ 100, así que con boost activo el slider
    // SIEMPRE arranca al 100% (independiente del valor que dejó el usuario).
    const int volumeMax = m_settings.getInt(QStringLiteral("volume_max"), 100);
    m_player.setVolumeMax(volumeMax);
    const double savedVol = m_settings.getInt(QStringLiteral("volume"), 100);
    m_player.setVolume(savedVol);
    if (m_settings.getInt(QStringLiteral("volume_muted"), 0) == 1)
        m_player.setMuted(true);
    // Preferencia de idioma de audio
    const QString savedAL = m_settings.getString(
        QStringLiteral("audio_language"), QStringLiteral("original"));
    if (m_yt)
        m_yt->setAudioLanguage(savedAL);

    // Color de acento persistido. Se aplica ANTES de generar el stylesheet
    // global (setStyleSheet(nt::styleSheet()) al final del constructor), así
    // que el QSS ya nace con el acento elegido.
    {
        const QString savedAccent = m_settings.getString(
            QStringLiteral("ui_accent"), QStringLiteral("#FF0033"));
        const QColor accent(savedAccent);
        if (accent.isValid())
            nt::setAccent(accent);
    }

    // Modo de generación de cola
    const QString savedNext = m_settings.getString(
        QStringLiteral("next_mode"), QStringLiteral("radio"));
    if (savedNext == QStringLiteral("none"))
        setNextMode(NextMode::None);
    else if (savedNext == QStringLiteral("suggestions"))
        setNextMode(NextMode::Suggestions);
    else
        setNextMode(NextMode::Radio);

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
    // Alta/baja de suscripción: refresca el feed "Nuevos de tus suscripciones"
    // al momento, sin esperar a re-entrar a Home.
    connect(&m_subs, &SubscriptionsRepo::changed,
            m_home, &HomePage::refreshDynamic);
    m_search = new SearchPage(m_yt, &m_settings, m_globalThumbs, this);
    m_channel = new ChannelPage(m_yt, m_globalThumbs, &m_subs, this);
    m_watch = new WatchPage(m_yt, &m_player, m_playerWidget, &m_videoCache,
                            &m_fav, &m_starred, &m_watchLater, &m_subs,
                            &m_settings, m_globalThumbs, &m_playlists, this);
    m_library = new LibraryPage(m_yt, &m_fav, &m_starred, &m_watchLater,
                                &m_history, &m_subs, &m_videoCache, m_libraryThumbs,
                                &m_playlists, this);
    m_playlist = new PlaylistPage(&m_playlists, &m_videoCache, m_libraryThumbs, this);
    m_historyPage = new HistoryPage(&m_history, &m_videoCache, m_libraryThumbs,
                                   &m_watchLater, &m_playlists, this);
    m_settingsPage = new SettingsPage(&m_settings, this);
    connect(m_settingsPage, &SettingsPage::qualityChanged,
            this, [this](const QString &val) {
                if (val == QStringLiteral("auto")) {
                    int def = m_settings.getInt(
                        QStringLiteral("quality_auto"), 360);
                    m_player.setMaxHeight(def);
                } else {
                    int h = val.toInt();
                    if (h > 0) {
                        if (m_noHw && h > 1080)
                            h = 1080;
                        m_player.setMaxHeight(h);
                    } else
                        m_player.setMaxHeight(360);
                }
                // El cambio de calidad aplicada al reproductor se refleja en
                // el siguiente vídeo; el botón de calidad del overlay se
                // actualizará al cargar el siguiente vídeo (WatchPage lee
                // settings al abrir).
            });
    connect(m_settingsPage, &SettingsPage::nextModeChanged,
            this, [this](const QString &val) {
                if (m_yt) {
                    if (val == QStringLiteral("none"))
                        setNextMode(NextMode::None);
                    else if (val == QStringLiteral("suggestions"))
                        setNextMode(NextMode::Suggestions);
                    else
                        setNextMode(NextMode::Radio);
                }
            });
    connect(m_settingsPage, &SettingsPage::audioLanguageChanged,
            this, [this](const QString &val) {
                if (m_yt)
                    m_yt->setAudioLanguage(val);
            });
    // Volumen máximo: aplica el tope a mpv en caliente. Como el volumen actual
    // se cape a 100 en persistVolume, al activar >100 el volumen visible
    // SE QUEDA en su valor (≤100) y el slider del overlay se reajusta solo.
    connect(m_settingsPage, &SettingsPage::volumeMaxChanged,
            this, [this](const QString &val) {
                m_player.setVolumeMax(val.toInt());
                // Refresca el slider del overlay para reflejar el nuevo rango.
                m_watch->refreshVolumeSlider();
                persistVolume();
            });
    connect(m_settingsPage, &SettingsPage::miniVideoModeChanged,
            this, [this]() {
                // Si se cambia estando ya en el mini (Ajustes es visible con el
                // mini bar), re-aplica el modo en caliente: vídeo vivo o
                // miniatura encima.
                applyMiniVideoMode();
            });
    connect(m_settingsPage, &SettingsPage::gridDensityChanged,
            this, [this](const QString &val) {
                const qreal density =
                    (val == QStringLiteral("compact")) ? 0.88 : 1.0;
                const auto lists = m_pages->findChildren<VideoListView *>();
                for (VideoListView *l : lists)
                    if (l)
                        l->setDensity(density);
            });
    // Color de acento: se aplica en caliente. Se re-genera el stylesheet
    // global (sliders, botón primario, selección, nav activa, scrollbar…), se
    // re-tiñen los iconos de la nav y se repintan las vistas que dibujan con
    // kAccent (barra de progreso de miniaturas, borde "Reproduciendo"). Las
    // páginas re-aplican además sus estilos inline que dependen del acento
    // (botón Suscribirse, acciones activas, enlaces de descripción, empty
    // state y botón reset de Ajustes).
    connect(m_settingsPage, &SettingsPage::accentChanged,
            this, [this](const QString &hex) {
                const QColor accent(hex);
                if (!accent.isValid())
                    return;
                nt::setAccent(accent);
                setStyleSheet(nt::styleSheet());
                const auto navButtons = m_nav->findChildren<NavButton *>();
                for (NavButton *b : navButtons)
                    if (b)
                        b->updateIcon();
                const auto lists = m_pages->findChildren<VideoListView *>();
                for (VideoListView *l : lists)
                    if (l)
                        l->update();
                m_watch->applyAccent();
                m_channel->applyAccent();
                m_home->applyAccent();
                m_settingsPage->applyAccent();
            });
    connect(m_settingsPage, &SettingsPage::clearThumbsRequested,
            this, [this]() {
                for (ThumbnailCache *t : {m_homeThumbs, m_globalThumbs,
                                            m_libraryThumbs})
                    if (t)
                        t->clear();
            });
    // El popup custom de Ajustes ya pide confirmación; sin QMessageBox
    // nativo (secuestraba el puntero con un segundo diálogo).
    connect(m_settingsPage, &SettingsPage::resetRequested,
            this, [this]() { resetAppData(); });
    // "Volver a detectar hardware": la página ya borró codec_profile; relanzar
    // HwProbe. Si hay reproducción en curso no se interrumpe el vídeo actual,
    // pero la app queda en modo "sondeando" (no admite otra reproducción) hasta
    // que termine.
    connect(m_settingsPage, &SettingsPage::redetectHwRequested,
            this, [this]() { startHwProbe(); });

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
    connect(m_home, &HomePage::navigateToSearchRequested, this,
            &MainWindow::goSearch);
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
    connect(m_historyPage, &HistoryPage::channelRequested, this,
            [this](const QString &channelId) {
                m_lastTab = m_currentTab;
                m_currentTab = 2;
                m_channel->openChannel(channelId);
                m_pages->setCurrentWidget(m_channel);
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

    // Densidad de cuadrícula guardada (Cómoda/Compacta)
    {
        const QString savedD = m_settings.getString(
            QStringLiteral("grid_density"), QStringLiteral("cozy"));
        const qreal density =
            (savedD == QStringLiteral("compact")) ? 0.88 : 1.0;
        const auto lists = m_pages->findChildren<VideoListView *>();
        for (VideoListView *l : lists)
            if (l)
                l->setDensity(density);
    }

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
    case Qt::Key_J:
        // -10 s (estilo YouTube).
        if (!m_player.isActive())
            return false;
        m_player.seek(m_player.position() - 10.0);
        return true;
    case Qt::Key_L:
        // +10 s (estilo YouTube).
        if (!m_player.isActive())
            return false;
        m_player.seek(m_player.position() + 10.0);
        return true;
    case Qt::Key_N:
        // Siguiente vídeo de la cola.
        if (!m_player.isActive())
            return false;
        if (m_player.hasNext())
            m_player.next();
        return true;
    case Qt::Key_P:
        // Vídeo anterior de la cola.
        if (!m_player.isActive())
            return false;
        if (m_player.hasPrevious())
            m_player.previous();
        return true;
    case Qt::Key_Up:
        if (!m_player.isActive())
            return false;
        // Respetar volume_max (150/175/200 con boost): capar a 100 haría BAJAR
        // el volumen si ya estamos por encima de 100 mientas se sube.
        m_player.setVolume(qMin(m_player.volume() + 5.0,
                                double(m_player.volumeMax())));
        persistVolume();
        updateMiniIcons();
        return true;
    case Qt::Key_Down:
        if (!m_player.isActive())
            return false;
        m_player.setVolume(qMax(m_player.volume() - 5.0, 0.0));
        persistVolume();
        updateMiniIcons();
        return true;
    case Qt::Key_M:
        if (!m_player.isActive())
            return false;
        m_player.toggleMute();
        persistVolume();
        updateMiniIcons();
        return true;
    case Qt::Key_F:
        // Pantalla completa solo desde la página del reproductor: el atajo no
        // debe escalar la ventana cuando el usuario está en Home, Ajustes, etc.
        if (m_pages->currentWidget() != m_watch)
            return false;
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

    // Hueco fijo donde se incrusta el PlayerWidget en vivo cuando el mini está
    // activo; si no, muestra la miniatura estática.
    m_miniVideoHost = new QWidget(m_miniBar);
    m_miniVideoHost->setObjectName(QStringLiteral("miniVideoHost"));
    m_miniVideoHost->setFixedSize(112, 63);
    m_miniVideoHost->setCursor(Qt::PointingHandCursor);
    auto *hostLay = new QVBoxLayout(m_miniVideoHost);
    hostLay->setContentsMargins(0, 0, 0, 0);
    hostLay->setSpacing(0);
    m_miniThumb = new QLabel(m_miniVideoHost);
    m_miniThumb->setAlignment(Qt::AlignCenter);
    m_miniThumb->setStyleSheet(
        QStringLiteral("background:#262626; border-radius:6px; color:#8F8F8F; "
                       "font-size:10px;"));
    m_miniThumb->setTextInteractionFlags(Qt::NoTextInteraction);
    hostLay->addWidget(m_miniThumb);
    row->addWidget(m_miniVideoHost);

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
        persistVolume();
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
    // Vídeo en vivo en el mini: al mostrarlo, el PlayerWidget se reparenta al
    // hueco del mini bar y sigue renderizando (sin reconf del decodificador ni
    // frames acumulados). Al volver a la página Player se reintegra al área de
    // vídeo. Solo en los cambios de estado, no en cada llamada.
    if (show && !m_miniDetached && m_watch && m_miniVideoHost) {
        m_watch->detachPlayerTo(m_miniVideoHost);
        // El modo de la miniatura: "video" muestra el vídeo en vivo; "thumb"
        // pone la miniatura estática POR ENCIMA del PlayerWidget (el vídeo
        // sigue renderizando detrás: pipeline caliente, sin reconf ni backlog).
        m_miniDetached = true;
        applyMiniVideoMode();
    } else if (!show && m_miniDetached && m_watch) {
        m_watch->attachPlayer();
        m_miniDetached = false;
    }
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

// Aplica el modo del mini player mientras hay vídeo incrustado (detached):
// "video" → el PlayerWidget queda a la vista (miniatura oculta);
// "thumb"  → la miniatura estática va por encima y el vídeo renderiza detrás.
// En ningún caso mpv deja de dibujar, por eso no hay salto al volver.
void MainWindow::applyMiniVideoMode()
{
    if (!m_miniDetached || !m_miniThumb)
        return;
    const bool live = m_settings.getString(QStringLiteral("mini_video"),
                                           QStringLiteral("thumb")) ==
                      QLatin1String("video");
    if (live) {
        m_miniThumb->hide();
    } else {
        m_miniThumb->show();
        m_miniThumb->raise();
    }
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
    if (thumbUrl.isEmpty())
        thumbUrl = fallbackThumbnailUrl(v.id);
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

// Persiste volumen y silencio actuales del controlador (src/db/settings:
// claves "volume" / "volume_muted"); MainWindow::persistVolume() al arranque
// los vuelve a aplicar. Se llama tras cada cambio hecho por el usuario.
// NOTA: si el ajuste "Volumen máximo" está activo (>100), el volumen persistido
// se CAPA a 100 para que al reiniciar la app el slider arranque al 100% y NO
// quede en el valor alto de la sesión anterior (protección auditiva).
void MainWindow::persistVolume()
{
    const double v = qMin(m_player.volume(), 100.0);
    m_settings.set(QStringLiteral("volume"), QString::number(v));
    m_settings.set(QStringLiteral("volume_muted"),
                   m_player.muted() ? QStringLiteral("1")
                                    : QStringLiteral("0"));
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

void MainWindow::resetAppData()
{
    // 1. Quiescer reproductor (evita escritura de posición al cerrar)
    m_player.stop();
    // 2. Cerrar SQLite y permitir borrado en Windows
    m_db.close();
    // 3. Borrar datos propios de la aplicación (no archivos personales)
    const QString dbPath = m_db.path();
    if (!dbPath.isEmpty()) {
        QFile::remove(dbPath);
        QFile::remove(dbPath + QStringLiteral("-wal"));
        QFile::remove(dbPath + QStringLiteral("-shm"));
    }
    // 3b. Borrar el caché de resoluciones HLS en disco (evita reusar URLs
    // caducadas/estranguladas; la BD ya no guarda nada de esto).
    QFile::remove(Innertube::hlsCacheFilePath());
    // 4. Limpiar cachés de miniaturas (solo directorios propios)
    for (ThumbnailCache *t : {m_homeThumbs, m_globalThumbs,
                                m_libraryThumbs}) {
        if (t)
            t->clear();
    }
    // 5. Reiniciar la aplicación con defaults; la BD se recreará al abrir
    QProcess::startDetached(
        QCoreApplication::applicationFilePath(),
        QCoreApplication::arguments(),
        QDir::currentPath());
    // El proceso actual sale; la nueva instancia arranca limpia.
    QTimer::singleShot(200, this, [this]() {
        qApp->quit();
    });
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

void MainWindow::applyCodecProfile(const QString &profile, bool persist)
{
    // Perfil activo en memoria (la escalera compara contra este; ver m_activeProfile).
    m_activeProfile = profile;
    if (persist)
        m_settings.set(QStringLiteral("codec_profile"), profile);
    m_noHw = profile == QLatin1String("soft-all");
    if (profile == QLatin1String("av1-hw")) {
        m_yt->setCodecProfile(Yt::CodecProfile::Modern);
        m_playerWidget->setInitialBufferSecs(1.0);
    } else if (profile == QLatin1String("vp9-hw")) {
        m_yt->setCodecProfile(Yt::CodecProfile::Vp9);
        m_playerWidget->setInitialBufferSecs(1.0);
    } else if (profile == QLatin1String("soft-all")) {
        // Sin HW: h264 (universal) -> perfil Legacy, espera agresiva y tope
        // 1080p (a un CPU puro un 1440p+ le revienta).
        m_yt->setCodecProfile(Yt::CodecProfile::Legacy);
        m_playerWidget->setInitialBufferSecs(6.0);
        if (m_player.maxHeight() > 1080)
            m_player.setMaxHeight(1080);
    } else {
        // h264-hw (y perfiles desconocidos): h264 por hardware, margen bajo el
        // throttle -> espera corta.
        m_yt->setCodecProfile(Yt::CodecProfile::Legacy);
        m_playerWidget->setInitialBufferSecs(1.5);
    }
    NT_DEBUG("E2E codec_profile apply=%s persist=%d noHw=%d\n",
             profile.toUtf8().constData(), persist ? 1 : 0, m_noHw ? 1 : 0);
    if (m_settingsPage)
        m_settingsPage->updateCodecProfile(profile);
}

void MainWindow::showHwPopup()
{
    if (m_hwPopup || !m_hwProbing)
        return;
    m_hwPopup = new QFrame(this);
    m_hwPopup->setObjectName(QStringLiteral("hwProbePopup"));
    m_hwPopup->setAttribute(Qt::WA_StyledBackground, true);
    m_hwPopup->setStyleSheet(QStringLiteral(
        "#hwProbePopup { background: rgba(18,18,18,0.96);"
        " border: 1px solid #3a3a3a; border-radius: 10px; }"));
    auto *lay = new QVBoxLayout(m_hwPopup);
    lay->setContentsMargins(28, 22, 28, 22);
    lay->setSpacing(8);
    auto *title = new QLabel(QStringLiteral("Detectando hardware de vídeo"),
                             m_hwPopup);
    title->setStyleSheet(QStringLiteral(
        "color: #fff; font-size: 16px; font-weight: 600;"));
    title->setAlignment(Qt::AlignCenter);
    auto *text = new QLabel(
        QStringLiteral("Comprobando la aceleración de vídeo de tu equipo.\n"
                       "La reproducción comenzará al terminar."),
        m_hwPopup);
    text->setStyleSheet(QStringLiteral("color: #c8c8c8; font-size: 12px;"));
    text->setAlignment(Qt::AlignCenter);
    lay->addWidget(title);
    lay->addWidget(text);
    m_hwPopup->adjustSize();
    centerHwPopup();
    m_hwPopup->show();
    m_hwPopup->raise();
}

void MainWindow::centerHwPopup()
{
    if (!m_hwPopup || !centralWidget())
        return;
    const QRect g = centralWidget()->geometry();
    const QSize s = m_hwPopup->size();
    m_hwPopup->move(g.center().x() - s.width() / 2,
                    g.center().y() - s.height() / 2);
    m_hwPopup->raise();
}

void MainWindow::startHwProbe()
{
#ifndef NEVERTUBE_NO_MPV
    if (m_hwProbing)
        return;
    // HwProbe es de un solo uso (m_done): re-crearlo en cada re-detección, si no
    // start() sería un no-op y m_hwProbing quedaría colgado para siempre.
    if (m_hwProbe) {
        m_hwProbe->deleteLater();
        m_hwProbe = nullptr;
    }
    m_hwProbing = true;
    m_hwProbe = new HwProbe(this);
    connect(m_hwProbe, &HwProbe::finished, this,
            [this](const QString &profile) {
                m_hwProbing = false;
                if (m_hwPopupTimer)
                    m_hwPopupTimer->stop();
                if (m_hwPopup) {
                    m_hwPopup->hide();
                    m_hwPopup->deleteLater();
                    m_hwPopup = nullptr;
                }
                if (profile.isEmpty()) {
                    // Indeterminado (los clips no decodifican): perfil
                    // conservador sin persistir. NO asumir HW (era el falso
                    // positivo); se reintenta el sondeo en el próximo arranque
                    // (no se marca la versión del sondeo).
                    applyCodecProfile(QStringLiteral("soft-all"), false);
                } else {
                    applyCodecProfile(profile, true);
                    // Sondeo de esta versión completado: no se repite en cada
                    // arranque (crítico en GPUs lentas).
                    m_settings.set(QStringLiteral("codec_probe_v"),
                                   QString::number(kCodecProbeVersion));
                }
                if (m_hasPendingWatch) {
                    const QVector<VideoInfo> v = m_pendingWatch;
                    const int idx = m_pendingIndex;
                    const qlonglong pos = m_pendingPos;
                    m_hasPendingWatch = false;
                    openWatchAt(v, idx, pos);
                }
            });
    // El cartel solo aparece si el sondeo tarda (PC básico); en un PC rápido el
    // resultado llega antes de 300 ms y el usuario no ve nada.
    if (!m_hwPopupTimer) {
        m_hwPopupTimer = new QTimer(this);
        m_hwPopupTimer->setSingleShot(true);
        connect(m_hwPopupTimer, &QTimer::timeout, this, [this]() {
            if (m_hwProbing)
                showHwPopup();
        });
    }
    m_hwPopupTimer->start(300);
    m_hwProbe->start();
#endif // NEVERTUBE_NO_MPV
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
    // Guarda defensiva: datos procedentes de red/BD no deben poder crashear
    // por un índice inválido (vector vacío o startIndex fuera de rango).
    if (startIndex < 0 || startIndex >= videos.size()) {
        NT_DEBUG("E2E openWatchAt rechazado startIndex=%d size=%d\n",
                 startIndex, (int)videos.size());
        return;
    }
    // Sondeo de hardware en curso: no se puede reproducir todavía. Se encola la
    // petición (con su posición de reanudación) y se muestra el cartel, que se
    // cierra al terminar el sondeo, momento en que se abre el vídeo.
    if (m_hwProbing) {
        m_pendingWatch = videos;
        m_pendingIndex = startIndex;
        m_pendingPos = startPositionMs;
        m_hasPendingWatch = true;
        showHwPopup();
        return;
    }
    // Deja de sembrar radios de Descubre: al abrir un vídeo la radio de
    // "A continuación" pasa a ser el único consumidor de fetchRadio()
    // (single-flight en Innertube).
    if (m_home)
        m_home->interruptDiscover();
    // Ir a la página Player ANTES de activar la cola: si se activa primero,
    // updateMiniPlayer ve "reproduciendo fuera de Player" y engancha el mini
    // un frame, para desengancharlo al cambiar de página (parpadeo al abrir).
    m_lastTab = m_currentTab;
    m_currentTab = 3;
    m_pages->setCurrentIndex(3);
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
    // Instancia nueva tras un reinicio por sesión gateada: reanuda donde se
    // cortó (un solo vídeo: la cola no se restaura).
    if (m_gateResumeMs > 0) {
        openWatchResume(QVector<VideoInfo>{v}, 0, m_gateResumeMs);
        m_gateResumeMs = 0;
        return;
    }
    openWatch(QVector<VideoInfo>{v}, 0);
}

void MainWindow::onRecoveryExhausted()
{
    // Único auto-reinicio de la app y SOLO en este error (recuperación de
    // sesión gateada agotada): relanza con el último vídeo, porque reiniciar
    // sale de la sesión gateada (verificación del usuario: al reiniciar se
    // arregla el audio). Guard en memoria: el mismo episodio no reinicia dos
    // veces (PlayerController puede reemitir la señal en una cascada de EOF).
    if (m_gateRestarting)
        return;
    m_gateRestarting = true;
    // Guard en settings: si esta instancia YA venía de un reinicio (armed=1),
    // la sesión nueva también está gateada -> se muestra el error y se desarma
    // el guard para no encadenar reinicios en bucle.
    if (m_settings.getInt(QStringLiteral("gate_restart_armed"), 0) == 1) {
        NT_DEBUG("E2E gate reinicio ya intentado -> error (sin 2º reinicio)\n");
        m_settings.set(QStringLiteral("gate_restart_armed"),
                       QStringLiteral("0"));
        m_settings.set(QStringLiteral("gate_restart_vid"), QString());
        m_settings.set(QStringLiteral("gate_restart_pos"), QStringLiteral("0"));
        return;
    }
    const VideoInfo v = m_player.current();
    if (v.id.isEmpty() || v.id.size() != 11) {
        NT_DEBUG("E2E gate reinicio sin vídeo válido -> error\n");
        return;
    }
    const QString url = QStringLiteral("https://www.youtube.com/watch?v=") + v.id;
    m_settings.set(QStringLiteral("gate_restart_vid"), v.id);
    m_settings.set(QStringLiteral("gate_restart_pos"),
                   QString::number(qRound64(m_player.position() * 1000.0)));
    m_settings.set(QStringLiteral("gate_restart_armed"), QStringLiteral("1"));
    NT_DEBUG("E2E gate reinicio -> relanzar %s pos=%.1fs\n",
             url.toUtf8().constData(), m_player.position());
    QProcess::startDetached(QCoreApplication::applicationFilePath(),
                            QStringList() << url, QDir::currentPath());
    // La instancia nueva arranca "http://..." -> playVideo reanuda el último
    // vídeo (ver gate_restart_armed en el constructor).
    QTimer::singleShot(200, this, [this]() { qApp->quit(); });
}

void MainWindow::onRecoveryHealthy()
{
    // Reproducción estable ≥5 min tras el reinicio: el episodio terminó y la
    // próxima sesión gateada podrá volver a reiniciar.
    if (m_settings.getInt(QStringLiteral("gate_restart_armed"), 0) != 1)
        return;
    NT_DEBUG("E2E gate guard desarmado (reproducción estable)\n");
    m_settings.set(QStringLiteral("gate_restart_armed"), QStringLiteral("0"));
    m_settings.set(QStringLiteral("gate_restart_vid"), QString());
    m_settings.set(QStringLiteral("gate_restart_pos"), QStringLiteral("0"));
}

void MainWindow::openChannel(const QString &channelId)
{
    m_lastTab = m_currentTab;
    m_currentTab = 2;
    m_channel->openChannel(channelId);
    m_pages->setCurrentWidget(m_channel);
}
