#include "watch_page.h"

#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include <functional>

#include "../../db/repositories.h"
#include "../../debug.h"
#include "../../i18n.h"
#include "../../models.h"
#include "../../player_controller.h"
#include "../../playerwidget.h"
#include "../../yt.h"
#include "../format.h"
#include "../icons.h"
#include "../player_glyphs.h"
#include "../confirm_dialog.h"
#include "../flow_layout.h"
#include "../row_menu.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

// Player menu resolutions, from lowest to highest. SINGLE source of
// truth: the menu indices (m_qualityItems), idxToQuality,
// qualityIndexForHeight and the height requested from Yt come from here. It
// must match the quality options of Settings (settings_page.cpp).
namespace {
const int kQualityHeights[] = {144, 240, 360, 480, 720, 1080, 1440, 2160};
constexpr int kQualityCount =
    static_cast<int>(sizeof(kQualityHeights) / sizeof(kQualityHeights[0]));

// Label for a resolution in the gear menu: appends "HD" to 720p and
// 1080p (and above) and "4K" to 2160p, like YouTube. Single source: used by the
// resolution menu.
QString qualityLabel(int height)
{
    if (height >= 2160)
        return QStringLiteral("%1p 4K").arg(height);
    if (height >= 720)
        return QStringLiteral("%1p HD").arg(height);
    return QStringLiteral("%1p").arg(height);
}
// Maximum height of the video area (PlayerContainer). On the page with a
// single scroll, an uncapped video would take width/aspect (768+ px in large
// and maximized windows) and description + "A continuación" would be pushed out
// of view. The cap bites when the aspect cap exceeds 560 px: in small
// windows width/aspect rules and this changes nothing. Pure layout: it does not
// touch render or repaint.
constexpr int kMaxPlayerHeight = 560;

// Video widths below which the control row trims
// elements, from least to most priority (like YouTube: first the volume
// slider, then the time labels). The sums come from the code, not by eye
// (the iconBtn buttons measure icon + 12 QSS padding; the clocks ~30 px
// at 12 px; the separators 1+12 px):
//   transport (5 buttons: shuffle/prev/play/next/repeat)     162
//   elapsed+duration clocks                                  ~60  (< kCtrlWithTimeMinW)
//   separators (2)                                           ~26
//   volume (mute 30 + fixed slider 80)                       110  (< kCtrlWithVolMinW)
//   actions (4 buttons: captions/playlist/gear/fs)           120
//   side margins of barLay                                   24
//   gaps (6 of them)                                         30-84
// The progress slider is the only flexible one (stretch) and takes the rest; the
// thresholds leave slack so as not to trim buttons (the setFixedWidth of the
// volume slider do not yield and the trim would fall on the icons and the clock).
constexpr int kCtrlWithVolMinW = 760;
constexpr int kCtrlWithTimeMinW = 640;

// Video area inside the overlay. gl1 (the layout that carries the box) used
// contentsMargins 0, so the box took the whole width with no separator. This
// class paints nothing: it only reports that width, which is what decides whether
// the row fits. No Q_OBJECT on purpose so as not to put a class with signals in
// the anonymous namespace: a std::function assigned on the fly.
class OverlayHost : public QWidget
{
public:
    explicit OverlayHost(QWidget *parent = nullptr) : QWidget(parent) {}
    std::function<void(int)> onWidth;

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        if (onWidth)
            onWidth(width());
    }
};
} // namespace

class PlayerContainer : public QWidget
{
public:
    PlayerContainer(PlayerWidget *player, QWidget *overlay, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_overlay(overlay)
        , m_player(player)
    {
        // The background comes from the global QSS (QWidget#playerArea): pure
        // black, not kBg, because it is the letterbox bands' background and it
        // must be truly black so the frame is not noticeable. It used to be an inline setStyleSheet.
        player->setParent(this);
        overlay->setParent(player);
        overlay->setGeometry(player->rect());
        overlay->raise();
        overlay->show();

        m_lay = new QVBoxLayout(this);
        m_lay->setContentsMargins(0, 0, 0, 0);
        m_lay->setSpacing(0);
        m_lay->addWidget(player);

        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumHeight(0);
        updateMaxHeight();
    }

    // Returns the PlayerWidget to this area after the mini player (it reparents
    // it and puts it back in the layout; the GL context is preserved because it
    // is the same window). The overlay stays a child of the player.
    void givePlayerBack()
    {
        m_player->setParent(this);
        if (m_lay->indexOf(m_player) < 0)
            m_lay->addWidget(m_player);
        m_player->show();
        m_overlay->setGeometry(m_player->rect());
        m_overlay->raise();
        updateMaxHeight();
    }

    QSize sizeHint() const override
    {
        if (m_aspect > 0.0)
            return QSize(320, qRound(320.0 / m_aspect));
        return QSize(320, 180);
    }

    void setVideoAspect(double aspect)
    {
        if (aspect <= 0.0)
            return;
        m_aspect = aspect;
        updateMaxHeight();
    }

    // Video-only fullscreen: the absolute height cap does NOT apply (or
    // the video would stay at 560 px with the screen full of black); only the
    // aspect cap rules, which was the original behavior.
    void setFullscreen(bool fullscreen)
    {
        if (m_fullscreen == fullscreen)
            return;
        m_fullscreen = fullscreen;
        updateMaxHeight();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        m_overlay->setGeometry(m_player->rect());
        m_overlay->raise();
        updateMaxHeight();
        NT_DEBUG("E2E pc resize %dx%d\n", width(), height());
    }

private:
    void updateMaxHeight()
    {
        if (m_aspect <= 0.0)
            return;
        // The video keeps its ratio and also never exceeds the absolute
        // cap: on large/maximized screens the stretch (3:1) would make the
        // area grow to 3/4 of the window and description + "A continuación"
        // would be left with no space. In small windows the aspect cap
        // (width/aspect) is what rules and this cap changes nothing.
        // In FULLSCREEN the cap does not apply: the video must fill the
        // screen (only its aspect limits it).
        const int h = qRound(width() / m_aspect);
        const int capped = m_fullscreen ? h : qMin(h, kMaxPlayerHeight);
        NT_DEBUG("E2E pc aspect=%.3f width=%d maxH=%d curMin=%d curMax=%d\n",
                 m_aspect, width(), capped, minimumHeight(), maximumHeight());
        if (capped == minimumHeight() && capped == maximumHeight())
            return;
        // min = max = cap: inside the page's single scroll the layout must
        // give the video EXACTLY its height (aspect or 560 px), neither stretched
        // by Expanding policies nor shrunk by the scrollable content.
        setMinimumHeight(capped);
        setMaximumHeight(capped);
    }

    QWidget *m_overlay;
    PlayerWidget *m_player;
    QVBoxLayout *m_lay = nullptr;
    double m_aspect = 0.0;
    // Fullscreen (video only): disables the kMaxPlayerHeight cap.
    bool m_fullscreen = false;
};

// --- Overlay sliders (seek and volume), hand-painted -------------
// QSlider with Qt 5.15 QSS does NOT place the handle well: its center sits ~half
// a width to the LEFT of the real point and the sub-page (the fill) anchors to
// the WIDGET edge, not the track edge. Result at minimum value: the circle
// goes out past the left of the track and a red residue peeks out beside it (the "bit
// of red" at 0:00). Margins do not fix it (the handle offset and the
// fill anchor are coupled); that is why it is painted here, with the SAME
// look as the previous QSS: track #2F2F2F with a 1 px translucent border,
// accent fill and a 12 px white circle. Keys to the painting: the circle's
// center maps between radius and width-radius (it ALWAYS stays fully inside the
// widget) and the fill ends at the LEFT edge of the circle, so at
// 0:00 it measures zero and leaves no red in sight.
class OverlaySlider : public QSlider
{
public:
    explicit OverlaySlider(Qt::Orientation orientation, QWidget *parent = nullptr)
        : QSlider(orientation, parent)
    {
        setMinimumHeight(18);
        setMouseTracking(true);
    }

protected:
    static constexpr qreal kRadius = 6.0; // 12 px circle
    static constexpr qreal kTrackH = 6.0; // track fill

    // X center of the circle for a value: between [radius, width - radius]. Pure
    // function (only depends on the width and the range), shared by the painting and
    // the hover.
    static qreal handleCenterX(int widgetWidth, int value, int vmin, int vmax)
    {
        const int span = vmax - vmin;
        const qreal ratio = span > 0 ? qreal(value - vmin) / qreal(span) : 0.0;
        return kRadius
               + ratio * qMax(qreal(0.0), qreal(widgetWidth) - 2.0 * kRadius);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);

        const qreal w = qreal(width());
        const qreal cy = qreal(height()) / 2.0;

        // Track: box of 8 px (6 fill + 1 border per side), radius 3,
        // like the previous #ovSeek/#ovVolume QSS.
        const qreal boxW = qMax(1.0, w - 1.0);
        p.setBrush(QColor(255, 255, 255, 71)); // @OVERLAY_EDGE@
        p.drawRoundedRect(QRectF(0.5, cy - 4.0, boxW, 8.0), 3.0, 3.0);
        const QRectF track(1.5, cy - kTrackH / 2.0, qMax(1.0, boxW - 2.0), kTrackH);
        p.setBrush(nt::kHandle);
        p.drawRoundedRect(track, 2.0, 2.0);

        // Fill: from the start of the track to the LEFT edge of the circle (at
        // 0:00, width zero). Discreet when the slider is disabled.
        const qreal cx = handleCenterX(width(), value(), minimum(), maximum());
        const qreal fillEnd = cx - kRadius;
        if (fillEnd > track.left() + 0.5) {
            p.setBrush(isEnabled() ? nt::kAccent : nt::kSelection);
            p.drawRoundedRect(QRectF(track.left(), cy - kTrackH / 2.0,
                                     fillEnd - track.left(), kTrackH),
                              2.0, 2.0);
        }

        // Circle: white; accent with the cursor over it or while dragging.
        p.setBrush(!isEnabled() ? nt::kDisabled
                  : (m_handleHot || isSliderDown()) ? nt::kAccent
                                                    : QColor(0xFF, 0xFF, 0xFF));
        p.drawEllipse(QPointF(cx, cy), kRadius, kRadius);
    }

    void enterEvent(QEvent *event) override
    {
        updateHandleHover(mapFromGlobal(QCursor::pos()));
        QSlider::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        if (m_handleHot) {
            m_handleHot = false;
            update();
        }
        QSlider::leaveEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        updateHandleHover(event->pos());
        QSlider::mouseMoveEvent(event);
    }

private:
    // The hover only turns on with the cursor OVER the circle (like the
    // `::handle:hover` of the QSS it replaces), not anywhere on the widget.
    void updateHandleHover(const QPoint &pos)
    {
        const qreal cx = handleCenterX(width(), value(), minimum(), maximum());
        const qreal m = kRadius + 2.0;
        const QRectF hr(cx - m, qreal(height()) / 2.0 - m, m * 2.0, m * 2.0);
        const bool hot = hr.contains(QPointF(pos));
        if (hot != m_handleHot) {
            m_handleHot = hot;
            update();
        }
    }

    bool m_handleHot = false;
};

// Seek slider: a click anywhere on the track seeks there and
// dragging does not fight the position updates while it is held
// down. It emits the sought value (seek trigger) on every change.
class SeekSlider : public OverlaySlider
{
public:
    explicit SeekSlider(Qt::Orientation orientation, QWidget *parent = nullptr)
        : OverlaySlider(orientation, parent)
    {
        setTracking(true);
    }

