#include "mainwindow.h"

#include "downloader/download_folder.h"
#include "downloader/download_settings.h"
#include "reels_filter.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCursor>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include <QMouseEvent>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QFrame>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QToolTip>
#include <functional>

#include "models.h"
#include "hwprobe.h"
#include "i18n.h"
#include "playerwidget.h"
#include "../src/debug.h"
#include "ui/format.h"
#include "ui/icons.h"
#include "ui/player_glyphs.h"
#include "ui/pages/home_page.h"
#include "ui/pages/channel_page.h"
#include "ui/pages/library_page.h"
#include "ui/pages/playlist_page.h"
#include "ui/confirm_dialog.h"
#include "ui/pages/settings_page.h"
#include "ui/pages/downloads_page.h"
#include "downloader/download_manager.h"
#include "updater/update_dialog.h"
#include "ui/pages/history_page.h"
#include "ui/pages/search_page.h"
#include "ui/pages/watch_page.h"
#include "ui/row_menu.h"
#include "ui/theme.h"
#include "ui/thumbnail_cache.h"
#include "ui/video_list_view.h"
#include "yt.h"
#include "yt_innertube.h"

namespace {

// Version of the HW probing logic (HwProbe). Bumping this version makes the
// app re-probe the GPU ONCE at startup, even with a persisted profile: that way
// a false positive written by an old version (e.g. av1-hw from the i5 with
// qsv-copy) self-corrects without having to re-probe on every launch (which
// would penalize the test machine, where probing is slow).
constexpr int kCodecProbeVersion = 2;

// Anti-bounce and budget of the profile descent ladder (codec/HW evidence).
// The same episode must not degrade several times in a row (the decoder can
// emit repeated "software"); and at most 3 steps are descended per session
// (av1-hw -> vp9-hw -> h264-hw -> soft-all).
constexpr qint64 kDescendCooldownMs = 15000;
constexpr int kDescendMaxPerSession = 3;

// Button of the bottom navigation bar. Uses the PNGs attached per state
// (home/search/library/settings × inactive/active/hover/pressed/disabled).
// The QSS cannot change qproperty-icon dynamically, so the icon is chosen here
// based on the button's real state. The accent states (active/hover/pressed)
// use PNGs pre-painted per accent color (nav/<key>_<state>_<suffix>.png,
// generated in tools/gen_nav_accent.cpp): no runtime retint — a hot accent
// change only re-evaluates the suffix, and the retint bug class ("black
// square" alpha, accent frozen in a static) cannot exist. Inactive and
// disabled use the PNG's native grays.
class NavButton : public QToolButton
{
public:
    using QToolButton::QToolButton;

    // Visible state (high→low priority): disabled > pressed > hover >
    // checked > normal. The accent color is applied on the active states.
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
        // Accent states: PNG pre-painted with the suffix of the current accent;
        // inactive/disabled use their native grays without suffix.
        const bool accentState = state == QLatin1String("active")
                                 || state == QLatin1String("hover")
                                 || state == QLatin1String("pressed");
        const QString path = accentState
            ? QStringLiteral(":/nav/%1_%2_%3.png")
                  .arg(key, state, nt::accentSuffix())
            : QStringLiteral(":/nav/%1_%2.png").arg(key, state);
        setIcon(QIcon(path));
        setIconSize(QSize(26, 26));
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

// Mini player bar: any click outside a button emits the go-back to the Player
// page. The internal buttons consume their own click and it never reaches here
// (unaccepted mouse events propagate up to the parent widget).
class MiniPlayerBar : public QWidget
{
public:
    explicit MiniPlayerBar(QWidget *parent = nullptr) : QWidget(parent)
    {
        // On switching window or workspace the app deactivates and no Leave
        // arrives: turn the hover off on the spot. On reactivating it is NOT
        // re-evaluated with QCursor::pos(): with the app inactive that position
        // stays FROZEN at the last known point (Wayland delivers no pointer
        // events on workspace switch) and, if the pointer was over the bar when
        // deactivated, it would falsely re-light on return even if the real
        // pointer is elsewhere. The hover only re-lights with a real
        // enterEvent (or on moving the pointer over the bar).
        connect(qApp, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
                    if (state == Qt::ApplicationInactive)
                        setHover(false);
                });
    }
    std::function<void()> onActivate;
    // Progress bar stuck to the bottom edge and full width. It lives outside
    // the layout (which carries side margins) and is repositioned here: that is
    // what leaves it "flush" like on YouTube.
    QProgressBar *progress = nullptr;

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

    // Hover over the whole bar: lightens the background to signal it is
    // clickable (expands to the Player). The state travels via dynamic property
    // to avoid an inline setStyleSheet; the rule lives in the QSS
    // (QWidget#miniBar[hover]).
    void enterEvent(QEvent *event) override
    {
        setHover(true);
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        // Do not turn the hover off if the cursor is still inside the bar: when
        // moving from the bar to a child button, Qt sends Leave to the parent
        // and the background would flicker. The guard is only evaluated when a
        // Leave arrives (active interaction, where QCursor::pos() is actually
        // fresh).
        if (!rect().contains(mapFromGlobal(QCursor::pos())))
            setHover(false);
        QWidget::leaveEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        if (progress && progress->parentWidget() == this) {
            progress->setGeometry(0, height() - progress->height(), width(),
                                  progress->height());
            progress->raise();
        }
    }

    // On hiding (leaving the Player page) the hover must not survive: the
    // Leave does not arrive if the cursor was still inside the bar at hide
    // time.
    void hideEvent(QHideEvent *event) override
    {
        setHover(false);
        QWidget::hideEvent(event);
    }

    // On re-showing (returning from the Player page) Qt does not always
    // re-send Enter under the cursor: re-evaluate against the real position. If
    // the pointer ended up over the bar it re-lights; otherwise it stays off.
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        setHover(rect().contains(mapFromGlobal(QCursor::pos())));
    }

private:
    void setHover(bool on)
    {
        if (property("hover").toBool() == on)
            return;
        setProperty("hover", on);
        // The dynamic property only repaints after re-polish (mandatory).
        style()->unpolish(this);
        style()->polish(this);
    }
};

// Mini player thumbnail with corners rounded FOR REAL: the QSS border-radius
// only rounds the QLabel BACKGROUND, not the QPixmap painted on top (square
// corners showed, same case as circularAvatar). With a pixmap it draws it
// clipped to a rounded rect; without pixmap it delegates to QLabel ("no
// thumbnail" state with the QSS background and text).
class RoundedThumb : public QLabel
{
public:
    using QLabel::QLabel;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        const QPixmap pm = pixmap(Qt::ReturnByValue);
        if (pm.isNull()) {
            QLabel::paintEvent(event);
            return;
        }
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()), 6.0, 6.0);
        p.setClipPath(clip);
        // Centered and not scaled: the pixmap already comes "cover" (>= label)
        // and the clip cuts the surplus, so it does not deform if the aspect
        // does not match.
        p.drawPixmap((width() - pm.width()) / 2, (height() - pm.height()) / 2,
                     pm);
    }
};

// Migrates old codec profile names (reactive detection) to the ones from
// HwProbe's offline probe. The migration is persisted so it is not repeated.
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
    // Auto-restart on gated session: if this instance came from a recovery
    // restart (guard armed in settings), the last video must resume where it
    // was cut off. The guard is NOT disarmed on open: only when playback
    // stabilizes for 5 min (recoveryHealthy) or when exhausted without success
    // (onRecoveryExhausted), so a restart never chains another one.
    if (m_settings.getInt(QStringLiteral("gate_restart_armed"), 0) == 1)
        m_gateResumeMs = m_settings.getInt(QStringLiteral("gate_restart_pos"), 0);
    connect(&m_player, &PlayerController::recoveryExhausted, this,
            &MainWindow::onRecoveryExhausted);
    connect(&m_player, &PlayerController::recoveryHealthy, this,
            &MainWindow::onRecoveryHealthy);
    // Video codec profile. HwProbe probes the GPU with offline micro-clips and
    // leaves a persisted profile (av1-hw|vp9-hw|h264-hw|soft-all). If there is
    // no result, soft-all (conservative) is applied without persisting, and
    // while probing runs nothing plays (the request is queued). The profile is
    // decided and persisted ONLY at startup: the reactive network path
    // (streamDecodeInfoReady) remains as a diagnostic trace and no longer
    // degrades the profile hot (correct detection at startup is HwProbe's
    // responsibility).
    const QString rawProfile = m_settings.getString(
        QStringLiteral("codec_profile"), QString());
    const QString savedProfile = normalizeCodecProfile(rawProfile);
    if (!rawProfile.isEmpty() && savedProfile != rawProfile)
        m_settings.set(QStringLiteral("codec_profile"), savedProfile);
#ifdef NEVERTUBE_NO_MPV
    // x86 build without mpv (Phase 5): no HwProbe (it probes with mpv). The
    // profile is decided per platform — the x86 target goes through DXVA2,
    // H.264 is the ceiling; av1/vp9 will go to SW and the reactive ladder
    // (streamDecodeInfoReady -> hwdecCurrent()=="no") would already descend
    // them hot. No saved value = h264-hw, persisted so Settings shows it.
#if defined(Q_OS_WIN)
    // KEY x86: an av1-hw/vp9-hw profile saved by the mpv era (HwProbe ran
    // there) would request codecs the i686 package cannot play — AV1 decodes
    // neither by HW nor by SW (the FFmpeg 9 native decoder does not support
    // native decode) and the reactive ladder cannot lower it because AV1 never
    // produces a frame (without streamDecodeInfoReady there is no descent: it
    // would keep the decoder error forever). The only valid x86 profiles are
    // h264-hw (DXVA2 ceiling) or soft-all (if DXVA2 does not start, the hot
    // ladder decides); anything else is CLAMPED to h264-hw and persisted.
    // Also, picking AV1 in the format selection is forbidden
    // (Yt::codecRankedScore): the Legacy profile can hit YouTube's av01 when
    // the video offers no h264, and that must also fall back to vp9.
    m_yt->setNoAv1(true);
    if (savedProfile != QLatin1String("h264-hw")
        && savedProfile != QLatin1String("soft-all")) {
        m_settings.set(QStringLiteral("codec_profile"),
                       QStringLiteral("h264-hw"));
        applyCodecProfile(QStringLiteral("h264-hw"), false);
    } else {
        applyCodecProfile(savedProfile, false);
    }
#else
    if (savedProfile.isEmpty()) {
        m_settings.set(QStringLiteral("codec_profile"),
                       QStringLiteral("h264-hw"));
        applyCodecProfile(QStringLiteral("h264-hw"), false);
    } else {
        applyCodecProfile(savedProfile, false);
    }
#endif
#else
    // The profile is re-probed at startup if there is no persisted value or if
    // it was written by another probe version (codec_probe_v). The saved value
    // is used as a fallback while probing; the fresh verdict replaces it.
    // Nothing plays until the verdict arrives (pending queue).
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
                // hwdec-current arrives "empty" on the first video-params (mpv
                // has not activated the decoder yet) and "no" when the decoder
                // knows it is in software; the widget retries until resolved.
                //
                // Descent ladder: if the ACTIVE profile promises hardware and
                // mpv decodes in SOFTWARE, the startup verdict does not hold
                // during playback -> drop one step and RE-RESOLVE with the new
                // profile (reloadCurrent without rotating the session: a codec
                // problem is not a session problem).
                //
                // Hard rule: NEVER on network. A 403, an expired visitorData or
                // a cache stall do not change hwdec-current; this signal only
                // exists with frames decoded in software. With no codec/HW
                // evidence (above: hwdecBackend != "no") nothing is touched.
                if (hwdecBackend != QLatin1String("no")) {
                    // Decoding in hardware: the profile hit -> a healthy streak
                    // resets the descent budget.
                    m_descendAttempts = 0;
                    return;
                }
                const QString lower = hwprobe::lowerProfile(m_activeProfile);
                NT_DEBUG("E2E codec_profile reactivo: perfil=%s cod=%s hw=no "
                         "-> bajar=%s\n",
                         m_activeProfile.toUtf8().constData(),
                         codec.toUtf8().constData(),
                         lower.isEmpty() ? "(nada)" : lower.toUtf8().constData());
                // soft-all is the floor; an unknown profile is not touched either.
                if (lower.isEmpty())
                    return;
                if (m_descendAttempts >= kDescendMaxPerSession)
                    return;
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (m_lastDescendMs > 0
                    && now - m_lastDescendMs < kDescendCooldownMs)
                    return; // anti-bounce: the same episode does not drop several times
                ++m_descendAttempts;
                m_lastDescendMs = now;
                // Ephemeral: applied IN MEMORY (persist=false). A one-off
                // failure must not "condemn" the machine to a lower profile
                // (Settings -> "Volver a detectar" restores it).
                applyCodecProfile(lower, /*persist=*/false);
                NT_DEBUG("E2E codec_profile descenso=%s intento=%d -> reload\n",
                         lower.toUtf8().constData(), m_descendAttempts);
                m_player.reloadCurrent();
            });
    m_elapsed.start();
    m_player.setPlayer(m_playerWidget);
    // Application-level filter: receives every KeyPress even when focus is on
    // a deep widget (QScrollArea, lists…), not just MainWindow.
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
    // No HW: never ask for more than 1080p (1440p+ kills a pure CPU).
    if (m_noHw && m_player.maxHeight() > 1080)
        m_player.setMaxHeight(1080);
    m_player.setMaxHeight(m_player.maxHeight());


    // Volume/mute persisted from the previous session.
    // "volume_max" is the Settings "Volumen máximo" option (100/150/175/200);
    // "volume" is only persisted if ≤ 100, so with boost active the slider
    // ALWAYS starts at 100% (regardless of the value the user left).
    const int volumeMax = m_settings.getInt(QStringLiteral("volume_max"), 100);
    m_player.setVolumeMax(volumeMax);
    const double savedVol = m_settings.getInt(QStringLiteral("volume"), 100);
    m_player.setVolume(savedVol);
    if (m_settings.getInt(QStringLiteral("volume_muted"), 0) == 1)
        m_player.setMuted(true);
    // Audio language preference
    const QString savedAL = m_settings.getString(
        QStringLiteral("audio_language"), QStringLiteral("original"));
    if (m_yt)
        m_yt->setAudioLanguage(savedAL);

    // Persisted accent color. Applied BEFORE generating the global stylesheet
    // (setStyleSheet(nt::styleSheet()) at the end of the constructor), so the
    // QSS is born with the chosen accent.
    {
        // Migration: the default red was #FF0033 and is now the icon red
        // #FA011B. The old default is no longer in the palette (nobody picked
        // it by hand: it was the factory one), so it is treated as absent.
        const QString savedAccent = m_settings.getString(
            QStringLiteral("ui_accent"), nt::accentHex());
        const QColor accent(savedAccent.toLower() == QStringLiteral("#ff0033")
                                ? QStringLiteral("#fa011b")
                                : savedAccent);
        if (accent.isValid())
            nt::setAccent(accent);
    }

    // Queue generation mode
    const QString savedNext = m_settings.getString(
        QStringLiteral("next_mode"), QStringLiteral("radio"));
    if (savedNext == QStringLiteral("none"))
        setNextMode(NextMode::None);
    else if (savedNext == QStringLiteral("suggestions"))
        setNextMode(NextMode::Suggestions);
    else
        setNextMode(NextMode::Radio);

    // UI language: read BEFORE building the pages so every nt::tr() they call
    // already resolves in the chosen language (there is no second pass that
    // retranslates the whole app). Adding a language is dropping its JSON into
    // localization/ and listing it in the .qrc: the Settings selector picks it
    // up by itself. The default is English; on the very first launch (no saved
    // ui_lang) a Spanish system switches to Spanish, any other language keeps
    // English. An unknown saved code is ignored (setLanguage fails and the
    // current language stays).
    const QString savedLang = m_settings.getString(QStringLiteral("ui_lang"));
    nt::setLanguage(savedLang.isEmpty() ? nt::systemPreferredLanguage()
                                        : savedLang);

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
    // Subscribe/unsubscribe: refreshes the "Nuevos de tus suscripciones" feed
    // right away, without waiting to re-enter Home.
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
    // The downloads page and its manager. The manager is the one that asks
    // for the master, plans the download and keeps the state.
    m_downloadsPage = new DownloadsPage(&m_downloads, this);
    m_downloadManager = new dl::DownloadManager(m_yt, &m_downloads, this, this);
    // The folder of the downloads: the one of Ajustes, or the default one
    // (the folder of downloads of the system plus the subfolder "NeverTube").
    applyDownloadFolder();
    // The page opens the folder where the files are written: the one of the
    // downloader (the default one, or the one set in Ajustes).
    m_downloadsPage->setFolder(m_downloadManager->directory());
    connect(m_downloadManager, &dl::DownloadManager::changed, this, [this]() {
        if (m_downloadsPage)
            m_downloadsPage->reload();
    });
    // The queue of the downloads lives in MEMORY, so a row that was waiting
    // for the other one to end was left "pending" and nothing read it when
    // the app opened again: it stayed "pending" for ever. The same with the
    // one that was running when the app closed.
    //
    // The revive is NOT done here: main.cpp calls resumePendingDownloads()
    // when the app really opens. Doing it in the constructor made every
    // window (the integration tests build one) start the real downloads of
    // the person, with network and files.
    //
    // `paused` and `error` are NOT touched: those wait for the person.
    connect(m_watch, &WatchPage::downloadRequested, this,
            [this](const QString &videoId, const QString &title,
                   const QString &channel, int height, const QString &codec,
                   bool audioOnly) {
        // The duration: with it the size of the download is known before it
        // starts.
        int duration = 0;
        VideoInfo cached;
        if (m_videoCache.get(videoId, cached))
            duration = cached.durationSeconds;
        // The cap of the resolution of the DOWNLOAD is NOT the cap of the
        // player: the download is a file and the menu of the download
        // offers the resolutions the video really has.
        m_downloadManager->start(videoId, title, channel, 0, duration, height,
                                 codec, audioOnly);
        // The tab of the downloads is opened: the progress is there, and the
        // person asked for a download.
        openDownloads();
    });
    connect(m_downloadsPage, &DownloadsPage::playRequested, this,
            [this](const QString &key, const QString &filePath) {
        if (filePath.isEmpty())
            return;
        // The local file: the queue is replaced with it and the history is
        // NOT touched (a downloaded file is not a YouTube video).
        //
        // The id of the download is the KEY of its row (the video or its
        // mp3): the title and the position come from the row, so the player
        // shows the name of the video and not its id.
        DownloadInfo d;
        const bool found = m_downloads.get(key, d);
        VideoInfo info;
        info.id = found ? d.videoId : key;
        info.title = (found && !d.title.isEmpty()) ? d.title : info.id;
        // The position of the local playback lives in the downloads table,
        // NOT in the history (that is about YouTube videos).
        const qlonglong pos = found ? d.positionMs : 0;
        m_player.playLocalFile(filePath, info, pos);
        goToPlayerPage();
    });
    connect(m_downloadsPage, &DownloadsPage::openFolderRequested, this,
            [](const QString &path) {
        if (path.isEmpty())
            return;
        // First the desktop: it is the way that respects the preferences of
        // the session.
        if (QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
            return;
        // But it returns FALSE in the desktops that have no handler for
        // "file://" (measured: it returned false for a folder AND for a
        // file, and the button did nothing at all). Then the file manager
        // of the session is called directly.
        const QStringList cmd = downloads::openCommandFor(path);
        if (cmd.isEmpty()) {
            NT_DEBUG("E2E downloads openFolder path=%s sin comando\n",
                     path.toUtf8().constData());
            return;
        }
        const bool started = QProcess::startDetached(cmd.first(), cmd.mid(1));
        NT_DEBUG("E2E downloads openFolder path=%s cmd=%s ok=%d\n",
                 path.toUtf8().constData(), cmd.join(' ').toUtf8().constData(),
                 started ? 1 : 0);
    });
    connect(m_downloadsPage, &DownloadsPage::pauseRequested, this,
            [this](const QString &key) {
                m_downloadManager->pause(key);
            });
    connect(m_downloadsPage, &DownloadsPage::resumeRequested, this,
            [this](const QString &key) {
                m_downloadManager->resume(key);
            });
    connect(m_downloadsPage, &DownloadsPage::pauseAllRequested, this,
            [this]() {
                m_downloadManager->pauseAll();
                if (m_downloadsPage)
                    m_downloadsPage->reload();
            });
    connect(m_downloadsPage, &DownloadsPage::resumeAllRequested, this,
            [this]() {
                m_downloadManager->resumeAll();
                if (m_downloadsPage)
                    m_downloadsPage->reload();
            });
    connect(m_downloadsPage, &DownloadsPage::removeRequested, this,
            [this](const QString &key) {
        // The delete asks BEFORE: what it deletes depends on the state of
        // the download.
        DownloadInfo info;
        if (!m_downloads.get(key, info))
            return;
        const bool finished = info.status == QLatin1String("done");
        const bool paused = info.status == QLatin1String("paused");
        if (finished) {
            // Finished: it asks with a CHECK BOX whether to delete the file
            // too. The box is UNCHECKED by default, so the action of a single
            // click is the safe one (the file stays). The file is only
            // deleted if it is INSIDE the folder of the downloads (the
            // manager checks it).
            bool alsoFile = false;
            nt::confirm(this, nt::tr("downloads.deleteTitle"),
                        nt::tr("downloads.deleteDone"),
                        nt::tr("downloads.deleteAccept"),
                        [this, key, &alsoFile]() {
                            if (alsoFile)
                                m_downloadManager->remove(key);
                            else
                                m_downloadManager->removeFromList(key);
                        },
                        nt::tr("downloads.deleteAlsoFile"), &alsoFile);
            return;
        }
        // In course or paused: it warns that it cancels the download and
        // deletes what was already downloaded. There is no check box here:
        // the partials always go with it.
        nt::confirm(this, nt::tr("downloads.deleteTitle"),
                    nt::tr(paused ? "downloads.deletePaused"
                                  : "downloads.deleteRunning"),
                    nt::tr("downloads.deleteAccept"),
                    [this, key]() { m_downloadManager->remove(key); });
    });
    connect(m_downloadsPage, &DownloadsPage::retryRequested, this,
            [this](const QString &key) {
        // The retry does NOT delete the row: the index of the fragments that
        // are already on the disk lives in it, and deleting it made the
        // download start from zero (the bytes that were already downloaded
        // were thrown away).
        m_downloadManager->retry(key);
    });
    // The folder of the downloads changed: the downloader and the page of
    // Descargas follow it.
    connect(m_settingsPage, &SettingsPage::downloadFolderChanged, this,
            [this](const QString &) { applyDownloadFolder(); });
    // The default codec of the downloads changed: the menu of the download
    // reads it when it opens, so there is nothing to do here.
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
                // The quality change applied to the player is reflected on the
                // next video; the overlay's quality button will update when the
                // next video loads (WatchPage reads settings on open).
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
    // Maximum volume: applies the cap to mpv hot. Since the current volume is
    // capped to 100 in persistVolume, enabling >100 leaves the visible volume
    // AT its value (≤100) and the overlay slider readjusts itself.
    connect(m_settingsPage, &SettingsPage::volumeMaxChanged,
            this, [this](const QString &val) {
                m_player.setVolumeMax(val.toInt());
                // Refreshes the overlay slider to reflect the new range.
                m_watch->refreshVolumeSlider();
                persistVolume();
            });
    connect(m_settingsPage, &SettingsPage::miniVideoModeChanged,
            this, [this]() {
                // If changed while already in the mini (Settings is visible
                // with the mini bar), re-applies the mode hot: live video or
                // thumbnail on top.
                applyMiniVideoMode();
            });
    connect(m_settingsPage, &SettingsPage::thumbSizeChanged,
            this, [this](const QString &val) { applyThumbSize(val); });
    // Accent color: applied hot. The global stylesheet is re-generated
    // (sliders, primary button, selection, active nav, scrollbar…), the nav
    // icons are re-tinted and the views that draw with kAccent are repainted
    // (thumbnail progress bar, "Reproduciendo" border). The pages additionally
    // re-apply their inline styles that depend on the accent (Subscribe button,
    // active actions, description links, empty state and the Settings reset
    // button).
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
                m_library->applyAccent();
                m_settingsPage->applyAccent();
            });
    connect(m_settingsPage, &SettingsPage::clearThumbsRequested,
            this, [this]() {
                for (ThumbnailCache *t : {m_homeThumbs, m_globalThumbs,
                                            m_libraryThumbs})
                    if (t)
                        t->clear();
            });
    // The custom Settings popup already asks for confirmation; no native
    // QMessageBox (it hijacked the pointer with a second dialog).
    connect(m_settingsPage, &SettingsPage::resetRequested,
            this, [this]() { resetAppData(); });
    // "Volver a detectar hardware": the page already cleared codec_profile;
    // relaunch HwProbe. If playback is in progress the current video is not
    // interrupted, but the app enters "probing" mode (no other playback is
    // accepted) until it finishes.
    connect(m_settingsPage, &SettingsPage::redetectHwRequested,
            this, [this]() { startHwProbe(); });
    // The updater (the manual button of Ajustes and the one automatic check
    // of the session): wired here, where the Settings page already exists and
    // the player is built.
    setupUpdater();
    // Language change from Settings. nt::setLanguage applies the catalog and
    // re-applies every live binding (bottom nav, mini tooltips) on its own;
    // applyLanguage adds what is not a binding.
    connect(m_settingsPage, &SettingsPage::languageChanged,
            this, &MainWindow::applyLanguage);

    m_pages->addWidget(m_home);
    m_pages->addWidget(m_search);
    m_pages->addWidget(m_channel);
    m_pages->addWidget(m_watch);
    m_pages->addWidget(m_library);
    m_pages->addWidget(m_settingsPage);
    m_pages->addWidget(m_playlist);
    m_pages->addWidget(m_historyPage);
    // The downloads page goes LAST (index 8): inserting it in the middle
    // would shift every index the app uses (the nav group, the inverse map
    // of showPreviousTab, the "is the player open" check).
    m_pages->addWidget(m_downloadsPage);

    connect(m_home, &HomePage::openRequested, this, &MainWindow::openWatch);
    connect(m_home, &HomePage::resumeRequested, this,
            &MainWindow::openWatchResume);
    connect(m_home, &HomePage::navigateToSearchRequested, this,
            &MainWindow::goSearch);
    connect(m_search, &SearchPage::openRequested, this, &MainWindow::openWatch);
    // A YouTube link pasted in the search box is a video, not a query: the
    // PLAYER is opened with it (the page does not search it nor save it in the
    // history). The queue is a single item with only the id: the title,
    // channel and the rest arrive from /next.
    connect(m_search, &SearchPage::videoUrlRequested, this,
            [this](const QString &videoId, const QString &) {
                m_searchUrlId = videoId;
                VideoInfo v;
                v.id = videoId;
                openWatch(QVector<VideoInfo>{v}, 0);
            });
    // ...and the history gets the NAME, not the address: applyCurrentDetail
    // fills the empty title of the queue item when /next answers, and that is
    // when the page replaces the link in the field and writes the query.
    connect(&m_player, &PlayerController::metadataChanged, this, [this]() {
        if (m_searchUrlId.isEmpty())
            return;
        const VideoInfo cur = m_player.current();
        if (cur.id != m_searchUrlId || cur.title.isEmpty())
            return;
        const QString id = m_searchUrlId;
        m_searchUrlId.clear();
        m_search->applyVideoTitle(id, cur.title);
    });
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
                    showNormal();
                    updateMiniPlayer();
                }
                updateDownloadPlaybackState();
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
    // Row options button on the three surfaces without repos: the shared menu
    // is shown from the window, a single implementation.
    connect(m_search, &SearchPage::rowMenuRequested, this,
            &MainWindow::showVideoRowMenu);
    connect(m_channel, &ChannelPage::rowMenuRequested, this,
            &MainWindow::showVideoRowMenu);
    connect(m_playlist, &PlaylistPage::rowMenuRequested, this,
            &MainWindow::showVideoRowMenu);
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
    // A stylesheet set on a widget re-resolves its WHOLE subtree, so this line
    // overwrites any rule that a page set on one of its own widgets while being
    // built. WatchPage needs exactly one such rule -- the tooltips of its control
    // bar do not receive the app sheet, see WatchPage::applyAccent -- so it is
    // re-applied here, after this line, instead of in its constructor.
    m_watch->applyAccent();
    updateDownloadPlaybackState();
    // QMainWindow's status bar shows no message at all: it is hidden so the
    // bottom navigation bar touches the window's bottom edge (without the
    // empty bordered strip that stayed below).
    statusBar()->hide();

    // Saved "Tamaño de las miniaturas": applied before painting anything, via
    // the same single point as the hot change (applyThumbSize).
    if (m_settingsPage)
        applyThumbSize(m_settingsPage->thumbSize());

    goHome();
    m_home->reload();
    connect(&m_player, &PlayerController::phaseChanged, this,
            [this](const QString &) {
                updateMiniPlayer();
                updateDownloadPlaybackState();
            });
    connect(&m_player, &PlayerController::phaseChanged, this,
            [this](const QString &phase) {
                // "Generation on playback": once playback is confirmed
                // (mpv -> playing) the current video's seed yields the radio
                // that gets saved for Descubre. Only in NON-Radio mode: in
                // Radio mode WatchPage::triggerRadio already requested the
                // "A continuación" radio and this radioReady also gets saved
                // (onRadioReadyDiscover), avoiding duplicating the queue.
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
                // New video: live detection starts from scratch.
                m_miniLiveDet.reset();
                m_miniLive = false;
                updateMiniPlayer();
                updateDownloadPlaybackState();
                m_home->setCurrentVideoId(m_player.current().id);
                // When the video changes, copy to the Home cache if needed
            });
    connect(&m_player, &PlayerController::metadataChanged, this,
            [this]() { updateMiniBar(); });
    connect(&m_player, &PlayerController::volumeChanged, this,
            [this]() { updateMiniIcons(); });
    connect(&m_player, &PlayerController::positionChanged, this,
            [this](double pos, double dur) {
                // Same live rule as the overlay (nt::LiveDetector): unstable
                // duration evidence with automatic unlock. A normal video is
                // not marked; a real live fills the bar to the edge instead of
                // jumping backward on every segment.
                // EXCEPTION: the synthetic AV1 HLS (built by the app from a
                // finite fMP4) is never a live — it is excluded from the
                // detection even if its duration wobbles while loading.
                const bool appStream = !m_player.currentCanBeLive();
                if (appStream && m_miniLive)
                    m_miniLive = false;
                // AUTHORITATIVE signal (videoDetails.isLive): same criterion
                // as the overlay — immediate entry into EN VIVO mode, without
                // waiting for the 2 duration ascents (and it covers pure lives
                // with dur=0). The detector still governs the exit.
                if (!appStream && m_player.currentIsLive())
                    m_miniLiveDet.forceLive();
                const bool live = !appStream && m_miniLiveDet.feed(dur);
                m_miniLive = live;
                if (m_miniProgress) {
                    // dur<=0 (no duration yet) must NOT go into "busy" mode:
                    // the 0,0 range draws an animated chunk. On a live, the
                    // full bar marks the live edge (playhead at the end, like
                    // the overlay); with no duration, empty track.
                    if (dur > 0 && !m_miniLive) {
                        m_miniProgress->setRange(0, qRound(dur));
                        m_miniProgress->setValue(qRound(pos));
                    } else {
                        m_miniProgress->setRange(0, 1);
                        m_miniProgress->setValue(m_miniLive ? 1 : 0);
                    }
                }
                m_miniPos = pos;
                m_miniDur = dur;
                updateMiniTimeText();
            });
    connect(m_pages, &QStackedWidget::currentChanged, this,
            [this](int) {
                updateMiniPlayer();
                updateDownloadPlaybackState();
            });
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
    // Do not steal keys while typing in a text field.
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
        // Space must never activate a focused button: it is reserved
        // for play/pause. If focus is on a button, we consume it anyway
        // (with or without player) so the spacebar does not press it.
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
      case Qt::Key_D:
          // The menu of the download: the same one the button of the
          // Player opens.
          if (!m_player.isActive())
              return false;
          m_watch->showDownloadMenu();
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
        // -10 s (YouTube style).
        if (!m_player.isActive())
            return false;
        m_player.seek(m_player.position() - 10.0);
        return true;
    case Qt::Key_L:
        // +10 s (YouTube style).
        if (!m_player.isActive())
            return false;
        m_player.seek(m_player.position() + 10.0);
        return true;
    case Qt::Key_N:
        // Next video in the queue.
        if (!m_player.isActive())
            return false;
        if (m_player.hasNext())
            m_player.next();
        return true;
    case Qt::Key_P:
        // Same as the "Anterior" button (YouTube): if the video is past
        // kPrevRestartThreshold (5 s) restart the current video; if it is at
        // the start, jump to the previous video of the queue. On the first
        // video without progress it does nothing (there is no previous to jump
        // to), just like the disabled button. Shift+P goes through the same
        // case (the filter above only discards Ctrl/Alt/Meta): both do the
        // same.
        if (!m_player.isActive())
            return false;
        m_player.previous();
        return true;
    case Qt::Key_Up:
        if (!m_player.isActive())
            return false;
        // Respect volume_max (150/175/200 with boost): capping to 100 would
        // LOWER the volume if we are already above 100 while turning it up.
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
        // Fullscreen only from the player page: the shortcut must not scale
        // the window when the user is on Home, Settings, etc.
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