    bool isSeeking() const { return m_dragging; }

    // Live ("EN VIVO"): the bar is informative (playhead at the edge, the
    // live's "now"); a click must not seek to a meaningless value.
    void setLive(bool live) { m_live = live; }

protected:
    static int valueFromPos(const QSlider *slider, int x)
    {
        // Same mapping as the painting (circle center between radius and
        // width-radius) so the value falls right under the cursor.
        const qreal w = qMax(qreal(1.0), qreal(slider->width()));
        const qreal r = qBound(qreal(0.0),
                               (qreal(x) - kRadius) / qMax(qreal(1.0), w - 2.0 * kRadius),
                               qreal(1.0));
        return slider->minimum()
               + qRound(qreal(slider->maximum() - slider->minimum()) * r);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (m_live) {
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton) {
            m_dragging = true;
            const int v = valueFromPos(this, event->pos().x());
            setValue(v);
            emit sliderMoved(v);
            event->accept();
            return;
        }
        QSlider::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragging) {
            const int v = valueFromPos(this, event->pos().x());
            setValue(v);
            emit sliderMoved(v);
            event->accept();
            return;
        }
        // Falls through to the base so the circle hover keeps working.
        OverlaySlider::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_dragging) {
            m_dragging = false;
            const int v = valueFromPos(this, event->pos().x());
            setValue(v);
            emit sliderMoved(v);
            event->accept();
            return;
        }
        QSlider::mouseReleaseEvent(event);
    }

private:
    bool m_dragging = false;
    bool m_live = false;
};

namespace {

// Avatar cropped into a circle for the player's channel row.
// The circular crop of the avatar is nt::circularAvatar (src/ui/icons.*), the
// same one used by the Channel sheet and the Library/Search card. This copy
// also stretched the pixmap to a square (drawing a "KeepAspectRatioByExpanding"
// scaled image into a size x size rect), so a non-square avatar
// came out deformed; the shared one crops the center.

// Channel icon with gap px of separation to the right of the circle, so
// that the name is not glued to the avatar.
QIcon avatarWithGap(const QPixmap &circular, int size, int gap)
{
    QPixmap canvas(size + gap, size);
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    p.drawPixmap(0, 0, size, size, circular);
    return QIcon(canvas);
}

// Enables mouse tracking + event filter on a widget and all its children,
// so that WatchPage can show/hide the floating controls.
void enableOverlayTracking(QObject *filterObj, QWidget *root)
{
    if (!root)
        return;
    root->setMouseTracking(true);
    root->installEventFilter(filterObj);
    const auto children = root->findChildren<QWidget *>();
    for (QWidget *w : children) {
        w->setMouseTracking(true);
        w->installEventFilter(filterObj);
    }
}

} // namespace

WatchPage::WatchPage(Yt *yt, PlayerController *player, PlayerWidget *playerWidget,
                     VideoCacheRepo *cache, FavoritesRepo *favorites,
                     StarredRepo *starred, WatchLaterRepo *watchLater,
                     SubscriptionsRepo *subs, SettingsRepo *settings,
                     ThumbnailCache *thumbs, PlaylistsRepo *playlists,
                     QWidget *parent)
    : QWidget(parent)
    , m_yt(yt)
    , m_player(player)
    , m_playerWidget(playerWidget)
    , m_cache(cache)
    , m_favs(favorites)
    , m_starred(starred)
    , m_watchLater(watchLater)
    , m_subs(subs)
    , m_settings(settings)
    , m_thumbs(thumbs)
    , m_playlists(playlists)
{
    // Auto mode: read the saved quality preference (default auto at 360p)
    const QString savedQ = m_settings
                               ? m_settings->getString(QStringLiteral("quality"),
                                                       QStringLiteral("auto"))
                               : QStringLiteral("auto");
    if (savedQ == QStringLiteral("auto")) {
        m_autoQuality = true;
        m_autoHeight = m_settings
                           ? m_settings->getInt(QStringLiteral("quality_auto"), 360)
                           : 360;
    } else {
        m_autoQuality = false;
        int h = savedQ.toInt();
        m_autoHeight = (h > 0) ? h : 360;
    }
    // The menu button/index start at the quality actually requested (Auto =
    // the last memorized one), not at a fixed 720p that deceives.
    m_qualityIndex = qualityIndexForHeight(m_autoHeight);

    auto *splitOuter = new QVBoxLayout(this);
    splitOuter->setContentsMargins(0, 0, 0, 0);
    splitOuter->setSpacing(0);

    m_overlay = new QStackedWidget(this);
    m_overlay->setAttribute(Qt::WA_TranslucentBackground, true);
    // The two "background: transparent" here (the stack and OverlayHost) are NOT
    // theme inlines pending migration: they are structural. The stack has to
    // let the video behind it show through, and OverlayHost is a flat QWidget that
    // only exists to place the control box on top of the video. Moving them
    // to the global QSS does not change a single pixel (they are already "colorless"), and
    // if the selector failed the stack would paint its palette background on top of the video again.
    m_overlay->setStyleSheet(QStringLiteral("background: transparent;"));

    auto *gl = new OverlayHost;
    gl->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *gl1 = new QVBoxLayout(gl);
    // 12 px separator: used to be 0,0,0,0. m_ctrlBar receives no geometry (only
    // gl1 places it), so the box touched the three edges of the video and its
    // border-radius and border delimited nothing: they looked like ornaments of a
    // band stuck to the edge. With the separator, the box floats and its shape says
    // what it says. Top 0 on purpose: the box rests on the bottom edge.
    gl1->setContentsMargins(12, 0, 12, 12);
    gl1->setSpacing(0);
    gl1->addStretch(1);

    m_ctrlBar = new QWidget(gl);
    m_ctrlBar->setObjectName(QStringLiteral("playerOverlay"));
    m_ctrlBar->setAttribute(Qt::WA_StyledBackground, true);
    // The style (translucent background, two-tone border, radius 12) lives in the
    // global QSS: it used to be an inline setStyleSheet with #2A2A2A, which also
    // contradicted kBorder (#262626), and that is why this surface was the only one that
    // did not cover an accent change.
    auto *barLay = new QVBoxLayout(m_ctrlBar);
    // Before (10, 4, 10, 6) with spacing 2: 4 px of air above the track and 2
    // between rows, versus 10 px on the sides. With the box floating that imbalance
    // shows. 8/8/6 is what the rest of the app uses. The box grows ~10 px.
    barLay->setContentsMargins(12, 8, 12, 8);
    barLay->setSpacing(6);

    // SINGLE row of controls (YouTube style): the TRANSPORT starts the row
    // with play/pause (like YouTube), then previous and next, and
    // finally shuffle and repeat; elapsed time + progress slider +
    // duration in the center, and volume/actions on the right. All the
    // controls are #iconBtn (transparent, only fill on hover): no
    // frames, no button boxes, no circular play. The icons are those of
    // nt::playerIcon (Material Symbols drawn with QPainter, vector).
    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);

    auto makeIconBtn = [this](QWidget *parent) {
        auto *b = new QPushButton(parent);
        b->setObjectName(QStringLiteral("iconBtn"));
        b->setCursor(Qt::PointingHandCursor);
        return b;
    };

    m_ovPlayBtn = makeIconBtn(m_ctrlBar);
    nt::bindTooltip(m_ovPlayBtn, "player.playPause");
    connect(m_ovPlayBtn, &QPushButton::clicked, this,
            &WatchPage::onPlayPause);
    btnRow->addWidget(m_ovPlayBtn);

    m_ovPrevBtn = makeIconBtn(m_ctrlBar);
    nt::bindTooltip(m_ovPrevBtn, "player.prev");
    // YouTube's behavior lives in PlayerController::previous() (and the
    // button's enabled state in previousAvailable()): the logic is not the UI's.
    connect(m_ovPrevBtn, &QPushButton::clicked, this, [this]() {
        m_player->previous();
    });
    btnRow->addWidget(m_ovPrevBtn);

    m_ovNextBtn = makeIconBtn(m_ctrlBar);
    nt::bindTooltip(m_ovNextBtn, "player.next");
    connect(m_ovNextBtn, &QPushButton::clicked, this, [this]() {
        if (m_player->hasNext())
            m_player->next();
    });
    btnRow->addWidget(m_ovNextBtn);

    m_ovShuffleBtn = makeIconBtn(m_ctrlBar);
    nt::bindTooltip(m_ovShuffleBtn, "player.shuffle");
    connect(m_ovShuffleBtn, &QPushButton::clicked, this, [this]() {
        m_player->setShuffle(!m_player->shuffle());
        updateOverlayIcons();
    });
    btnRow->addWidget(m_ovShuffleBtn);

    m_ovRepeatBtn = makeIconBtn(m_ctrlBar);
    nt::bindTooltip(m_ovRepeatBtn, "player.repeat");
    connect(m_ovRepeatBtn, &QPushButton::clicked, this, [this]() {
        m_player->setRepeat(!m_player->repeat());
        updateOverlayIcons();
    });
    btnRow->addWidget(m_ovRepeatBtn);

    // Elapsed time, progress and duration in the center: the slider takes
    // all the flexible gap (stretch) between the two labels.
    m_ovPos = new QLabel(QStringLiteral("0:00"), m_ctrlBar);
    m_ovPos->setObjectName(QStringLiteral("ovPos"));
    btnRow->addWidget(m_ovPos);

    m_ovSeek = new SeekSlider(Qt::Horizontal, m_ctrlBar);
    m_ovSeek->setObjectName(QStringLiteral("ovSeek"));
    m_ovSeek->setCursor(Qt::PointingHandCursor);
    m_ovSeek->setMinimumWidth(60);
    connect(m_ovSeek, &QSlider::sliderMoved, this,
            [this](int value) { m_player->seek(value); });
    btnRow->addWidget(m_ovSeek, 1);

    m_ovDur = new QLabel(QStringLiteral("0:00"), m_ctrlBar);
    m_ovDur->setObjectName(QStringLiteral("ovDur"));
    btnRow->addWidget(m_ovDur);

    // Subtle vertical separators (1 px) between the subgroups of the row.
    auto makeSep = [this](QWidget *parent) {
        auto *sep = new QFrame(parent);
        sep->setObjectName(QStringLiteral("ovSep"));
        sep->setFixedSize(1, 20);
        return sep;
    };
    btnRow->addWidget(makeSep(m_ctrlBar));

    m_ovMuteBtn = makeIconBtn(m_ctrlBar);
    nt::bindTooltip(m_ovMuteBtn, "player.mute");
    connect(m_ovMuteBtn, &QPushButton::clicked, this,
            &WatchPage::onToggleMute);
    btnRow->addWidget(m_ovMuteBtn);

    m_ovVolume = new OverlaySlider(Qt::Horizontal, m_ctrlBar);
    m_ovVolume->setObjectName(QStringLiteral("ovVolume"));
    m_ovVolume->setRange(0, m_player->volumeMax());
    m_ovVolume->setValue(int(m_player->volume()));
    m_ovVolume->setFixedWidth(80);
    m_ovVolume->setCursor(Qt::PointingHandCursor);
    connect(m_ovVolume, &QSlider::valueChanged, this, [this](int v) {
        m_player->setVolume(v);
        // The cap at 100% on persistence is in MainWindow::persistVolume,
        // called by other paths. Here, if v>100 (boost active) it is not persisted
        // to prevent a high volume from surviving the app restart.
        if (m_settings && v <= 100)
            m_settings->set(QStringLiteral("volume"), QString::number(v));
    });
    btnRow->addWidget(m_ovVolume);

    btnRow->addWidget(makeSep(m_ctrlBar));

    m_ovSubsBtn = makeIconBtn(m_ctrlBar);
    nt::bindTooltip(m_ovSubsBtn, "player.subtitles");
    connect(m_ovSubsBtn, &QPushButton::clicked, this, [this]() {
        m_subsVisible = !m_subsVisible;
        if (m_playerWidget)
            m_playerWidget->setSubVisibility(m_subsVisible);
        updateOverlayIcons();
    });
    btnRow->addWidget(m_ovSubsBtn);

    m_ovPlaylistBtn = makeIconBtn(m_ctrlBar);
    nt::bindTooltip(m_ovPlaylistBtn, "player.addToPlaylist");
    connect(m_ovPlaylistBtn, &QPushButton::clicked, this,
            &WatchPage::showPlaylistMenu);
    btnRow->addWidget(m_ovPlaylistBtn);

    // The resolution menu is anchored to this button, which stopped being the text
    // chip "720p" to be the settings icon of the row. The active resolution
    // now lives in the tooltip (see onVideoSizeChanged/setQualityIndex).
    m_quality = makeIconBtn(m_ctrlBar);
    // NOT bound: onVideoSizeChanged overwrites the tooltip with the active
    // resolution. The initial text is in the active language.
    m_quality->setToolTip(nt::tr("player.quality"));
    connect(m_quality, &QPushButton::clicked, this,
            &WatchPage::toggleQualityMenu);
    btnRow->addWidget(m_quality);

    m_ovFsBtn = makeIconBtn(m_ctrlBar);
    nt::bindTooltip(m_ovFsBtn, "player.fullscreen");
    connect(m_ovFsBtn, &QPushButton::clicked, this,
            &WatchPage::toggleFullscreen);
    btnRow->addWidget(m_ovFsBtn);

    // Base icons of the row (nt::playerIcon). The ones that depend on the state
    // (play/pause, shuffle, repeat, captions) are refreshed by
    // updateOverlayIcons().
    m_ovShuffleBtn->setIcon(
        nt::playerIcon(nt::PlayerIcon::Shuffle, nt::kText, 20));
    m_ovPrevBtn->setIcon(nt::playerIcon(nt::PlayerIcon::Prev, nt::kText, 20));
    m_ovPlayBtn->setIcon(nt::playerIcon(nt::PlayerIcon::Play, nt::kText, 22));
    m_ovNextBtn->setIcon(nt::playerIcon(nt::PlayerIcon::Next, nt::kText, 20));
    m_ovRepeatBtn->setIcon(
        nt::playerIcon(nt::PlayerIcon::Repeat, nt::kText, 20));
    m_ovMuteBtn->setIcon(nt::playerIcon(nt::PlayerIcon::Volume, nt::kText, 20));
    m_ovSubsBtn->setIcon(
        nt::playerIcon(nt::PlayerIcon::Captions, nt::kText, 20));
    m_ovPlaylistBtn->setIcon(
        nt::playerIcon(nt::PlayerIcon::PlaylistAdd, nt::kText, 20));
    m_quality->setIcon(nt::playerIcon(nt::PlayerIcon::Settings, nt::kText, 20));
    m_ovFsBtn->setIcon(
        nt::playerIcon(nt::PlayerIcon::Fullscreen, nt::kText, 20));

    barLay->addLayout(btnRow);
    gl1->addWidget(m_ctrlBar);
    m_ctrlBar->hide();

    // Responsive logic: if the video width does not fit the full row, the
    // volume slider is hidden first (mute stays) and then the
    // time labels. It is connected to the widget that knows the real width
    // (OverlayHost), not the parent.
    gl->onWidth = [this](int w) {
        if (!m_ovVolume)
            return;
        if (w <= 0) {
            if (m_ovVolume->isHidden())
                m_ovVolume->setVisible(true);
            if (m_ovPos->isHidden()) {
                m_ovPos->setVisible(true);
                m_ovDur->setVisible(true);
            }
            return;
        }
        const bool showVol = (w >= kCtrlWithVolMinW);
        const bool showTime = (w >= kCtrlWithTimeMinW);
        // isHidden() and NOT isVisible(): the box is hidden almost all the time,
        // and isVisible() returns false as soon as the parent is hidden, so
        // with isVisible() the guard would never be met (nothing would ever be
        // hidden) and a relayout would be forced on every resize.
        if (m_ovVolume->isHidden() == showVol)
            m_ovVolume->setVisible(showVol);
        if (m_ovPos->isHidden() == showTime) {
            m_ovPos->setVisible(showTime);
            m_ovDur->setVisible(showTime);
        }
    };

    m_qualityMenu = new QFrame(gl);
    m_qualityMenu->setObjectName(QStringLiteral("qualityMenu"));
    m_qualityMenu->setAttribute(Qt::WA_StyledBackground, true);
    // The style lives in the global QSS (QFrame#qualityMenu): here it was an
    // inline setStyleSheet that repeated #1F1F1F and #2D2D2D, which are already tokens.
    auto *menuLay = new QVBoxLayout(m_qualityMenu);
    menuLay->setContentsMargins(4, 4, 4, 4);
    menuLay->setSpacing(1);
    // In the soft-all profile (CPU decoding, no HW) it makes no sense
    // to offer 1440p/2160p: the menu hides them so the user does not pick a
    // resolution a pure CPU cannot handle. The indices stay
    // aligned with idxToQuality(): they are only hidden, not removed.
    const bool softAll = m_settings
        && m_settings->getString(QStringLiteral("codec_profile"),
                                 QStringLiteral("av1-hw"))
               == QStringLiteral("soft-all");
    m_qualityItems.reserve(kQualityCount);
    for (int i = 0; i < kQualityCount; ++i) {
        auto *item =
            new QPushButton(qualityLabel(kQualityHeights[i]), m_qualityMenu);
        item->setFixedWidth(90);
        // soft-all: CPU only, 1440p/2160p are not offered. Each item is
        // stored in m_qualityItems to hide it on the fly if mpv
        // reports a real lower ceiling (see applyQualityLimits).
        item->setVisible(!(softAll && kQualityHeights[i] >= 1440));
        const int idx = i;
        connect(item, &QPushButton::clicked, this, [this, idx]() {
            setQualityIndex(idx);
            m_qualityMenu->hide();
            m_qualityMenuActive = false;
        });
        menuLay->addWidget(item);
        m_qualityItems.append(item);
    }
    m_qualityMenu->adjustSize();
    m_qualityMenu->hide();

    m_qualityMenuAnchor = gl;

    m_overlay->addWidget(gl);
    auto *spinner = new QLabel(nt::tr("common.loading"), this);
    spinner->setAlignment(Qt::AlignCenter);
    spinner->setObjectName(QStringLiteral("ovSpinner"));
    m_overlay->addWidget(spinner);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setAlignment(Qt::AlignCenter);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setObjectName(QStringLiteral("ovError"));
    m_overlay->addWidget(m_errorLabel);
    m_overlay->setCurrentIndex(0);
    enableOverlayTracking(this, gl);

    // Hide the floating controls after mouse inactivity.
    m_overlayTimer = new QTimer(this);
    m_overlayTimer->setSingleShot(true);
    m_overlayTimer->setInterval(2500);
    connect(m_overlayTimer, &QTimer::timeout, this,
            &WatchPage::hideOverlayControls);

    auto *pc = new PlayerContainer(playerWidget, m_overlay, this);
    m_playerContainer = pc;
    pc->setObjectName(QStringLiteral("playerArea"));
    // PlayerContainer is a flat QWidget, not a QFrame: Qt only paints the background
    // the QSS gives it if the widget carries WA_StyledBackground. With the background in
    // the global stylesheet (instead of its own setStyleSheet) it has to be said
    // explicitly, or the letterbox bands stay in kBg (#0F0F0F) instead of
    // black and the frame around the video shows.
    pc->setAttribute(Qt::WA_StyledBackground, true);
    pc->ensurePolished();
    enableOverlayTracking(this, pc);
    connect(playerWidget, &PlayerWidget::videoAspectChanged, pc,
            &PlayerContainer::setVideoAspect);
    // No button-like focus frame when clicking the video.
    pc->setFocusPolicy(Qt::NoFocus);
    playerWidget->setFocusPolicy(Qt::NoFocus);
    m_overlay->setFocusPolicy(Qt::NoFocus);
    gl->setFocusPolicy(Qt::NoFocus);

    // SINGLE scroll of the whole page: video + description + "A continuación"
    // stacked and affected by the SAME scroll (YouTube style). On scrolling down, the
    // video leaves the screen and description/queue gain space. The height
    // of the video is set by PlayerContainer::updateMaxHeight (aspect cap
    // up to 560 px) inside the scrollable content.
    auto *topScroll = new QScrollArea(this);
    topScroll->setWidgetResizable(true);
    topScroll->setFrameShape(QFrame::NoFrame);
    topScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_topScroll = topScroll;
    auto *topWidget = new QWidget;
    auto *top = new QVBoxLayout(topWidget);
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(0);
    topScroll->setWidget(topWidget);
    splitOuter->addWidget(topScroll, 1);

    // The player lives INSIDE the scroll (at the top of the content): it moves
    // with the page.
    top->addWidget(pc);

    // Middle row INSIDE the scroll: description on the left and "A
    // continuación" on the right, with NO vertical limit on the queue (it grows with
    // all its rows and the general scroll goes through it, with no internal scroll).
    auto *midRow = new QWidget;
    auto *midLay = new QHBoxLayout(midRow);
    midLay->setContentsMargins(0, 0, 0, 0);
    midLay->setSpacing(0);

    auto *infoWidget = new QWidget;
    infoWidget->setObjectName(QStringLiteral("watchBody"));
    m_watchBody = infoWidget;
    auto *infoLay = new QVBoxLayout(infoWidget);
    infoLay->setContentsMargins(16, 14, 16, 8);
    infoLay->setSpacing(6);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("watchTitle"));
    m_title->setWordWrap(true);
    infoLay->addWidget(m_title);

    m_meta = new QLabel(this);
    m_meta->setObjectName(QStringLiteral("watchMeta"));
    infoLay->addWidget(m_meta);

    m_channelBtn = new QPushButton(nt::tr("channel.title"), this);
    m_channelBtn->setObjectName(QStringLiteral("channelBtn"));
    m_channelBtn->setCursor(Qt::PointingHandCursor);
    m_channelBtn->setIconSize(QSize(43, 40));
    m_channelBtn->setIcon(nt::icon(nt::Icon::User, nt::kMuted, 20));
    // On purpose it is NOT #ghost: the button has an icon on the left and text on the
    // right, aligned, and needs 8px padding instead of the 6px of #ghost.
    m_channelBtn->setObjectName(QStringLiteral("channelBtn"));
    m_channelBtn->setMinimumHeight(56);
    m_channelBtn->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);

    m_subscribeBtn = new QPushButton(nt::tr("channel.subscribe"), this);
    m_subscribeBtn->setObjectName(QStringLiteral("subscribe"));
    m_subscribeBtn->setCursor(Qt::PointingHandCursor);
    m_subscribeBtn->setMinimumHeight(36);
    // No stylesheet of its own: the initial state (not subscribed, with accent) is given
    // by the QPushButton#subscribe rule, and the hover is the light variant of
    // @ACCENT@, so it follows the chosen color without re-applying anything.

    auto *channelRow = new QHBoxLayout;
    channelRow->setSpacing(8);
    channelRow->addWidget(m_channelBtn);
    channelRow->addStretch(1);
    channelRow->addWidget(m_subscribeBtn, 0, Qt::AlignVCenter);
    infoLay->addLayout(channelRow);

    // Action row. Height 44 px (WCAG 2.5.5, recommended touch target;
    // 40 px fell short) and 10 px of separation between contiguous targets.
    // The segmented group grows to the same height so the row is not stepped.
    auto *actionsRow = new FlowLayout(0, 10, 10);

    // Segmented Like / Dislike group. Both are icon-only: the text
    // of the 6 chips freed half a row and the FlowLayout wrapped. The
    // tooltip and the accessible name keep the label.
    auto *seg = new QWidget(this);
    seg->setObjectName(QStringLiteral("actionSeg"));
    seg->setFixedHeight(44);
    auto *segLay = new QHBoxLayout(seg);
    segLay->setContentsMargins(0, 0, 0, 0);
    segLay->setSpacing(0);
    auto makeSeg = [&](const QString &objName, nt::Icon icon, const QString &tip,
                       QPushButton **out, nt::Icon *iconOut) {
        auto *b = new QPushButton(seg);
        b->setObjectName(objName);
        b->setCursor(Qt::PointingHandCursor);
        b->setIcon(nt::icon(icon, nt::kMuted, 20));
        b->setIconSize(QSize(20, 20));
        b->setFixedSize(52, 44);
        b->setToolTip(tip);
        b->setAccessibleName(tip);
        *out = b;
        *iconOut = icon;
    };
    makeSeg(QStringLiteral("actionSegL"), nt::Icon::Thumb,
            nt::tr("watch.like"), &m_likeBtn, &m_likeIcon);
    makeSeg(QStringLiteral("actionSegR"), nt::Icon::ThumbDown,
            nt::tr("watch.dislike"), &m_dislikeBtn, &m_dislikeIcon);
    segLay->addWidget(m_likeBtn);
    segLay->addWidget(m_dislikeBtn);
    actionsRow->addWidget(seg);

    // Rest of the actions: circular icon-only pill, with tooltip. They are
    // added EXPLICITLY to actionsRow: creating the button is not enough, without
    // addWidget it stays out of the layout and is not visible.
    auto makeAction = [this, actionsRow](nt::Icon icon, const QString &tip,
                             QPushButton **out, nt::Icon *iconOut) {
        auto *b = new QPushButton(this);
        b->setObjectName(QStringLiteral("actionChip"));
        b->setCursor(Qt::PointingHandCursor);
        b->setIcon(nt::icon(icon, nt::kMuted, 20));
        b->setIconSize(QSize(20, 20));
        b->setFixedSize(44, 44);
        b->setToolTip(tip);
        b->setAccessibleName(tip);
        actionsRow->addWidget(b);
        *out = b;
        *iconOut = icon;
    };
    makeAction(nt::Icon::Share, nt::tr("row.share"),
               &m_shareBtn, &m_shareIcon);
    makeAction(nt::Icon::Star, nt::tr("watch.favorite"),
               &m_starBtn, &m_starIcon);
    makeAction(nt::Icon::WatchLater, nt::tr("library.watchLater"),
               &m_watchLaterBtn, &m_watchLaterIcon);
    makeAction(nt::Icon::ListAdd, nt::tr("watch.addToPlaylist"),
               &m_playlistBtn, &m_playlistIcon);
    connect(m_playlistBtn, &QPushButton::clicked, this,
            &WatchPage::showPlaylistMenu);
    infoLay->addLayout(actionsRow);

    m_descLabel = new QLabel(this);
    m_descLabel->setWordWrap(true);
    m_descLabel->setTextFormat(Qt::RichText);
    m_descLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse
                                         | Qt::TextSelectableByMouse);
    m_descLabel->setOpenExternalLinks(true);
    m_descLabel->setObjectName(QStringLiteral("watchDesc"));
    // Description card: #181818 background with the COMPLETE text. There is no longer
    // a collapse to 3 lines or a "…más"/"…menos" control: the description shows
    // in full (the user asked for it).
    m_descCard = new QFrame(this);
    m_descCard->setObjectName(QStringLiteral("watchDescCard"));
    auto *descLay = new QVBoxLayout(m_descCard);
    descLay->setContentsMargins(12, 10, 12, 10);
    descLay->setSpacing(4);
    descLay->addWidget(m_descLabel);
    infoLay->addWidget(m_descCard);
    midLay->addWidget(infoWidget, 7);

    auto *bottomWidget = new QWidget;
    m_bottomWidget = bottomWidget;
    auto *bot = new QVBoxLayout(bottomWidget);
    bot->setContentsMargins(0, 8, 16, 8);
    bot->setSpacing(6);
    auto *quRow = new QHBoxLayout;
    quRow->setContentsMargins(0, 0, 0, 0);
    quRow->setSpacing(8);
    auto *quTitle = new QLabel(nt::tr("watch.upNext"), this);
    quTitle->setObjectName(QStringLiteral("queueTitle"));
    m_queueCount = new QLabel(this);
    m_queueCount->setObjectName(QStringLiteral("queueCount"));
    quRow->addWidget(quTitle);
    quRow->addStretch(1);
    quRow->addWidget(m_queueCount);
    m_nextUpList = new VideoListView(m_thumbs, this);
    m_nextUpList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_nextUpList->setSizePolicy(QSizePolicy::Expanding,
                                QSizePolicy::Expanding);
    m_nextUpList->setMinimumHeight(90);
    bot->addLayout(quRow);
    bot->addWidget(m_nextUpList);

    // "A continuación" to the right of the description, inside the general
    // scroll (with no vertical limit: updateQueue sets the height to the
    // real content).
    midLay->addWidget(bottomWidget, 3);
    // Both blocks keep their NATURAL height and stay top-aligned: the
    // row grows with the queue (no limit), but the description does not stretch to
    // that height — below each block there is only scrollable empty space.
    midLay->setAlignment(infoWidget, Qt::AlignTop);
    midLay->setAlignment(bottomWidget, Qt::AlignTop);
    top->addWidget(midRow);
    // The stretch absorbs the surplus when the content does not fill the window:
    // the blocks stay glued to the top, with a gap at the end (like YouTube).
    top->addStretch(1);

    // Player "Back": a floating link over the video that ONLY appears
    // with the mouse. It lives inside the overlay page (gl, transparent
    // OverlayHost), so it shares the same 2.5 s cycle as the controls
    // (showOverlayControls/hideOverlayControls), follows the mini player
    // reparent with nothing to do by hand (the overlay is a child of the player), and
    // does not need its own geometry on resize: in the top
    // left corner the position does not change.
    //
    // It used to take a fixed 38 px bar across the whole width ABOVE the scroll
    // (m_backBar with splitOuter->insertWidget(0, ...)): a 63 px link in
    // a band full of gap, which also pushed the video 38 px down.
    // The user asked for it outside. The whole bar disappears, and with it the 38 px
    // and the gap it left.
    //
    // The token is #backLink, the same discreet link as Channel, History and
    // Playlist; inside the overlay its top edge sits against the edge of the
    // video (gl1 does not manage this child, only m_ctrlBar and the stretch).
    m_back = new QPushButton(gl);
    m_back->setObjectName(QStringLiteral("backLink"));
    nt::bindText(m_back, "common.back");
    // The Back icon (below) is already the arrow: the "◀" in the text was a glyph
    // that does not exist in the default font and came out as a dot, and it also
    // duplicated the same affordance.
    m_back->setIcon(nt::icon(nt::Icon::Back, nt::kMuted, 13));
    m_back->setCursor(Qt::PointingHandCursor);
    m_back->move(12, 8);
    m_back->raise();
    m_back->hide(); // appears with the controls, not before

    connect(m_back, &QPushButton::clicked, this,
            &WatchPage::backRequested);
    connect(m_channelBtn, &QPushButton::clicked, this, [this]() {
        if (m_player->index() < 0)
            return;
        const VideoInfo v = m_player->current();
        if (!v.channelId.isEmpty())
            emit channelRequested(v.channelId, v.channel);
    });
    connect(m_subscribeBtn, &QPushButton::clicked, this, [this]() {
        if (m_player->index() < 0)
            return;
        const VideoInfo v = m_player->current();
        if (v.channelId.isEmpty())
            return;
        updateSubscription(v, !m_subs->isSubscribed(v.channelId));
    });
    connect(m_likeBtn, &QPushButton::clicked, this, [this]() {
        const VideoInfo v = m_player->current();
        if (v.id.isEmpty())
            return;
        m_favs->toggle(v.id);
        m_liked = m_favs->isFavorite(v.id);
        if (m_liked)
            m_disliked = false;
        updateActionButtons();
    });
    connect(m_starBtn, &QPushButton::clicked, this, [this]() {
        const VideoInfo v = m_player->current();
        if (v.id.isEmpty())
            return;
        m_starred->toggle(v.id);
        m_isStarred = m_starred->isStarred(v.id);
        updateActionButtons();
    });
    connect(m_watchLaterBtn, &QPushButton::clicked, this, [this]() {
        const VideoInfo v = m_player->current();
        if (v.id.isEmpty())
            return;
        m_watchLater->toggle(v.id);
        m_watchLaterMarked = m_watchLater->isWatchLater(v.id);
        updateActionButtons();
    });
    connect(m_dislikeBtn, &QPushButton::clicked, this, [this]() {
        const VideoInfo v = m_player->current();
        if (v.id.isEmpty())
            return;
        m_disliked = !m_disliked;
        if (m_disliked)
            m_liked = false;
        updateActionButtons();
    });
    connect(m_shareBtn, &QPushButton::clicked, this, [this]() {
        const VideoInfo v = m_player->current();
        if (v.id.isEmpty())
            return;
        const QString url = QStringLiteral(
            "https://www.youtube.com/watch?v=%1").arg(v.id);
        QGuiApplication::clipboard()->setText(url);
        QToolTip::showText(m_shareBtn->mapToGlobal(QPoint(0, 0)),
                           nt::tr("row.linkCopied"), m_shareBtn);
    });
    connect(m_player, &PlayerController::currentChanged, this,
            &WatchPage::onCurrentChanged);
    connect(m_player, &PlayerController::queueChanged, this,
            &WatchPage::onQueueChanged);
    connect(m_player, &PlayerController::phaseChanged, this,
            &WatchPage::onPhaseChanged);
    connect(m_player, &PlayerController::positionChanged, this,
            &WatchPage::onPositionChanged);
    connect(m_player, &PlayerController::volumeChanged, this,
            &WatchPage::onVolumeChanged);
    connect(playerWidget, &PlayerWidget::videoSizeChanged, this,
            &WatchPage::onVideoSizeChanged);
    // When mpv loads the file it emits the real maximum height of its
    // tracks; if it caps at 720p, we hide 1080p/1440p/2160p from the
    // resolution menu (mpv could not select those tracks and it felt like a
    // "dead click" for the user).
    connect(playerWidget, &PlayerWidget::qualityLimitsChanged, this,
            &WatchPage::applyQualityLimits);
    // Real video heights (Innertube): if Auto asked for 360 of a 1080 video,
    // the menu must still offer 1080 even if the loaded stream is 360.
    connect(m_yt, &Yt::streamHeightsReady, this, &WatchPage::onStreamHeights);
    connect(m_player, &PlayerController::durationChanged, this,
            &WatchPage::onDurationKnown);
    connect(m_yt, &Yt::detailReady, this, &WatchPage::onDetailReady);
    connect(m_yt, &Yt::detailFailed, this, &WatchPage::onDetailFailed);
    connect(m_yt, &Yt::relatedReady, this, &WatchPage::onRelatedReady);
    connect(m_yt, &Yt::radioReady, this, &WatchPage::onRadioReady);
    connect(m_yt, &Yt::currentDetailReady, this,
            &WatchPage::onCurrentDetailReady);
    connect(m_yt, &Yt::channelInfoReady, this,
            &WatchPage::onChannelInfo);
    connect(m_thumbs, &ThumbnailCache::ready, this,
            &WatchPage::onAvatarReady);
    connect(m_nextUpList, &VideoListView::videoActivated, this,
            &WatchPage::onNextUpActivated);
    // Per-row options button, as in the rest of the surfaces with rows. Here
    // the page DOES have the repos (watch later, playlists) and the queue
    // controller, so it adds its own action ("Quitar de la cola") in front
    // of the shared menu.
    m_nextUpList->setRowMoreVisible(true);
    connect(m_nextUpList, &VideoListView::moreRequested, this,
            &WatchPage::showQueueRowMenu);

    m_nextUpList->showVideos(QVector<VideoInfo>(), VideoListView::ListMode);
    bottomWidget->hide();
    m_title->setText(nt::tr("watch.playing"));
    m_meta->clear();
    m_channelBtn->setText(nt::tr("channel.title"));
    m_descLabel->hide();
    updateActionButtons();
}