namespace {
// Tooltip state for MainWindow::eventFilter. The text of every widget that has
// one lives in a dynamic property (see the comment in the filter): QWidget's own
// property is emptied so Qt does not show the tip by itself, which is what lets
// the delay be driven from here.
const char *const kTooltipText = "_nt_tip_text";

QPointer<QWidget> g_tipArmed;   // widget whose tip is waiting to be shown
QTimer *g_tipTimer = nullptr;

// Gap between the anchor and its tip.
constexpr int kTipGap = 10;
// The offset Qt adds to the point it is given, (2, cursor height), measured on
// this session (qtooltip.cpp, placeTip).
constexpr int kCursorH = 16;

// How long the pointer has to rest before the tip appears. Qt's own
// SH_ToolTip_WakeUpDelay is around a second, which along the transport row feels
// slow; this is what the rest of the UI is tuned against.
constexpr int kTipWaitMs = 450;

// Qt places the tip itself: QToolTip::showText() offsets whatever point it gets
// by (2, cursor height) and flips it above when it does not fit below
// (qtooltip.cpp, placeTip). Flipping "above the cursor" is what puts it ON the
// player's control buttons, because the cursor is on the button.
//
// So the tip is shown and then placed exactly where it belongs: once Qt has built
// it, its real size is known (it is the only thing that cannot be guessed, the
// label wraps and the sheet sets the font), and the window is moved to sit below
// the anchor, or above it when there is no room -- clear of the anchor either
// way. The move happens in the same batch of events as the show, before the
// frame is composited, so it is not seen as a jump.
void tooltipShow()
{
    QWidget *w = g_tipArmed;
    g_tipArmed = nullptr;
    if (!w || !w->window()->isActiveWindow())
        return;
    const QString text = w->property(kTooltipText).toString();
    if (text.isEmpty())
        return;

    // Qt builds the tip at the point it is given plus (2, cursor height), and
    // repositions a REUSED tip from that same point (qtooltip.cpp, showText ->
    // placeTip). So the point decides where the tip lands in BOTH cases, and
    // giving it the TOP edge of the anchor -- which is what this used to do --
    // is what left the tip sitting over the button when moving quickly from one
    // control to the next, the case where the previous tip is still on screen
    // and gets reused. It is given the bottom edge instead, so it lands just
    // below the anchor.
    //
    // Hiding the tip before showing the new one is NOT the fix for that: Qt's
    // hideText() only starts the fade, so the next tip was created while the
    // previous one was still visible and two boxes showed at once.
    const QPoint anchorTopLeft2 = w->mapToGlobal(QPoint(0, 0));
    const int below = anchorTopLeft2.y() + w->height() + 1;
    QToolTip::showText(QPoint(anchorTopLeft2.x() - 2, below - kCursorH), text, w);

    const QPoint anchorTopLeft = w->mapToGlobal(QPoint(0, 0));
    const QRect anchor(anchorTopLeft, QSize(w->width(), w->height()));

    QTimer::singleShot(0, w, [anchor] {
        QWidget *tip = nullptr;
        for (QObject *o : QApplication::topLevelWidgets()) {
            auto *c = qobject_cast<QWidget *>(o);
            if (c && c->objectName() == QLatin1String("qtooltip_label"))
                tip = c;
        }
        if (!tip || !tip->isVisible())
            return;
        const QSize sz = tip->size();
        QScreen *scr = QGuiApplication::screenAt(anchor.topLeft());
        if (!scr)
            scr = QGuiApplication::primaryScreen();
        const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1920, 1080);

        int x = anchor.left();
        int y = anchor.bottom() + kTipGap;
        if (y + sz.height() > avail.bottom())
            y = anchor.top() - sz.height() - kTipGap;
        x = qBound(avail.left(), x, avail.right() - sz.width());
        y = qBound(avail.top(), y, avail.bottom() - sz.height());
        tip->move(x, y);
    });
}