void WatchPage::setWatchData(const QVector<VideoInfo> &queue, int startIndex)
{
    m_queue = queue;
    m_queueStart = startIndex;
    NT_DEBUG("E2E setWatchData queue=%d start=%d\n", (int)queue.size(),
             startIndex);
    updateQueue();
}

void WatchPage::triggerRadio(const QString &videoId)
{
    if (videoId.isEmpty())
        return;
    m_relatedVideoId = videoId;
    NT_DEBUG("E2E nextUp triggerRadio id=%s\n",
             videoId.toUtf8().constData());
    m_yt->radioVideos(videoId);
}

void WatchPage::detachPlayerTo(QWidget *host)
{
    if (!m_playerWidget || !host || m_playerDetached)
        return;
    // The overlay (controls) is a child of the player: it is hidden, not moved. The
    // player is reparented visible so mpv keeps rendering in the mini.
    m_overlay->hide();
    m_playerWidget->setParent(host);
    m_playerWidget->setGeometry(host->rect());
    m_playerWidget->show();
    m_playerDetached = true;
    NT_DEBUG("E2E player detach host=%p\n", static_cast<void *>(host));
}

void WatchPage::attachPlayer()
{
    if (!m_playerWidget || !m_playerContainer || !m_playerDetached)
        return;
    m_playerContainer->givePlayerBack();
    m_overlay->setGeometry(m_playerWidget->rect());
    m_overlay->show();
    m_playerDetached = false;
    NT_DEBUG("E2E player attach\n");
}

void WatchPage::onCurrentChanged()
{
    // New video: live detection restarts (the duration is received
    // from scratch; the previous video's state is not carried over).
    m_liveDet.reset();
    m_liveDurPeak = 0.0;
    if (m_liveMedia) {
        m_liveMedia = false;
        m_ovSeek->setLive(false);
        m_ovPos->setProperty("live", false);
        m_ovPos->style()->unpolish(m_ovPos);
        m_ovPos->style()->polish(m_ovPos);
        m_ovPos->update();
    }
    if (!m_player->isActive()) {
        m_title->setText(nt::tr("watch.playing"));
        return;
    }
    // Page with a single scroll (YouTube style): on changing video it
    // returns to the top so the new one starts from the video. Not in fullscreen
    // (there the scroll is hidden and the video fills the page).
    if (m_topScroll && !m_inFullscreen)
        m_topScroll->verticalScrollBar()->setValue(0);
    // New video: the ceiling of available qualities is recalculated with its
    // streamHeightsReady; until then the previous one is not carried over.
    m_availableMaxH = 0;
    m_title->setText(m_player->current().title);
    m_liked = m_favs->isFavorite(m_player->current().id);
    m_disliked = false;
    m_isStarred = m_starred->isStarred(m_player->current().id);
    m_watchLaterMarked = m_watchLater->isWatchLater(m_player->current().id);
    updateActionButtons();
    updateInfo();
    updateQueue();
    m_yt->getDetail(m_player->current().id);
    // Translated detail (normal /next): only when the setting's language is
    // explicit (es/en/pt); in "original" the /next adds nothing and the request
    // is saved. It is deferred one event loop iteration so that the POST
    // /player (critical playback path) goes first: Ipv4Https
    // is a single-socket FIFO, and if this /next enters first, the video
    // waits for the translation to finish before requesting the stream.
    if (!m_yt->audioLanguage().isEmpty()) {
        const QString id = m_player->current().id;
        QTimer::singleShot(0, this, [this, id]() {
            if (!m_yt->audioLanguage().isEmpty())
                m_yt->currentDetail(id);
        });
    }
    m_fetchingDetail = true;
    m_description.clear();
    m_meta->setText(nt::tr("watch.loadingDetails"));
}

void WatchPage::onQueueChanged()
{
    m_queue = m_player->queue();
    updateQueue();
}

void WatchPage::updateQueue()
{
    if (!m_player->isActive()) {
        m_nextUpList->clearVideos();
        if (m_bottomWidget)
            m_bottomWidget->setVisible(false);
        m_relatedVideoId.clear();
        return;
    }
    const int idx = m_player->index();
    const VideoInfo cur = m_player->current();
    const int remaining = m_queue.size() - (idx + 1);
    const NextMode mode = nextMode();

    // Seeding: only Suggestions auto-fills the queue when few items are left.
    // Radio is generated by an explicit click from outside "A continuación"
    // (search, home, channel) and does NOT auto-reload when exhausted.
    if (mode == NextMode::Suggestions && remaining <= 5 && !cur.id.isEmpty()
        && cur.id != m_relatedVideoId) {
        m_relatedVideoId = cur.id;
        NT_DEBUG("E2E nextUp seed mode=suggestions id=%s remaining=%d\n",
                 cur.id.toUtf8().constData(), remaining);
        m_yt->relatedVideos(cur.id);
    }

    // Complete list of the collection: the playing video stays marked
    // with a box and the skipped/already watched ones stay visible and eligible.
    m_nextUpList->showVideos(m_queue, VideoListView::ListMode);
    m_nextUpList->setCurrentRow(idx);
    if (m_queueCount)
        m_queueCount->setText(m_queue.isEmpty()
            ? QString()
            : nt::tr("common.videos").arg(m_queue.size()));
    // The queue lives INSIDE the page scroll: its height is adjusted to the
    // real content (rows × row height) so that the page scroll
    // moves the whole queue, with no duplicated internal scroll.
    const int rowH = m_nextUpList->sizeHintForRow(0);
    if (rowH > 0)
        m_nextUpList->setFixedHeight(qMax(1, m_queue.size()) * rowH);
    if (m_bottomWidget)
        m_bottomWidget->setVisible(!m_queue.isEmpty() && !m_inFullscreen);
}

void WatchPage::onRelatedReady(const QString &videoId,
                               const QVector<VideoInfo> &videos)
{
    if (videos.isEmpty() || videoId != m_relatedVideoId)
        return;
    const VideoInfo cur = m_player->current();
    if (cur.id != m_relatedVideoId)
        return;
    NT_DEBUG("E2E nextUp related append id=%s count=%d\n",
             videoId.toUtf8().constData(), (int)videos.size());
    m_player->appendVideos(videos);
}

void WatchPage::onRadioReady(const QString &videoId,
                             const QVector<VideoInfo> &videos)
{
    if (videoId != m_relatedVideoId)
        return;
    const VideoInfo cur = m_player->current();
    if (cur.id != m_relatedVideoId)
        return;
    if (!videos.isEmpty()) {
        NT_DEBUG("E2E nextUp radio append id=%s count=%d 1º=%s\n",
                 videoId.toUtf8().constData(), (int)videos.size(),
                 videos.first().title.toUtf8().constData());
        m_player->appendVideos(videos);
        return;
    }
    // Empty RDAMVM radio (neither panel nor mix in any client): last
    // resort with the flat /next of recommendations so as not to leave "A
    // continuación" empty. The onRelatedReady guard validates m_relatedVideoId.
    NT_DEBUG("E2E nextUp radio vacia -> fallback /next id=%s\n",
             videoId.toUtf8().constData());
    m_yt->relatedVideos(videoId);
}

void WatchPage::updateInfo()
{
    if (!m_player->isActive())
        return;
    const VideoInfo v = m_player->current();
    QStringList meta;
    if (v.viewCount > 0)
        meta << viewsLabel(v.viewCount);
    if (!v.publishedAt.isEmpty()) {
        const QString p = publishedLabel(v.publishedAt);
        if (!p.isEmpty())
            meta << p;
    }
    m_meta->setText(meta.join(nt::tr("fmt.metaSep")));
    updateChannelRow();
}

void WatchPage::updateChannelRow()
{
    if (!m_player->isActive())
        return;
    const VideoInfo v = m_player->current();
    m_channelBtn->setText(v.channel.isEmpty() ? nt::tr("channel.title")
                                              : v.channel);
    m_channelAvatarKey.clear();
    m_channelAvatarUrl.clear();
    const QIcon placeholder = nt::icon(nt::Icon::User, nt::kMuted, 20);
    if (!v.channelId.isEmpty()) {
        m_channelAvatarKey = QStringLiteral("ch_%1").arg(v.channelId);
        const QPixmap pm = m_thumbs->pixmap(m_channelAvatarKey);
        if (!pm.isNull()) {
            m_channelBtn->setIcon(avatarWithGap(nt::circularAvatar(pm, 40), 40, 3));
        } else {
            m_channelBtn->setIcon(placeholder);
            // Requests channel data (name and avatar) if missing.
            m_yt->channelInfo(v.channelId);
        }
    } else {
        m_channelBtn->setIcon(placeholder);
    }
    updateSubscribeButton(m_subs->isSubscribed(v.channelId));
}

void WatchPage::updateSubscribeButton(bool subscribed)
{
    m_subscribeBtn->setText(subscribed ? nt::tr("channel.subscribed")
                                       : nt::tr("channel.subscribe"));
    m_subscribeBtn->setIcon(QIcon());
    // Same #subscribe rule as the channel sheet button: both
    // states are decided by the `subscribed` property and the accent comes from
    // @ACCENT@. Re-polish required when changing the property.
    m_subscribeBtn->setProperty("subscribed", subscribed);
    m_subscribeBtn->style()->unpolish(m_subscribeBtn);
    m_subscribeBtn->style()->polish(m_subscribeBtn);
    m_subscribeBtn->update();
}

void WatchPage::updateSubscription(const VideoInfo &v, bool subscribe)
{
    if (!subscribe) {
        m_subs->remove(v.channelId);
    } else {
        m_subs->add(v.channelId, v.channel, m_channelAvatarUrl);
    }
    updateSubscribeButton(subscribe);
}

void WatchPage::updateActionButtons()
{
    auto paint = [this](QPushButton *b, nt::Icon icon, bool active) {
        b->setIcon(nt::icon(icon, active ? QColor(nt::accentHex())
                                         : nt::kMuted, 20));
        b->setIconSize(QSize(20, 20));
        // Both states live in the global QSS (#actionChip) and are told apart by
        // the boolean property `active`, just like the Subscribe button. The
        // accent comes from @ACCENT@, so a color change repaints it on its own
        // and there is no longer a need to redo the stylesheet here.
        b->setProperty("active", active);
        // The property changes the selector, so it must be re-polished: without this
        // Qt does not resolve the rules again and the button keeps the previous
        // state (for example, "Me gusta" pressed that is not shown as marked).
        b->style()->unpolish(b);
        b->style()->polish(b);
        b->update();
    };
    paint(m_likeBtn, m_likeIcon, m_liked);
    paint(m_dislikeBtn, m_dislikeIcon, m_disliked);
    paint(m_shareBtn, m_shareIcon, false);
    paint(m_starBtn, m_isStarred ? m_starFilledIcon : m_starIcon, m_isStarred);
    paint(m_watchLaterBtn, m_watchLaterIcon, m_watchLaterMarked);
    paint(m_playlistBtn, m_playlistIcon, false);
}