void tooltipArm(QWidget *w)
{
    if (!w || w->property(kTooltipText).toString().isEmpty())
        return;
    if (g_tipArmed == w)
        return;   // already waiting: do not restart the delay while moving
    if (!g_tipTimer) {
        g_tipTimer = new QTimer(qApp);
        g_tipTimer->setSingleShot(true);
        QObject::connect(g_tipTimer, &QTimer::timeout, &tooltipShow);
    }
    g_tipArmed = w;
    g_tipTimer->start(kTipWaitMs);
}

void tooltipDisarm()
{
    g_tipArmed = nullptr;
    if (g_tipTimer)
        g_tipTimer->stop();
}

QWidget *tooltipArmed() { return g_tipArmed; }
} // namespace

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // TOOLTIP: Qt shows ONE box for the whole row of controls.
    //
    // Qt hides the tip on any Leave (QTipLabel installs itself as an app-wide
    // event filter, qtooltip.cpp:188), but then the next one comes back almost
    // immediately: while a tip has been showing, the wake-up delay drops from
    // SH_ToolTip_WakeUpDelay to a hardcoded 20 ms, and that fast path stays
    // armed for SH_ToolTip_FallAsleepDelay (~10 s) after the last show
    // (qapplication.cpp:1904 and :3062). Going along the transport buttons
    // therefore reads as a single box whose text changes.
    //
    // The delay is driven from the app instead, so leaving a button really closes
    // its tip and the next one waits the normal time under the button it belongs
    // to:
    //   - the FIRST QEvent::ToolTip of a widget is consumed: the text moves to a
    //     dynamic property and QWidget::toolTip is emptied, so Qt shows nothing by
    //     itself (that property is also what carries the translated text);
    //   - Enter arms a single-shot timer with the normal wake delay;
    //   - Leave stops it and closes the tip;
    //   - the timer calls QToolTip::showText(), so the tip is still Qt's own
    //     window: styled by the sheet, positioned by Qt.
    if (watched->isWidgetType()) {
        auto *w = qobject_cast<QWidget *>(watched);
        switch (event->type()) {
        case QEvent::ToolTip:
            if (!w->toolTip().isEmpty()) {
                // First contact: take the text over and swallow the event, so Qt
                // never puts its own window on screen by itself.
                const QString text = w->toolTip();
                w->setProperty(kTooltipText, text);
                w->setToolTip(QString());
                // Emptying toolTip() also drops what a screen reader used to get
                // as this widget's description, so it is kept there.
                if (w->accessibleDescription().isEmpty())
                    w->setAccessibleDescription(text);
                tooltipArm(w);
                return true;
            }
            if (!w->property(kTooltipText).toString().isEmpty()) {
                tooltipArm(w);
                return true;
            }
            break;
        case QEvent::Enter:
            if (!w->property(kTooltipText).toString().isEmpty())
                tooltipArm(w);
            break;
        case QEvent::Leave:
            if (!w->property(kTooltipText).toString().isEmpty()) {
                if (tooltipArmed() == w)
                    tooltipDisarm();
                // The tip goes away at once instead of lingering while the
                // pointer crosses over to the next control.
                QToolTip::hideText();
            }
            break;
        default:
            break;
        }
    }
    if (event->type() == QEvent::Resize && watched == m_miniTitle) {
        updateMiniTitleElide();
        return false;
    }
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
    // Height of the bar. It is the LAST widget of the root layout, so it stays
    // stuck to the window's bottom edge no matter its height (trimming it here
    // does NOT separate it from the edge). Each button is a FIXED 104x52
    // capsule (not Expanding): the icon 26 + gap + 11px text line content
    // needs ~46 and with the QSS 3px vertical padding (theme.cpp,
    // QToolButton#navBtn) 52 fits in 60. Lowering the height more clips the
    // text or the icon, it does not shrink them.
    m_nav->setFixedHeight(60);

    auto *lay = new QHBoxLayout(m_nav);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);            // gap between capsules

    auto *group = new QButtonGroup(m_nav);
    group->setExclusive(true);

    auto addBtn = [&](const QString &key, int index) {
        auto *b = new NavButton(m_nav);
        b->setObjectName(QStringLiteral("navBtn"));
        b->setCheckable(true);
        b->setProperty("navKey", key);
        // The label is BOUND (not set): the bottom bar is static chrome, so on
        // a language change the text follows without rebuilding the bar. The
        // binding is applied at the call site so the key is a visible literal.
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        // Compact capsule: fixed width, not 1/4 of the window. With Expanding
        // the active tab's fill measured 474 px in an 1896 window and read as
        // a band, not as a position indicator.
        b->setFixedSize(104, 52);
        b->setIconSize(QSize(26, 26));
        b->updateIcon();
        group->addButton(b, index);
        lay->addWidget(b);
        m_navButtons.append(b);
        return b;
    };
    // The group goes CENTERED: a stretch on each side.
    lay->addStretch(1);
    {
        QToolButton *b0 = addBtn(QStringLiteral("home"), 0);
        nt::bindText(b0, "nav.home");
        QToolButton *b1 = addBtn(QStringLiteral("search"), 1);
        nt::bindText(b1, "nav.search");
        QToolButton *b2 = addBtn(QStringLiteral("library"), 2);
        nt::bindText(b2, "nav.library");
        QToolButton *b3 = addBtn(QStringLiteral("downloads"), 3);
        nt::bindText(b3, "nav.downloads");
        QToolButton *b4 = addBtn(QStringLiteral("settings"), 4);
        nt::bindText(b4, "nav.settings");
    }
    lay->addStretch(1);

    connect(group, QOverload<int>::of(&QButtonGroup::idClicked), this,
            &MainWindow::onNavButton);
}