void WatchPage::showQueueRowMenu(int index, const VideoInfo &video,
                                 const QPoint &globalPos)
{
    if (video.id.isEmpty())
        return;
    QMenu menu(this);

    // Own action of the queue. The playing video is not removed from the
    // queue (see PlayerController::removeFromQueue), so in its row the
    // action does not appear instead of appearing without doing anything.
    if (index != m_player->index()) {
        QAction *rem = menu.addAction(nt::tr("watch.removeFromQueue"));
        connect(rem, &QAction::triggered, this, [this, index]() {
            // On the next loop iteration, with the menu closed: removing it deletes the row
            // from the list the popup was born from.
            QTimer::singleShot(0, this, [this, index]() {
                m_player->removeFromQueue(index);
            });
        });
    }

    nt::RowMenuHooks hooks;
    hooks.onNewPlaylist = [this](const QString &videoId) {
        promptNewPlaylist(videoId);
    };
    hooks.onChannel = [this](const QString &channelId, const QString &name) {
        emit channelRequested(channelId, name);
    };
    nt::addRowMenuActions(&menu, video, m_watchLater, m_playlists, hooks);

    menu.exec(globalPos);
}

void WatchPage::showPlaylistMenu()
{
    if (!m_player->isActive())
        return;
    const VideoInfo v = m_player->current();
    if (v.id.isEmpty())
        return;

    QMenu menu(this);
    const auto playlists = m_playlists->all();
    bool anyAdded = false;
    for (const PlaylistItem &p : playlists) {
        const bool contains = m_playlists->contains(p.id, v.id);
        if (contains)
            anyAdded = true;
        // No check glyph: "✔" (U+2714) is not in the default font and
        // came out as a dot. The "(ya está)" already says the same.
        const QString label = contains
            ? nt::tr("row.already").arg(p.title)
            : p.title;
        QAction *act = menu.addAction(label);
        act->setEnabled(!contains);
        connect(act, &QAction::triggered, this,
                [this, p, v]() { m_playlists->addVideo(p.id, v.id); });
    }
    menu.addSeparator();
    QAction *newAct = menu.addAction(nt::tr("row.newPlaylist"));
    connect(newAct, &QAction::triggered, this, [this, v]() {
        promptNewPlaylist(v.id);
    });
    if (playlists.isEmpty() && !anyAdded)
        menu.addAction(nt::tr("watch.noPlaylists"))->setEnabled(false);
    menu.exec(m_playlistBtn
                  ? m_playlistBtn->mapToGlobal(QPoint(0, m_playlistBtn->height()))
                  : QCursor::pos());
}

// Player "New playlist": nt::inputText, the SAME dialog used by
// Library, History and Playlist. Before, the Player had its own
// QFrame#popupDialog with the field here (child of the page, with no veil and no modal, and
// not closing with Escape), so the same action looked and behaved
// in three ways depending on where it was requested from. The playlist is created AFTER
// closing the dialog, not before: the text only arrives if the user accepts.
void WatchPage::promptNewPlaylist(const QString &videoId)
{
    QString name;
    if (!nt::inputText(this, nt::tr("playlist.newName"),
                       nt::tr("playlist.nameField"), QString(),
                       nt::tr("playlist.create"), &name))
        return;
    if (name.isEmpty())
        return;
    const QString id = m_playlists->create(name);
    if (id.isEmpty())
        return;
    if (!videoId.isEmpty())
        m_playlists->addVideo(id, videoId);
}

void WatchPage::onChannelInfo(const ChannelInfo &channel)
{
    if (!m_player->isActive())
        return;
    const VideoInfo v = m_player->current();
    if (channel.id != v.channelId)
        return;
    if (!channel.name.isEmpty())
        m_channelBtn->setText(channel.name);
    m_channelAvatarKey = QStringLiteral("ch_%1").arg(channel.id);
    if (!channel.avatarUrl.isEmpty()) {
        m_channelAvatarUrl = channel.avatarUrl;
        m_thumbs->requestChannelAvatar(channel.id, channel.avatarUrl);
        if (m_subs->isSubscribed(channel.id))
            m_subs->setAvatar(channel.id, channel.avatarUrl);
    }
}

void WatchPage::onAvatarReady(const QString &key)
{
    if (key != m_channelAvatarKey)
        return;
    const QPixmap pm = m_thumbs->pixmap(key);
    if (!pm.isNull())
        m_channelBtn->setIcon(avatarWithGap(nt::circularAvatar(pm, 40), 40, 3));
}

// The backend already knows the real duration of the media (mpv's or the
// FfmpegBackend audio clock). Videos opened from search/home/channel arrive
// with durationSeconds=0, and "Continuar viendo" discards everything without
// duration in the cache: that is why a video watched for several minutes never appeared
// there. ONLY the duration is filled in and what is already cached is respected (the upsert is
// an INSERT OR REPLACE, putting a queue item with empty fields would erase them).
void WatchPage::onDurationKnown(double seconds)
{
    const int dur = qRound(seconds);
    if (dur <= 0 || !m_cache || !m_player->isActive())
        return;
    const VideoInfo cur = m_player->current();
    if (cur.id.isEmpty())
        return;
    VideoInfo cached;
    const bool known = m_cache->get(cur.id, cached);
    // The player response is the AUTHORITATIVE source of whether the media is a live
    // (videoDetails.isLive, already wired up to currentIsLive()): the
    // mark is written next to the duration so Home/Library paint it without waiting
    // to re-analyze a renderer. A live broadcast provides a duration that
    // grows with time (the "so far"), so the equality guard does not
    // cut it; isLive is also compared for the rare case of a repeated duration with
    // the state changed.
    const bool live = m_player->currentIsLive();
    if (known && cached.durationSeconds == dur && cached.isLive == live)
        return; // already up to date: do not write for no reason
    if (known) {
        // Fill in only what is missing: the upsert is INSERT OR REPLACE, so
        // putting the queue item as is would erase what is already cached.
        if (cached.title.isEmpty())
            cached.title = cur.title;
        if (cached.channel.isEmpty())
            cached.channel = cur.channel;
        if (cached.channelId.isEmpty())
            cached.channelId = cur.channelId;
        if (cached.thumbnailUrl.isEmpty())
            cached.thumbnailUrl = cur.thumbnailUrl;
        if (cached.viewCount == 0)
            cached.viewCount = cur.viewCount;
        if (cached.publishedAt.isEmpty())
            cached.publishedAt = cur.publishedAt;
        cached.durationSeconds = dur;
        cached.isLive = live;
    } else {
        cached = cur;
        cached.durationSeconds = dur;
        cached.isLive = live;
    }
    m_cache->upsert(cached);
}

void WatchPage::onDetailFailed(const QString &videoId)
{
    if (!isCurrentVideo(videoId))
        return;
    // No client returned metadata; do not leave "Cargando detalles…"
    // stuck. It shows what is already known from the queue element.
    m_fetchingDetail = false;
    m_description.clear();
    m_descLabel->hide();
    updateInfo();
}

void WatchPage::onDetailReady(const VideoInfo &video, const QString &desc,
                              qint64 likes, const QVector<FormatInfo> &formats)
{
    Q_UNUSED(formats);
    if (!isCurrentVideo(video.id))
        return;
    m_player->applyCurrentDetail(video);
    m_title->setText(m_player->current().title);
    updateChannelRow();
    m_fetchingDetail = false;
    m_likeCount = likes;
    applyDescription(desc);
    QStringList meta;
    if (video.viewCount > 0)
        meta << viewsLabel(video.viewCount);
    if (!video.publishedAt.isEmpty()) {
        const QString p = publishedLabel(video.publishedAt);
        if (!p.isEmpty())
            meta << p;
    }
    meta << nt::tr("watch.likes").arg(likes);
    m_meta->setText(meta.join(nt::tr("fmt.metaSep")));
    // The detail just filled in what was missing in the queue item (channel,
    // id, views, date) via applyCurrentDetail: the list is repainted so that
    // the playing video's row already shows the three fields.
    updateQueue();
    // The /next videoMetadataRenderer does NOT bring duration. Since the upsert is
    // INSERT OR REPLACE, storing it as is would set duration=0 and erase the
    // real duration already cached by onDurationKnown(). "Continuar viendo" filters
    // by duration > 0, so losing it would drop the video from the list.
    VideoInfo toCache = video;
    if (toCache.durationSeconds <= 0) {
        VideoInfo known;
        if (m_cache->get(toCache.id, known) && known.durationSeconds > 0)
            toCache.durationSeconds = known.durationSeconds;
    }
    m_cache->upsert(toCache);
}

// Translated detail of the current video (/next): title and description in the
// requested "hl" language. It is applied if the video matches and the language is not
// "original" (when it is original, the Innertube detail already shows what
// YouTube delivers by default and we do not want to overwrite it).
void WatchPage::onCurrentDetailReady(const QString &videoId,
                                     const QString &title,
                                     const QString &description)
{
    NT_DEBUG("E2E currentDetail app id=%s title=%d desc=%d\n",
             videoId.toUtf8().constData(), title.size(), description.size());
    if (!isCurrentVideo(videoId))
        return;
    const QString audioLang = m_yt->audioLanguage();
    // If the user asked for "original", do not overwrite the description
    // (youtube delivers in the channel's language; the hl=es fix already preserves
    // Spanish speakers). Only apply for explicit languages.
    if (audioLang.isEmpty())
        return;
    if (!title.isEmpty()) {
        m_player->renameCurrent(videoId, title);
        m_title->setText(title);
    }
    if (!description.isEmpty())
        applyDescription(description);
}

// True if the given video is the one playing.
bool WatchPage::isCurrentVideo(const QString &videoId) const
{
    return m_player->isActive() && videoId == m_player->current().id;
}

// Paints the video description as the HTML of the shared helper
// (format::descriptionHtml: accent links + line breaks) and saves the
// plain text. It grows to full height (no cap or "Ver más"). Empty = hides
// the description.
void WatchPage::applyDescription(const QString &description)
{
    m_description = description;
    if (description.isEmpty()) {
        m_descLabel->hide();
        if (m_descCard)
            m_descCard->hide();
        return;
    }
    QString rich = descriptionHtml(description);
    m_descLabel->setText(rich);
    // No height cap: the description shows in full.
    m_descLabel->setMaximumHeight(QWIDGETSIZE_MAX);
    m_descLabel->show();
    if (m_descCard)
        m_descCard->show();
}

void WatchPage::onPhaseChanged(const QString &phase)
{
    if (m_qualityMenuActive) {
        m_qualityMenu->hide();
        m_qualityMenuActive = false;
    }
    if (phase == QLatin1String("loading")) {
        m_overlay->setCurrentIndex(1);
    } else if (phase == QLatin1String("error")) {
        m_errorLabel->setText(m_player->errorMessage());
        m_overlay->setCurrentIndex(2);
    } else {
        m_overlay->setCurrentIndex(0);
    }
    updateOverlayIcons();
    if (m_overlay->currentIndex() == 0)
        showOverlayControls();
    else
        hideOverlayControls();
    // Auto: the memory (quality_auto) only changes with an explicit
    // menu choice (see onQualityChanged); when a video ends the
    // player is not touched nor is the resolution raised on the fly.
    NT_DEBUG("E2E overlay visible=%d idx=%d\n", m_overlay->isVisible(),
             m_overlay->currentIndex());
    const QPoint qp = m_quality->mapToGlobal(QPoint(0, 0));
    NT_DEBUG("E2E combo geo global=(%d,%d) %dx%d\n", qp.x(), qp.y(),
             m_quality->width(), m_quality->height());
    if (QWidget *tw = m_quality->window())
        NT_DEBUG("E2E win-top global=(%d,%d) size=%dx%d combo-rel=(%d,%d)\n",
                 tw->mapToGlobal(QPoint(0, 0)).x(),
                 tw->mapToGlobal(QPoint(0, 0)).y(), tw->width(), tw->height(),
                 qp.x() - tw->mapToGlobal(QPoint(0, 0)).x(),
                 qp.y() - tw->mapToGlobal(QPoint(0, 0)).y());
}

void WatchPage::onPositionChanged(double position, double duration)
{
    // A live has no "final duration": mpv reports 0 (pure HLS live) or the
    // "so far" duration, which grows/or oscillates with each segment. With the
    // pos/dur bar the percentage jumps backwards when the duration changes.
    // It is treated like YouTube: playhead at the right edge (the live's
    // "now"), "EN VIVO" mark and the time since the broadcast in negative.
    // The DETECTION uses sustained evidence (nt::LiveDetector): a VOD materializes
    // its duration once and keeps it stable, a live changes it with each
    // segment; and if the duration stabilizes, it unlocks (false positive
    // → normal bar and seek recovered). A normal video is never marked.
    NT_DEBUG("E2E tick pos=%.1f dur=%.1f\n", position, duration);
    // Synthetic AV1 HLS (built by the app from a finite fMP4):
    // by construction it CANNOT be a live, and mpv reports unstable durations
    // there (they grow/reset while the BYTERANGE ranges are loaded),
    // which in a real live are the signal. This stream is EXCLUDED from
    // detection: never "EN VIVO", normal bar and seek always operational.
    const bool appStream = !m_player->currentCanBeLive();
    if (appStream && m_liveMedia) {
        m_liveMedia = false;
        m_ovSeek->setLive(false);
        m_liveDurPeak = 0.0;
        m_ovPos->setProperty("live", false);
        m_ovPos->style()->unpolish(m_ovPos);
        m_ovPos->style()->polish(m_ovPos);
        m_ovPos->update();
        NT_DEBUG("E2E live released (app stream) pos=%.1f dur=%.1f\n",
                 position, duration);
    }
    // AUTHORITATIVE signal (player response videoDetails.isLive): YouTube
    // declares the media as a current live -> enter EN VIVO mode
    // INSTANTLY, without waiting for the 2 duration rises (and it covers pure
    // lives with dur=0, which never produce rises). The detector still
    // governs the exit: if the duration stabilizes (finished live),
    // it unlocks on its own after ~15 s as always.
    if (!appStream && m_player->currentIsLive())
        m_liveDet.forceLive();
    const bool live = !appStream && m_liveDet.feed(duration);
    if (live != m_liveMedia) {
        m_liveMedia = live;
        m_ovSeek->setLive(live);
        NT_DEBUG("E2E live %s pos=%.1f dur=%.1f\n",
                 live ? "detect" : "released", position, duration);
        // Negative clock from zero on each (re)entry.
        m_liveDurPeak = 0.0;
        // The "EN VIVO" mark is painted in the accent with a QSS rule by
        // dynamic property (re-polish required when changing it).
        m_ovPos->setProperty("live", live);
        m_ovPos->style()->unpolish(m_ovPos);
        m_ovPos->style()->polish(m_ovPos);
        m_ovPos->update();
    }
    QSignalBlocker blockOv(m_ovSeek);
    if (m_liveMedia) {
        // Bar at the end: the playhead glued to the right edge and the live's
        // history fills backwards. The negative clock uses the PEAK
        // duration seen (monotonic, without the back-and-forth of the DVR window); without
        // known duration only the mark remains.
        m_ovSeek->setRange(0, 1);
        m_ovSeek->setValue(1);
        m_ovPos->setText(nt::tr("watch.live"));
        m_liveDurPeak = qMax(m_liveDurPeak, duration);
        m_ovDur->setText(m_liveDurPeak > 0
                             ? QStringLiteral("-%1").arg(fmtTime(m_liveDurPeak))
                             : QString());
    } else {
        m_ovSeek->setRange(0, duration > 0 ? qRound(duration) : 0);
        if (!m_ovSeek->isSeeking())
            m_ovSeek->setValue(qRound(position));
        // The clock is split: elapsed to the left of the slider and
        // duration to the right, like the mockup.
        m_ovPos->setText(fmtTime(position));
        m_ovDur->setText(fmtTime(duration));
    }
    // "Anterior" is enabled on crossing the restart threshold (advanced); the
    // change is only applied when it differs, so it is not repainted every second.
    if (m_ovPrevBtn) {
        const bool available = m_player->previousAvailable();
        if (m_ovPrevBtn->isEnabled() != available)
            m_ovPrevBtn->setEnabled(available);
    }
}

void WatchPage::onQualityChanged(int index)
{
    const int height = idxToQuality(index);
    NT_DEBUG("E2E qualityChanged index=%d -> %dp auto=%d\n", index, height,
             m_autoQuality ? 1 : 0);
    m_player->setMaxHeight(height);
    if (m_autoQuality) {
        // In Auto the menu does not leave the mode: the choice becomes the LAST
        // memorized resolution (quality_auto) and the next video reuses it.
        m_autoHeight = height;
        if (m_settings)
            m_settings->set(QStringLiteral("quality_auto"),
                            QString::number(height));
    } else if (m_settings) {
        // Fixed quality: it is persisted as an explicit value (leaves Auto).
        m_settings->set(QStringLiteral("quality"), QString::number(height));
    }
    if (m_player->isActive())
        m_player->reloadCurrent();
}

// Real video heights (Innertube adaptiveFormats) for the video being
// played. With Auto the stream is capped to the memorized quality (e.g.
// 360) and the loaded file does not bring the higher tracks, so the stream
// ceiling is useless for the menu; this data does reflect what the video offers.
void WatchPage::onStreamHeights(const QString &videoId,
                                const QVector<int> &heights)
{
    if (heights.isEmpty() || !m_player->isActive())
        return;
    // Only for the video being played: discard delayed resolutions.
    if (!isCurrentVideo(videoId))
        return;
    int maxH = 0;
    for (int h : heights) {
        if (h > maxH)
            maxH = h;
    }
    if (maxH <= 0)
        return;
    m_availableMaxH = maxH;
    NT_DEBUG("E2E streamHeights video=%s max=%d n=%d\n",
             videoId.toUtf8().constData(), maxH, int(heights.size()));
    // Reapply with the real video ceiling (not the filtered stream's).
    applyQualityLimits(m_availableMaxH);
}

// Hides the resolution menu items above the real ceiling: the video's
// (Innertube m_availableMaxH) if known; if not, the loaded file's
// one. The items are hidden (not removed) to preserve the indices of
// idxToQuality(); a value <= 0 (failed query or FILE_LOADED with no tracks) does not
// touch the menu. For sub-360p clips all stay hidden and the button shows the
// real height (see onVideoSizeChanged).
void WatchPage::applyQualityLimits(int maxVideoHeight)
{
    if (m_availableMaxH > 0)
        maxVideoHeight = m_availableMaxH;
    if (maxVideoHeight <= 0 || m_qualityItems.isEmpty())
        return;
    NT_DEBUG("E2E qualityLimits apply max=%d items=%d\n",
             maxVideoHeight, int(m_qualityItems.size()));
    for (int i = 0; i < int(m_qualityItems.size()); ++i) {
        QPushButton *b = m_qualityItems.at(i);
        if (!b)
            continue;
        b->setVisible(kQualityHeights[i] <= maxVideoHeight);
    }
    m_qualityMenu->adjustSize();
}

int WatchPage::qualityIndexForHeight(int height) const
{
    int best = 0;
    for (int i = 0; i < kQualityCount; ++i) {
        if (kQualityHeights[i] <= height)
            best = i;
        else
            break;
    }
    return best;
}

void WatchPage::onVideoSizeChanged(int width, int height)
{
    if (height <= 0 || !m_player->isActive())
        return;
    m_videoHeight = height;
    // Requested quality (the last one saved by the user or when clicking the menu):
    // the button must show that quality while the file has it.
    const int requested = idxToQuality(m_qualityIndex);
    const int visible = (requested <= height) ? requested : height;
    // The quality button is the settings icon: the active resolution lives in
    // the tooltip (the text chip left with the new row).
    m_quality->setToolTip(
        nt::tr("watch.qualityTip").arg(visible));
    // The menu is anchored with m_quality->mapTo(anchor, ...) when it opens
    // (toggleQualityMenu), so it follows the button without readjusting anything here.
    NT_DEBUG("E2E combo sync %dx%d requested=%d button=%d\n",
             width, height, requested, visible);
}

int WatchPage::idxToQuality(int idx) const
{
    if (idx >= 0 && idx < kQualityCount)
        return kQualityHeights[idx];
    return 720;
}

void WatchPage::setQualityIndex(int index)
{
    m_qualityIndex = index;
    onQualityChanged(index);
    m_quality->setToolTip(
        nt::tr("watch.qualityTip").arg(idxToQuality(index)));
}

void WatchPage::toggleQualityMenu()
{
    if (m_qualityMenuActive) {
        m_qualityMenu->hide();
        m_qualityMenuActive = false;
        return;
    }
    QWidget *anchor = m_qualityMenuAnchor ? m_qualityMenuAnchor : m_quality;
    const QPoint btnPos = m_quality->mapTo(anchor, QPoint(0, 0));
    m_qualityMenu->move(btnPos.x(), btnPos.y() - m_qualityMenu->height() - 4);
    m_qualityMenu->show();
    m_qualityMenu->raise();
    m_qualityMenuActive = true;
    const QPoint mg = m_qualityMenu->mapToGlobal(QPoint(0, 0));
    NT_DEBUG("E2E menu shown global=(%d,%d) size=%dx%d\n",
             mg.x(), mg.y(), m_qualityMenu->width(),
             m_qualityMenu->height());
    if (QWidget *tw = m_quality->window())
        NT_DEBUG("E2E win-top global=(%d,%d) size=%dx%d\n",
                 tw->mapToGlobal(QPoint(0, 0)).x(),
                 tw->mapToGlobal(QPoint(0, 0)).y(),
                 tw->width(), tw->height());
}

void WatchPage::onPlayPause()
{
    m_player->playPause();
}

void WatchPage::onToggleMute()
{
    m_player->toggleMute();
    // Persist current mute (and volume in case the overlay changed it
    // without going through the mini bar slider).
    if (m_settings) {
        m_settings->set(QStringLiteral("volume"),
                        QString::number(int(m_player->volume())));
        m_settings->set(QStringLiteral("volume_muted"),
                        m_player->muted() ? QStringLiteral("1")
                                          : QStringLiteral("0"));
    }
    updateOverlayIcons();
}

void WatchPage::onNextUpActivated(int index, const VideoInfo &)
{
    m_player->playIndex(index);
}