void MainWindow::onNavButton(int index)
{
    m_lastTab = m_currentTab;
    switch (index) {
    case 0:
        m_currentTab = 0;
        // Save BEFORE refreshing: refreshDynamic() reads the history position
        // to paint "Continuar viendo", and here it is called with the page
        // still hidden (the setCurrentIndex comes after), so HomePage's
        // visibility guard skips the changed() of the save.
        m_player.savePositionNow();
        m_home->refreshDynamic();
        break;
    case 1:
        m_currentTab = 1;
        break;
    case 2:
        m_currentTab = 4;
        break;
    case 3:
        m_currentTab = 8;  // the downloads page
        break;
    case 4:
        m_currentTab = 5;  // settings is the LAST button of the bar
        break;
    default:
        return;
    }
    m_pages->setCurrentIndex(m_currentTab);
    if (m_currentTab == 4)
        m_library->reload();
    else if (m_currentTab == 5) {
        m_settingsPage->setVisible(true);
    } else if (m_currentTab == 8 && m_downloadsPage) {
        m_downloadsPage->reload();
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
    lay->setContentsMargins(12, 8, 12, 4);
    lay->setSpacing(3);

    auto *row = new QHBoxLayout;
    row->setSpacing(6);

    // Fixed slot where the live PlayerWidget is embedded when the mini is
    // active; otherwise it shows the static thumbnail.
    m_miniVideoHost = new QWidget(m_miniBar);
    m_miniVideoHost->setObjectName(QStringLiteral("miniVideoHost"));
    m_miniVideoHost->setFixedSize(112, 63);
    m_miniVideoHost->setCursor(Qt::PointingHandCursor);
    auto *hostLay = new QVBoxLayout(m_miniVideoHost);
    hostLay->setContentsMargins(0, 0, 0, 0);
    hostLay->setSpacing(0);
    m_miniThumb = new RoundedThumb(m_miniVideoHost);
    m_miniThumb->setAlignment(Qt::AlignCenter);
    m_miniThumb->setObjectName(QStringLiteral("miniThumb"));
    m_miniThumb->setTextInteractionFlags(Qt::NoTextInteraction);
    hostLay->addWidget(m_miniThumb);
    row->addWidget(m_miniVideoHost);

    // Text block: title on top and, below, "Canal · time" at the left
    // (adjacent, not right-aligned: next to the channel it reads well and does
    // not end up under the buttons).
    auto *textBox = new QVBoxLayout;
    textBox->setSpacing(2);
    m_miniTitle = new QLabel(nt::tr("mini.nothing"), m_miniBar);
    m_miniTitle->setObjectName(QStringLiteral("miniTitle"));
    m_miniTitle->setTextInteractionFlags(Qt::NoTextInteraction);
    // Single line with ellipsis: updateMiniTitleElide() truncates to the real
    // width and the window's eventFilter recomputes it when the label is
    // resized.
    m_miniTitle->setWordWrap(false);
    m_miniTitleFull = nt::tr("mini.nothing");
    m_miniTitle->installEventFilter(this);
    m_miniTime = new QLabel(QStringLiteral("0:00 / 0:00"), m_miniBar);
    m_miniTime->setObjectName(QStringLiteral("miniTime"));
    m_miniTime->setTextInteractionFlags(Qt::NoTextInteraction);
    textBox->addWidget(m_miniTitle);
    textBox->addWidget(m_miniTime);
    row->addLayout(textBox, 1);

    m_miniPlayBtn = new QPushButton(m_miniBar);
    m_miniPlayBtn->setObjectName(QStringLiteral("iconBtn"));
    m_miniPlayBtn->setCursor(Qt::PointingHandCursor);
    m_miniPlayBtn->setIcon(nt::playerIcon(nt::PlayerIcon::Play, nt::kText, 16));
    nt::bindTooltip(m_miniPlayBtn, "mini.playPause");
    row->addWidget(m_miniPlayBtn);

    m_miniNextBtn = new QPushButton(m_miniBar);
    m_miniNextBtn->setObjectName(QStringLiteral("iconBtn"));
    m_miniNextBtn->setCursor(Qt::PointingHandCursor);
    m_miniNextBtn->setIcon(nt::playerIcon(nt::PlayerIcon::Next, nt::kText, 16));
    nt::bindTooltip(m_miniNextBtn, "mini.next");
    row->addWidget(m_miniNextBtn);

    m_miniMuteBtn = new QPushButton(m_miniBar);
    m_miniMuteBtn->setObjectName(QStringLiteral("iconBtn"));
    m_miniMuteBtn->setCursor(Qt::PointingHandCursor);
    m_miniMuteBtn->setIcon(
        nt::playerIcon(nt::PlayerIcon::Volume, nt::kText, 16));
    nt::bindTooltip(m_miniMuteBtn, "mini.mute");
    row->addWidget(m_miniMuteBtn);

    m_miniCloseBtn = new QPushButton(m_miniBar);
    m_miniCloseBtn->setObjectName(QStringLiteral("iconBtn"));
    m_miniCloseBtn->setCursor(Qt::PointingHandCursor);
    m_miniCloseBtn->setIcon(nt::playerIcon(nt::PlayerIcon::Close, nt::kText, 16));
    nt::bindTooltip(m_miniCloseBtn, "mini.close");
    row->addWidget(m_miniCloseBtn);

    lay->addLayout(row);

    // Full-width progress stuck to the bottom edge: it lives OUTSIDE the layout
    // (which has side margins) and MiniPlayerBar::resizeEvent repositions it.
    m_miniProgress = new QProgressBar(m_miniBar);
    m_miniProgress->setObjectName(QStringLiteral("miniProgress"));
    m_miniProgress->setTextVisible(false);
    m_miniProgress->setFixedHeight(4);
    // Progress clicks must not steal the bar's "expand".
    m_miniProgress->setAttribute(Qt::WA_TransparentForMouseEvents);
    bar->progress = m_miniProgress;

    connect(m_miniPlayBtn, &QPushButton::clicked, this, [this]() {
        m_player.playPause();
        updateMiniIcons();
    });
    connect(m_miniNextBtn, &QPushButton::clicked, this, [this]() {
        if (m_player.hasNext())
            m_player.next();
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

// Mini player visibility rule: never at the same time as the Player page.
// It is shown when leaving Player with active/paused playback and hidden when
// going back to Player or when playback stops (isActive()==false).
void MainWindow::updateDownloadPlaybackState()
{
    if (!m_downloadManager)
        return;
    // Real-time playback is what the downloader must yield to: a video that is
    // PLAYING (Player page, full screen or the mini player, the three of them
    // paint frames continuously) or LOADING (the moment the stream is opened,
    // the most sensitive one). A paused, stopped or failed video does not need
    // CPU, so the downloads go back to full speed by themselves. This is the
    // whole recoverability of the scheduling: no state is saved anywhere, it
    // is derived from the player on every phase change.
    const QString phase = m_player.phase();
    const bool realtime = m_player.isActive()
                          && (phase == QLatin1String("playing")
                              || phase == QLatin1String("loading"));
    m_downloadManager->setPlaybackActive(realtime);
}

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
    updateDownloadPlaybackState();
    // Live video in the mini: on showing it, the PlayerWidget is reparented to
    // the mini bar slot and keeps rendering (no decoder reconf and no
    // accumulated frames). On returning to the Player page it is reinserted
    // into the video area. Only on state changes, not on every call.
    if (show && !m_miniDetached && m_watch && m_miniVideoHost) {
        m_watch->detachPlayerTo(m_miniVideoHost);
        // The thumbnail mode: "video" shows the live video; "thumb" puts the
        // static thumbnail ON TOP of the PlayerWidget (the video keeps
        // rendering behind: hot pipeline, no reconf and no backlog).
        m_miniDetached = true;
        applyMiniVideoMode();
    } else if (!show && m_miniDetached && m_watch) {
        m_watch->attachPlayer();
        m_miniDetached = false;
    }
    if (show)
        updateMiniBar();
}

// Clicking anywhere on the mini bar (except its buttons) returns to the full
// Player without re-resolving the stream: playback is already alive in the
// same PlayerController throughout the navigation.
void MainWindow::goToPlayerPage()
{
    m_lastTab = m_currentTab;
    m_currentTab = 3;
    m_pages->setCurrentIndex(3);
}

// Applies the mini player mode while there is embedded video (detached):
// "video" → the PlayerWidget stays visible (thumbnail hidden);
// "thumb"  → the static thumbnail goes on top and the video renders behind.
// In no case does mpv stop drawing, hence there is no jump on return.
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

void MainWindow::applyThumbSize(const QString &value)
{
    // Lists (search, channel, history, playlists, "A continuación" queue):
    // the row and the thumbnail scale with the factor.
    const qreal scale = SettingsPage::thumbListScale(value);
    if (m_pages) {
        const auto lists = m_pages->findChildren<VideoListView *>();
        for (VideoListView *l : lists)
            if (l)
                l->setDensity(scale);
    }
    // Home grids: here the factor does nothing visible (the width dimensions
    // the cell with the fixed columns), so the lever is the number of columns
    // per row. HomePage applies the width cap to it so the cell never drops
    // below kMinCellW in narrow windows.
    if (m_home)
        m_home->setCellColumns(SettingsPage::thumbGridColumns(value));
}

void MainWindow::applyLanguage(const QString &code)
{
    if (!nt::setLanguage(code)) {
        NT_DEBUG("E2E ui_lang: %s no existe, se mantiene %s\n",
                 code.toUtf8().constData(),
                 nt::language().toUtf8().constData());
        return;
    }
    // Settings: rebuild deferred. The signal is emitted from a QMenu born in
    // that page; recreating its widgets synchronously would delete the menu
    // while it is still inside its own delivery.
    QTimer::singleShot(0, this, [this]() {
        if (m_settingsPage)
            m_settingsPage->retranslate();
    });
    // Library: the section titles are "Title · N" (not bindable) and the
    // "Ver todo / Ver menos" toggles are dynamic; the page re-applies them.
    if (m_library)
        m_library->retranslate();
    // Pages that compose text with data or wrap it in the empty-state HTML (the
    // logo): the static chrome is live-bound and re-applied by
    // nt::setLanguage(), but these need to recompose.
    if (m_search)
        m_search->retranslate();
    if (m_channel)
        m_channel->retranslate();
    if (m_watch)
        m_watch->retranslate();
    if (m_playlist)
        m_playlist->retranslate();
    if (m_historyPage)
        m_historyPage->retranslate();
    // List rows paint their meta line (channel · views · date) and the "EN VIVO"
    // badge with nt::tr AT PAINT TIME (format::metaLine plus the delegate and
    // AsyncThumb): neither a binding nor a page retranslate covers them, so the
    // tree is repainted.
    const auto widgets = findChildren<QWidget *>();
    for (QWidget *w : widgets)
        w->update();
    // Shell text that is NOT a binding because it depends on the playback
    // state: the mini title fallback and the "no thumbnail" hint.
    const VideoInfo cur = m_player.current();
    m_miniTitleFull = (cur.id.isEmpty() || cur.title.isEmpty())
                          ? nt::tr("mini.nothing")
                          : cur.title;
    updateMiniTitleElide();
    if (m_miniBar && m_miniBar->isVisible())
        updateMiniBar();
}

void MainWindow::updateMiniBar()
{
    if (!m_miniBar || !m_miniBar->isVisible())
        return;
    const VideoInfo v = m_player.current();
    if (v.id.isEmpty()) {
        m_miniTitleFull = nt::tr("mini.nothing");
        updateMiniTitleElide();
        m_miniThumb->setText(QString());
        m_miniProgress->setRange(0, 1);
        m_miniProgress->setValue(0);
        return;
    }
    m_miniChannel = v.channel;
    m_miniTitleFull = v.title.isEmpty() ? nt::tr("mini.nothing")
                                        : v.title;
    updateMiniTitleElide();
    updateMiniTimeText();

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
        m_miniThumb->setText(nt::tr("mini.noThumb"));
        return;
    }
    m_miniThumb->setText(QString());
    // With a real thumbnail the QSS gray background (miniThumb) gets in the
    // way: the bitmap is the content, so only the radius remains. It is NOT a
    // pending-to-migrate theme inline, it is dynamic state (with or without
    // thumbnail).
    m_miniThumb->setStyleSheet(
        QStringLiteral("background:transparent; border-radius:6px;"));
    m_miniThumb->setPixmap(
        pm.scaled(m_miniThumb->size(), Qt::KeepAspectRatioByExpanding,
                  Qt::SmoothTransformation));
}

// Persists the controller's current volume and mute (src/db/settings: keys
// "volume" / "volume_muted"); MainWindow::persistVolume() at startup applies
// them again. Called after every user-made change.
// NOTE: if the "Volumen máximo" setting is active (>100), the persisted volume
// is CAPPED to 100 so that on app restart the slider starts at 100% and does
// NOT sit at the previous session's high value (hearing protection).
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
    m_miniPlayBtn->setIcon(nt::playerIcon(
        m_player.isPaused() ? nt::PlayerIcon::Play : nt::PlayerIcon::Pause,
        nt::kText, 16));
    m_miniMuteBtn->setIcon(nt::playerIcon(
        m_player.muted() ? nt::PlayerIcon::Muted : nt::PlayerIcon::Volume,
        nt::kText, 16));
    if (m_miniNextBtn) {
        m_miniNextBtn->setEnabled(m_player.hasNext());
        m_miniNextBtn->setIcon(
            nt::playerIcon(nt::PlayerIcon::Next, nt::kText, 16));
    }
}

// The mini title fits one line: elided to the real width (the eventFilter
// recomputes it when the label is resized).
void MainWindow::updateMiniTitleElide()
{
    if (!m_miniTitle)
        return;
    const QString elided = m_miniTitle->fontMetrics().elidedText(
        m_miniTitleFull, Qt::ElideRight, qMax(0, m_miniTitle->width()));
    m_miniTitle->setText(elided);
}

// Mini's second line: "Canal · m:ss / m:ss". A live has no final duration:
// only the "EN VIVO" mark (the bar runs full to the live edge). With the
// duration still unknown (start of a normal video) only the elapsed time is
// shown, to avoid painting a "/ 0:00" that jumps once it materializes.
void MainWindow::updateMiniTimeText()
{
    if (!m_miniTime)
        return;
    QString t;
    if (m_miniLive)
        t = nt::tr("mini.live");
    else if (m_miniDur <= 0)
        t = fmtTime(m_miniPos);
    else
        t = QStringLiteral("%1 / %2")
                .arg(fmtTime(m_miniPos), fmtTime(m_miniDur));
    if (!m_miniChannel.isEmpty())
        t = m_miniChannel + QStringLiteral(" · ") + t;
    m_miniTime->setText(t);
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

// The row that a video id names. The page of Descargas passes the KEY of the
// row (the mp3 is "<videoId>-audio"), but the command line passes the VIDEO
// id: the row that EXISTS is the one that is paused or continued. With the
// plain id an mp3 was neither (the key of its row is not the video id).
static QString downloadRowKey(DownloadRepo *repo, const QString &idOrKey)
{
    if (!repo)
        return idOrKey;
    DownloadInfo info;
    if (repo->get(idOrKey, info))
        return idOrKey;
    const QString audioKey = downloadKey(idOrKey, true);
    if (repo->get(audioKey, info))
        return audioKey;
    return idOrKey;
}

// Pauses a download (the button of the page of Descargas).
void MainWindow::pauseDownload(const QString &idOrKey)
{
    if (m_downloadManager)
        m_downloadManager->pause(downloadRowKey(&m_downloads, idOrKey));
}

// Continues a paused download (the button of the page of Descargas).
void MainWindow::resumeDownload(const QString &idOrKey)
{
    if (m_downloadManager)
        m_downloadManager->resume(downloadRowKey(&m_downloads, idOrKey));
}

// Revives the queue that a previous session left half way. It is called by
// main.cpp when the app opens (NOT by the constructor: see the header).
void MainWindow::resumePendingDownloads()
{
    if (!m_downloadManager)
        return;
    // The folders of fragments that no row claims (a download deleted by a
    // version that did not remove them, or a restored database) are not going
    // to be used by anybody: hundreds of MB. It goes BEFORE the revive, and
    // the row of a download that is going to be revived keeps its folder
    // (the sweep only removes what has no row).
    m_downloadManager->sweepOrphanParts();
    m_downloadManager->resumePending();
}

// Queues a download by id.
void MainWindow::queueDownload(const QString &videoId)
{
    if (!m_downloadManager)
        return;
    m_downloadManager->start(videoId, QString(), QString(), 0);
}

// Queues the download of a video: it is what the button of the Player does.
//
// With a height (or audioOnly) the dialog of the download is SKIPPED and
// that is what is downloaded: that is how it is driven from the command
// line, where there is nobody to ask.
void MainWindow::downloadVideo(const QString &videoId, int height,
                                bool audioOnly)
{
    if (!m_downloadManager)
        return;
    m_downloadManager->start(videoId, QString(), QString(), 0, 0, height,
                             QString(), audioOnly);
    // The tab of the downloads is opened: the progress is there, and the
    // person asked for a download.
    openDownloads();
}

QAbstractButton *MainWindow::downloadButton()
{
    if (!m_watch)
        return nullptr;
    return m_watch->findChild<QAbstractButton *>(
        QStringLiteral("downloadButton"));
}

// Plays a local file (a download).
void MainWindow::playLocal(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return;
    VideoInfo info;
    info.id = QFileInfo(path).completeBaseName();
    info.title = info.id;
    m_player.playLocalFile(path, info, 0);
    goToPlayerPage();
}

// Opens the "Descargas" tab: the nav button of the bottom bar (id 4).
void MainWindow::openDownloads()
{
    m_currentTab = 8;
    m_pages->setCurrentIndex(m_currentTab);
    if (m_downloadsPage)
        m_downloadsPage->reload();
    if (m_navButtons.size() > 3)
        m_navButtons[3]->setChecked(true);
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

// Options menu for a video row, for the surfaces that do not have the repos
// (Search, Channel, Playlist): the window does, and that is why the menu is
// built HERE and not again on every page. The actions are the SAME ones
// offered by Library, History and "A continuación" (nt::addRowMenuActions),
// and the only thing this window adds is the "Nueva playlist" popup (the
// Library one) and the navigation to the channel.
void MainWindow::showVideoRowMenu(const VideoInfo &video, const QPoint &globalPos)
{
    if (video.id.isEmpty())
        return;
    QMenu menu(this);
    nt::RowMenuHooks hooks;
    hooks.onNewPlaylist = [this](const QString &videoId) {
        m_library->createPlaylistForVideo(videoId);
    };
    hooks.onChannel = [this](const QString &channelId, const QString &) {
        openChannel(channelId);
    };
    nt::addRowMenuActions(&menu, video, &m_watchLater, &m_playlists, hooks);
    menu.exec(globalPos);
}

// The folder of the downloads: the one of Ajustes, or the default one. The
// downloader writes there and the page of Descargas opens it.
void MainWindow::applyDownloadFolder()
{
    if (!m_downloadManager)
        return;
    const QString folder = m_settings.getString(
        QString::fromLatin1(downloadsettings::kFolderKey), QString());
    m_downloadManager->setDirectory(downloads::folderOf(folder));
    if (m_downloadsPage)
        m_downloadsPage->setFolder(m_downloadManager->directory());
    NT_DEBUG("E2E downloads folder=%s\n",
             m_downloadManager->directory().toUtf8().constData());
}

void MainWindow::resetAppData()
{
    // 1. Quiesce the player (avoids position writes on exit)
    m_player.stop();
    // 2. Close SQLite and allow deletion on Windows
    m_db.close();
    // 3. Delete the application's own data (not personal files)
    const QString dbPath = m_db.path();
    if (!dbPath.isEmpty()) {
        QFile::remove(dbPath);
        QFile::remove(dbPath + QStringLiteral("-wal"));
        QFile::remove(dbPath + QStringLiteral("-shm"));
    }
    // 3b. Delete the on-disk HLS resolution cache (avoids reusing expired/
    // throttled URLs; the DB no longer stores any of this).
    QFile::remove(Innertube::hlsCacheFilePath());
    // 4. Clear thumbnail caches (own directories only)
    for (ThumbnailCache *t : {m_homeThumbs, m_globalThumbs,
                                m_libraryThumbs}) {
        if (t)
            t->clear();
    }
    // 5. Restart the application with defaults; the DB will be recreated on
    // open
    QProcess::startDetached(
        QCoreApplication::applicationFilePath(),
        QCoreApplication::arguments(),
        QDir::currentPath());
    // The current process exits; the new instance starts clean.
    QTimer::singleShot(200, this, []() { qApp->quit(); });
}

void MainWindow::showPreviousTab()
{
    int dest = m_lastTab;
    if (dest == m_currentTab || dest < 0 || dest >= m_pages->count())
        dest = 0;
    m_pages->setCurrentIndex(dest);
    m_currentTab = dest;
    if (dest == 0) {
        // Same as in onNavButton: the position is saved before
        // "Continuar viendo" reads it from the DB.
        m_player.savePositionNow();
        m_home->refreshDynamic();
    }
    // Channel (2) and player (3) have no nav button: mark none.
    if (m_currentTab == 2 || m_currentTab == 3)
        return;
    // The pages of the bar: library (4), downloads (8) and settings (5).
    // The SETTINGS button is the last one (index 4).
    const int nav = m_currentTab == 4 ? 2 : m_currentTab == 8 ? 3
                   : m_currentTab == 5 ? 4 : m_currentTab;
    if (nav < m_navButtons.size())
        m_navButtons[nav]->setChecked(true);
}

void MainWindow::applyCodecProfile(const QString &profile, bool persist)
{
    // Active profile in memory (the ladder compares against this; see
    // m_activeProfile).
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
        // No HW: h264 (universal) -> Legacy profile, aggressive wait and 1080p
        // cap (1440p+ kills a pure CPU).
        m_yt->setCodecProfile(Yt::CodecProfile::Legacy);
        m_playerWidget->setInitialBufferSecs(6.0);
        if (m_player.maxHeight() > 1080)
            m_player.setMaxHeight(1080);
    } else {
        // h264-hw (and unknown profiles): h264 by hardware, margin under the
        // throttle -> short wait.
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
    // Style in the global QSS (QWidget#hwProbePopup): this used to be an
    // inline with rgba(18,18,18,0.96) and #3a3a3a, which did not exist as
    // tokens.
    auto *lay = new QVBoxLayout(m_hwPopup);
    lay->setContentsMargins(28, 22, 28, 22);
    lay->setSpacing(8);
    auto *title = new QLabel(m_hwPopup);
    title->setObjectName(QStringLiteral("hwPopupTitle"));
    title->setAlignment(Qt::AlignCenter);
    nt::bindText(title, "hw.detecting");
    auto *text = new QLabel(m_hwPopup);
    text->setObjectName(QStringLiteral("hwPopupBody"));
    text->setAlignment(Qt::AlignCenter);
    nt::bindText(text, "hw.note");
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
    // HwProbe is single-use (m_done): re-create it on every re-detection,
    // otherwise start() would be a no-op and m_hwProbing would stay hung
    // forever.
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
                    // Indeterminate (the clips do not decode): conservative
                    // profile without persisting. Do NOT assume HW (that was
                    // the false positive); the probe retries on the next
                    // launch (the probe version is not marked).
                    applyCodecProfile(QStringLiteral("soft-all"), false);
                } else {
                    applyCodecProfile(profile, true);
                    // Probe of this version completed: not repeated on every
                    // launch (critical on slow GPUs).
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
    // The notice only appears if the probe takes a while (basic PC); on a fast
    // PC the result arrives before 300 ms and the user sees nothing.
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

// Captures ANY radio that arrives (from "A continuación"'s triggerRadio in
// Radio mode or from generation by player playback) and saves it as a
// Descubre source. Idempotent per seed: one radio per played video. The
// metadata of its videos goes into the "videos" cache so Descubre can resolve
// them without network.
void MainWindow::onRadioReadyDiscover(const QString &videoId,
                                      const QVector<VideoInfo> &videos)
{
    if (videoId.isEmpty() || videoId.size() != 11)
        return;
    if (videos.isEmpty()) {
        m_radioSeedsRequested.remove(videoId);
        return;
    }
    // Cache the metadata: done BEFORE touching the saved radio, because this
    // arrival can also be a REFRESH (the saved radio had incomplete metadata
    // and "A continuación" requested it again).
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
    if (m_discoverRadios.hasRadio(videoId)) {
        // Radio already saved: this arrival is a metadata refresh, so its id
        // list is rewritten (saveRadio does not overwrite on purpose). Without
        // this, the radio would keep pointing at the old ids (no views nor
        // date) and every opening would refresh again in vain.
        NT_DEBUG("E2E discover radio refresh seed=%s n=%d\n",
                 videoId.toUtf8().constData(), (int)valid.size());
        m_discoverRadios.clearForSeed(videoId);
    }
    m_discoverRadios.saveRadio(videoId, valid);
    // The system already generated radios at some point: an empty Descubre
    // (e.g. after "Borrar todo" on the history) does not auto-seed itself
    // again.
    m_settings.set(QStringLiteral("discover_seeded"), QStringLiteral("1"));
    NT_DEBUG("E2E discover radio guardada seed=%s n=%d total=%d\n",
             videoId.toUtf8().constData(), (int)valid.size(),
             m_discoverRadios.count());
    // Rebuild the Home sections only with the page VISIBLE: with playback on
    // the Player page (Home hidden) this refresh re-reads the history while
    // the just-opened video still has pos=0 (start=0 until the first 20 s
    // flush) and would CLEAR "Continuar viendo" for nothing (m_continueVideos
    // stays frozen at the empty result because the visibility guard of
    // refreshContinueWatching skips the recalculation while hidden). Entering
    // Home always re-runs refreshDynamic (onNavButton), so nothing is lost.
    if (m_home && m_home->isVisible())
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
    // Defensive guard: data coming from network/DB must not be able to crash
    // on an invalid index (empty vector or startIndex out of range).
    if (startIndex < 0 || startIndex >= videos.size()) {
        NT_DEBUG("E2E openWatchAt rechazado startIndex=%d size=%d\n",
                 startIndex, (int)videos.size());
        return;
    }
    // Hardware probe in progress: playback not possible yet. The request (with
    // its resume position) is queued and the notice is shown, which closes
    // when the probe finishes, at which point the video opens.
    if (m_hwProbing) {
        m_pendingWatch = videos;
        m_pendingIndex = startIndex;
        m_pendingPos = startPositionMs;
        m_hasPendingWatch = true;
        showHwPopup();
        return;
    }
    // Stop seeding Descubre radios: on opening a video the "A continuación"
    // radio becomes the only consumer of fetchRadio() (single-flight in
    // Innertube).
    if (m_home)
        m_home->interruptDiscover();
    // Go to the Player page BEFORE activating the queue: if activated first,
    // updateMiniPlayer sees "playing outside Player" and attaches the mini for
    // a frame, then detaches it on page change (flicker when opening).
    m_lastTab = m_currentTab;
    m_currentTab = 3;
    m_pages->setCurrentIndex(3);
    const NextMode mode = nextMode();
    if (mode == NextMode::Radio) {
        // Radio mode: open with only the clicked video and generate the queue
        // of the RDAMVM mix for "A continuación". An explicit click from
        // outside the list (search, home, channel) triggers the radio; inside
        // the list it is not regenerated.
        m_player.playQueue(QVector<VideoInfo>{videos.at(startIndex)}, 0,
                           startPositionMs);
        m_watch->setWatchData(m_player.queue(), m_player.index());
        const QString &id = videos.at(startIndex).id;
        // This video's radio was already generated before: reused from the DB
        // for "A continuación" without asking YouTube again. Only if the
        // metadata of its videos is complete (channel + views or date): a
        // radio saved by an earlier parser version does not bring it and gets
        // refreshed once on opening (the new fetch does, see
        // Innertube::requestRadioWeb). After that refresh the cache holds
        // metadata and the following launches reuse without network.
        bool reused = false;
        if (id.size() == 11 && m_discoverRadios.hasRadio(id)
            && !m_discoverRadios.radioVideoIdsForSeed(id).isEmpty()) {
            const QStringList saved = m_discoverRadios.radioVideoIdsForSeed(id);
            const QVector<VideoInfo> fromCache = m_videoCache.getMany(saved);
            // It is enough for the radio to carry a channel and SOME metadata
            // (views or date): a radio saved by a parser that did not bring
            // them has all rows at 0/empty and gets refreshed once; the videos
            // YouTube returns without date (lives, premieres) force nothing.
            bool anyMeta = false;
            bool anyChannel = false;
            for (const VideoInfo &cv : fromCache) {
                if (!cv.channel.isEmpty())
                    anyChannel = true;
                if (cv.viewCount > 0 || !cv.publishedAt.isEmpty())
                    anyMeta = true;
            }
            // The Shorts (the vertical ones) are left out even of a radio
            // that was saved before: they are not what "A continuación" is
            // for.
            const QVector<VideoInfo> clean =
                reels::withoutShorts(fromCache);
            if (!clean.isEmpty() && anyMeta && anyChannel) {
                NT_DEBUG("E2E nextUp radio reuse id=%s n=%d\n",
                         id.toUtf8().constData(), (int)clean.size());
                m_player.appendVideos(clean);
                reused = true;
            } else {
                NT_DEBUG("E2E nextUp radio stale id=%s n=%d -> refresh\n",
                         id.toUtf8().constData(), (int)fromCache.size());
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
    // New instance after a gated-session restart: resumes where it was cut
    // off (a single video: the queue is not restored).
    if (m_gateResumeMs > 0) {
        openWatchResume(QVector<VideoInfo>{v}, 0, m_gateResumeMs);
        m_gateResumeMs = 0;
        return;
    }
    openWatch(QVector<VideoInfo>{v}, 0);
}

void MainWindow::onRecoveryExhausted()
{
    // The app's ONLY auto-restart and ONLY on this error (gated-session
    // recovery exhausted): relaunches with the last video, because restarting
    // gets out of the gated session (user verification: on restart the audio
    // is fixed). In-memory guard: the same episode does not restart twice
    // (PlayerController can re-emit the signal in an EOF cascade).
    if (m_gateRestarting)
        return;
    m_gateRestarting = true;
    // Guard in settings: if this instance ALREADY came from a restart
    // (armed=1), the new session is also gated -> the error is shown and the
    // guard is disarmed so restarts do not chain in a loop.
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
    // The new instance starts with "http://..." -> playVideo resumes the last
    // video (see gate_restart_armed in the constructor).
    QTimer::singleShot(200, this, []() { qApp->quit(); });
}

void MainWindow::onRecoveryHealthy()
{
    // Stable playback ≥5 min after the restart: the episode is over and the
    // next gated session may restart again.
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

// ---------------------------------------------------------------------------
// Updater
//
// The rules, and why each one is here:
//   - the query is ASYNC (Qt5 Network) and there is never more than one in
//     flight: nothing of the interface blocks;
//   - the automatic check runs once per session, 20 s after opening, and only
//     if the last one (persisted in "update_last_check") is older than 24 h:
//     the API is not polled and a restart does not ask GitHub again;
//   - it is NEVER shown on top of a video that is playing: the release stays
//     pending and the dialog appears when the player is at rest;
//   - "Mas tarde" silences that version for the automatic check
//     ("update_skip_version"); the manual button always answers;
//   - only a NEWER version is offered: GitHub's "latest" excludes pre-releases
//     and drafts, and isNewerVersion applies the SemVer order on top (a
//     pre-release is older than its own final version, so a beta can never be
//     offered as a stable update).
// ---------------------------------------------------------------------------
void MainWindow::setupUpdater()
{
    connect(m_settingsPage, &SettingsPage::checkUpdatesRequested, this,
            [this]() { checkForUpdates(true); });

    // A video that STOPS releases the pending update: doing it from the
    // handler of stateChanged would run the dialog's event loop inside the
    // player's own signal, so it is deferred one turn of the loop.
    connect(m_playerWidget, &PlayerWidget::stateChanged, this, [this]() {
        if (m_updateDialogPending)
            QTimer::singleShot(0, this, [this]() { showUpdateDialog(true); });
    });

    QTimer::singleShot(20000, this, [this]() {
        const qint64 last =
            m_settings.getInt(QStringLiteral("update_last_check"), 0);
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        if (last > 0 && now - last < 24 * 3600)
            return; // it was checked recently: no need to ask again
        checkForUpdates(false);
    });
}

void MainWindow::checkForUpdates(bool manual)
{
    if (!m_updateChecker) {
        m_updateChecker = new nt::UpdateChecker(this);
        connect(m_updateChecker, &nt::UpdateChecker::finished, this,
                [this](const nt::UpdateRelease &release) {
                    onUpdateCheckFinished(release);
                });
        connect(m_updateChecker, &nt::UpdateChecker::failed, this,
                [this](const QString &message) {
                    onUpdateCheckFailed(message);
                });
    }
    if (m_updateChecker->isRunning())
        return;
    m_updateCheckManual = manual;
    // The timestamp is written when the query STARTS: a machine that is
    // offline every time must not retry in a loop on every start.
    m_settings.set(QStringLiteral("update_last_check"),
                   QString::number(QDateTime::currentSecsSinceEpoch()));
    m_updateChecker->check();
}

void MainWindow::onUpdateCheckFinished(const nt::UpdateRelease &release)
{
    m_updateRelease = release;
    const bool newer = release.found && !release.prerelease && !release.draft
        && nt::isNewerVersion(release.version, nt::appVersion());

    if (m_updateCheckManual) {
        // The manual check always answers, and it does it now: the person
        // pressed the button and is looking at the screen.
        m_updateDialogPending = true;
        showUpdateDialog(false);
        return;
    }
    if (!newer)
        return; // silent: nobody asked
    if (m_settings.getString(QStringLiteral("update_skip_version"))
        == release.version)
        return; // postponed for this version
    m_updateDialogPending = true;
    showUpdateDialog(true);
}

void MainWindow::showUpdateDialog(bool waitForIdle)
{
    if (!m_updateDialogPending)
        return;
    // Never on top of a video: the update waits (the pending flag keeps it
    // alive and the player's stateChanged brings it back).
    if (waitForIdle && m_playerWidget
        && m_playerWidget->state() == QLatin1String("playing"))
        return;
    m_updateDialogPending = false;

    nt::UpdateDialog dialog(m_updateRelease, nt::appVersion(), this);
    connect(&dialog, &nt::UpdateDialog::postponed, this,
            [this](const QString &version) {
                m_settings.set(QStringLiteral("update_skip_version"), version);
            });
    connect(&dialog, &nt::UpdateDialog::installRequested, this,
            &MainWindow::onUpdateInstallRequested);
    dialog.exec();
}

void MainWindow::onUpdateCheckFailed(const QString &message)
{
    // The automatic check is silent: an update that cannot be checked is not
    // an error worth interrupting anybody with. The manual one says what
    // happened (no network, rate limit, ...).
    if (!m_updateCheckManual)
        return;
    nt::confirm(this, nt::tr("update.title"), message, nt::tr("common.close"),
                []() {});
}

void MainWindow::onUpdateInstallRequested(const QString &installerPath)
{
    // Order: stop the player, launch the installer, leave. Nothing is
    // force-killed and nothing is deleted:
    //   - the database writes are immediate (settings, history, subscriptions
    //     and the state of every download live in SQLite, outside {app});
    //   - a download in progress keeps its row in "downloading"/"muxing" and
    //     resumePendingDownloads() revives it at the next start, with the
    //     fragments already on disk;
    //   - the installer only rewrites {app} (and removes an old app-local
    //     UCRT): the personal data and the downloaded files are elsewhere, and
    //     the [UninstallDelete] of the .iss only runs on an uninstall.
    if (m_playerWidget)
        m_playerWidget->stop();
    // The installer is the one that asks Windows for administrator rights
    // when it needs them (/NORESTART: the restarts are not ours to trigger)
    // and it has the "run NeverTube" option on its last page, so the app
    // comes back with one click.
    QProcess::startDetached(installerPath,
                            QStringList() << QStringLiteral("/NORESTART"),
                            QDir::tempPath());
    QTimer::singleShot(0, this, []() { qApp->quit(); });
}