// Fullscreen of ONLY the video area (+ control bar), not the whole
// app: the info/queue blocks are hidden so the player area
// fills the whole page; MainWindow takes care of the window.
void WatchPage::toggleFullscreen()
{
    if (m_inFullscreen)
        leaveFullscreen();
    else
        enterFullscreen();
}

void WatchPage::enterFullscreen()
{
    if (m_inFullscreen)
        return;
    m_inFullscreen = true;
    if (m_playerContainer)
        m_playerContainer->setFullscreen(true);
    // Video-ONLY fullscreen: the Back button + description + queue are hidden.
    // With everything in the single scroll, hiding them leaves the content = only video and
    // widgetResizable stretches it to the whole page (without reparenting the player).
    // The "Volver" link is hidden in fullscreen, where only
    // the video should remain. There is no longer a fixed bar leaving a strip: the link lives inside
    // the overlay and showOverlayControls does not show it again here because the
    // guard above (m_inFullscreen) takes it into account.
    if (m_back)
        m_back->hide();
    if (m_watchBody)
        m_watchBody->hide();
    if (m_bottomWidget)
        m_bottomWidget->hide();
    if (m_topScroll)
        m_topScroll->verticalScrollBar()->setValue(0);
    if (m_overlay->currentIndex() == 0)
        showOverlayControls();
    emit fullscreenToggled(true);
    NT_DEBUG("E2E fullscreen enter\n");
}

void WatchPage::leaveFullscreen()
{
    if (!m_inFullscreen)
        return;
    m_inFullscreen = false;
    if (m_playerContainer)
        m_playerContainer->setFullscreen(false);
    // On exit the link is not forced: if the controls are visible (they usually
    // move right before leaving) it comes back with them, and if not, it will appear on the
    // next mouse movement over the video, which is the new rule.
    if (m_back && m_ctrlBar && m_ctrlBar->isVisible())
        m_back->show();
    if (m_watchBody)
        m_watchBody->show();
    if (m_bottomWidget)
        m_bottomWidget->show();
    emit fullscreenToggled(false);
    NT_DEBUG("E2E fullscreen exit\n");
}

// Event loop for the overlay: any mouse movement (or click or
// scroll) over the video area shows the controls and restarts the timer.
bool WatchPage::eventFilter(QObject *watched, QEvent *event)
{
    // In the mini player the PlayerWidget is reparented to the mini bar slot
    // (the filter is still installed on it). Events on the mini video must not
    // toggle play/pause or show the overlay: the mini bar handles the
    // click (expand to the full Player). Without this guard, in "Ver vídeo" mode
    // (thumbnail hidden) the click on the video paused when expanding.
    if (m_playerDetached)
        return QWidget::eventFilter(watched, event);
    if (event->type() == QEvent::KeyPress) {
        const auto *ke = static_cast<const QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Escape && m_inFullscreen) {
            leaveFullscreen();
            return true;
        }
    }
    switch (event->type()) {
    case QEvent::Wheel:
        showOverlayControls();
        break;
    case QEvent::MouseButtonRelease:
        // Releases whatever was pressed on the bar: on release, the 2.5 s
        // timer can hide it again. Without this, a slow click ate it.
        if (m_ctrlPressed && isControlClick(watched)) {
            m_ctrlPressed = false;
            showOverlayControls();
        }
        break;
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        // Ignore the synthetic MouseMove that the seek slider generates when
        // moving under a stationary cursor; only count real movement.
        const QPoint gp =
            static_cast<const QMouseEvent *>(event)->globalPos();
        if (gp == m_lastMousePos && event->type() == QEvent::MouseMove)
            break;
        m_lastMousePos = gp;
        showOverlayControls();
        // Marks that a control is pressed so that hideOverlayControls() does not
        // make the box disappear mid-gesture (the 2.5 s timer still runs out
        // if the user holds without moving: you press the volume, you
        // stay still and the box goes away with your finger on it).
        if (event->type() == QEvent::MouseButtonPress && isControlClick(watched))
            m_ctrlPressed = true;
        // A single click on the video toggles play/pause immediately.
        // Note: the same click hooks events on two widgets (PlayerWidget and
        // the overlay QWidget), so we deduplicate the same physical click
        // by position within a 50 ms window: otherwise it would come out pause+play.
        if (event->type() == QEvent::MouseButtonPress
            && isVideoClick(watched)) {
            const qint64 nowMs =
                static_cast<const QMouseEvent *>(event)->timestamp();
            if (nowMs == m_lastVideoPressMs
                && gp == m_lastVideoPressPos)
                break;
            m_lastVideoPressMs = nowMs;
            m_lastVideoPressPos = gp;
            NT_DEBUG("E2E playPause phase=%s\n",
                     m_player->phase().toUtf8().constData());
            m_player->playPause();
        }
        break;
    }
    default:
        break;
    }
    // Let the event through: the overlay does not consume video clicks.
    return QWidget::eventFilter(watched, event);
}

// Considers "click on the video" any click that does not point to the floating
// control bar or the quality menu, with the overlay in the normal view.
bool WatchPage::isVideoClick(const QObject *watched) const
{
    if (!m_overlay || m_overlay->currentIndex() != 0)
        return false;
    const auto *w = qobject_cast<const QWidget *>(watched);
    if (!w)
        return false;
    if (m_ctrlBar && (w == m_ctrlBar || m_ctrlBar->isAncestorOf(w)))
        return false;
    if (m_qualityMenu && (w == m_qualityMenu || m_qualityMenu->isAncestorOf(w)))
        return false;
    return true;
}

// Considers "click on a control" any click that points to the floating
// control bar (or the quality menu), with the overlay in the normal view.
// It is the counterpart of isVideoClick(): it serves to not hide the box while the
// user holds a control pressed.
bool WatchPage::isControlClick(const QObject *watched) const
{
    if (!m_overlay || m_overlay->currentIndex() != 0)
        return false;
    const auto *w = qobject_cast<const QWidget *>(watched);
    if (!w)
        return false;
    if (m_ctrlBar && (w == m_ctrlBar || m_ctrlBar->isAncestorOf(w)))
        return true;
    if (m_qualityMenu && (w == m_qualityMenu || m_qualityMenu->isAncestorOf(w)))
        return true;
    return false;
}

void WatchPage::showOverlayControls()
{
    if (!m_ctrlBar)
        return;
    if (m_overlay->currentIndex() != 0)
        return;
    NT_DEBUG("E2E overlayShow idx=%d barVisible=%d\n",
             m_overlay->currentIndex(), m_ctrlBar->isVisible());
    m_ctrlBar->show();
    m_ctrlBar->raise();
    // "Volver" comes out with the controls, but NOT in fullscreen: there only
    // the video should remain and this block also runs in fullscreen (it is the same
    // 2.5 s cycle), so the state is decided here and not in each branch.
    if (m_back)
        m_back->setVisible(!m_inFullscreen);
    if (m_qualityMenuActive && m_qualityMenu->isVisible())
        m_qualityMenu->raise();
    updateOverlayIcons();
    if (m_overlayTimer)
        m_overlayTimer->start();
}

void WatchPage::hideOverlayControls()
{
    if (!m_ctrlBar)
        return;
    // If a control is pressed, we do not hide: we postpone. Without this, holding
    // it without moving (e.g. dragging the volume) runs out the 2.5 s
    // timer and the box disappears with the finger on it, Qt releases the grab and the
    // gesture is lost midway.
    if (m_ctrlPressed) {
        if (m_overlayTimer)
            m_overlayTimer->start();
        return;
    }
    NT_DEBUG("E2E overlayHide idx=%d barVisible=%d\n",
             m_overlay->currentIndex(), m_ctrlBar->isVisible());
    m_ctrlBar->hide();
    // The "Volver" link shares the cycle with the control box.
    if (m_back)
        m_back->hide();
    // reaches the m_ctrlPressed guard, so here the state can no longer
    // be stuck. It is reset anyway: if a release were lost
    // (the filter is on the player and its children, but just in case), the
    // box would stop auto-hiding forever.
    m_ctrlPressed = false;
    if (m_qualityMenuActive) {
        m_qualityMenu->hide();
        m_qualityMenuActive = false;
    }
}

void WatchPage::updateOverlayIcons()
{
    if (!m_ovPlayBtn)
        return;
    const QColor accent(nt::accentHex());
    // Player art icons (nt::playerIcon). The active modes
    // (shuffle, repeat, captions) go in the accent color.
    m_ovPlayBtn->setIcon(nt::playerIcon(
        m_player->isPaused() ? nt::PlayerIcon::Play : nt::PlayerIcon::Pause,
        nt::kText, 22));
    m_ovNextBtn->setEnabled(m_player->hasNext());
    m_ovNextBtn->setIcon(nt::playerIcon(nt::PlayerIcon::Next, nt::kText, 20));
    if (m_ovPrevBtn) {
        // Enabled if there is a previous one OR if the current one is already advanced (restart):
        // previousAvailable() is the same condition that decides previous().
        m_ovPrevBtn->setEnabled(m_player->previousAvailable());
        m_ovPrevBtn->setIcon(
            nt::playerIcon(nt::PlayerIcon::Prev, nt::kText, 20));
    }
    m_ovShuffleBtn->setIcon(nt::playerIcon(
        nt::PlayerIcon::Shuffle, m_player->shuffle() ? accent : nt::kText, 20));
    m_ovRepeatBtn->setIcon(nt::playerIcon(
        nt::PlayerIcon::Repeat, m_player->repeat() ? accent : nt::kText, 20));
    m_ovMuteBtn->setIcon(nt::playerIcon(
        m_player->muted() ? nt::PlayerIcon::Muted : nt::PlayerIcon::Volume,
        nt::kText, 20));
    m_ovSubsBtn->setIcon(nt::playerIcon(
        nt::PlayerIcon::Captions, m_subsVisible ? accent : nt::kText, 20));
    QSignalBlocker block(m_ovVolume);
    m_ovVolume->setValue(qRound(m_player->volume()));
}

void WatchPage::refreshVolumeSlider()
{
    if (!m_ovVolume)
        return;
    QSignalBlocker block(m_ovVolume);
    // Applies the new range (it may have changed with the Settings setting).
    m_ovVolume->setRange(0, m_player->volumeMax());
    // The current volume may have been capped by the new limit.
    m_ovVolume->setValue(qRound(m_player->volume()));
}

void WatchPage::onVolumeChanged()
{
    if (!m_ovVolume)
        return;
    // On-the-fly slider synchronization after any volume change
    // (MainWindow keyboard, mute button, slider, maximum volume
    // setting). QSignalBlocker: the programmatic setValue must NOT re-fire
    // valueChanged (which would persist and re-apply the volume).
    QSignalBlocker block(m_ovVolume);
    m_ovVolume->setValue(qRound(m_player->volume()));
    m_ovMuteBtn->setIcon(nt::playerIcon(
        m_player->muted() ? nt::PlayerIcon::Muted : nt::PlayerIcon::Volume,
        nt::kText, 20));
}

void WatchPage::applyAccent()
{
    // Subscribe button: repaints with the state (subscribed / live accent) and the
    // current accent color.
    if (m_subscribeBtn)
        updateSubscribeButton(
            m_subscribeBtn->property("subscribed").toBool());
    // Actions (like/dislike/star/watch later): the icon and the text of the
    // active state use the accent.
    updateActionButtons();
    // Description: the links were painted with the previous accent color;
    // the rich text is regenerated (always at full height).
    if (!m_description.isEmpty())
        applyDescription(m_description);
    // Overlay: the active captions icon uses the accent.
    updateOverlayIcons();
}