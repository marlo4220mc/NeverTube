#include "watch_page.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "../../db/repositories.h"
#include "../../debug.h"
#include "../../models.h"
#include "../../player_controller.h"
#include "../../playerwidget.h"
#include "../../yt.h"
#include "../format.h"
#include "../icons.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

namespace {

class PlayerContainer : public QWidget
{
public:
    PlayerContainer(PlayerWidget *player, QWidget *overlay, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_overlay(overlay)
        , m_player(player)
    {
        setStyleSheet(QStringLiteral("background:#000;"));
        player->setParent(this);
        overlay->setParent(player);
        overlay->setGeometry(player->rect());
        overlay->raise();
        overlay->show();

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(player);

        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumHeight(0);
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
        const int h = qRound(width() / m_aspect);
        NT_DEBUG("E2E pc aspect=%.3f width=%d maxH=%d curMax=%d\n",
                 m_aspect, width(), h, maximumHeight());
        if (h == maximumHeight())
            return;
        setMaximumHeight(h);
    }

    QWidget *m_overlay;
    PlayerWidget *m_player;
    double m_aspect = 0.0;
};

}  // namespace

// Deslizador de búsqueda: click en cualquier punto del track busca ahí y el
// arrastre no pelea con las actualizaciones de posición mientras se mantiene
// pulsado. Emite el valor buscado (gatillo de seek) por cada cambio.
class SeekSlider : public QSlider
{
public:
    explicit SeekSlider(Qt::Orientation orientation, QWidget *parent = nullptr)
        : QSlider(orientation, parent)
    {
        setTracking(true);
    }

    bool isSeeking() const { return m_dragging; }

protected:
    static int valueFromPos(const QSlider *slider, int x)
    {
        const int w = qMax(1, slider->width());
        const qreal r = qBound(qreal(0.0), qreal(x) / qreal(w), qreal(1.0));
        return slider->minimum()
               + qRound(qreal(slider->maximum() - slider->minimum()) * r);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
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
        QSlider::mouseMoveEvent(event);
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
};

namespace {

// Avatar recortado en círculo para la fila del canal del reproductor.
QPixmap circularAvatar(const QPixmap &src, int size)
{
    QPixmap out(size, size);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addEllipse(0, 0, size, size);
    p.setClipPath(path);
    p.drawPixmap(0, 0, size, size,
                 src.scaled(size, size, Qt::KeepAspectRatioByExpanding,
                            Qt::SmoothTransformation));
    return out;
}

// Icono del canal con gap px de separación a la derecha del círculo, para
// que el nombre no quede pegado al avatar.
QIcon avatarWithGap(const QPixmap &circular, int size, int gap)
{
    QPixmap canvas(size + gap, size);
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    p.drawPixmap(0, 0, size, size, circular);
    return QIcon(canvas);
}

// Activa tracking del ratón + event filter en un widget y todos sus hijos,
// para que WatchPage pueda mostrar/ocultar los controles flotantes.
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
                     SettingsRepo *settings, ThumbnailCache *thumbs,
                     QWidget *parent)
    : QWidget(parent)
    , m_yt(yt)
    , m_player(player)
    , m_playerWidget(playerWidget)
    , m_cache(cache)
    , m_favs(favorites)
    , m_settings(settings)
    , m_thumbs(thumbs)
{
    auto *splitOuter = new QVBoxLayout(this);
    splitOuter->setContentsMargins(0, 0, 0, 0);
    splitOuter->setSpacing(0);

    m_overlay = new QStackedWidget(this);
    m_overlay->setAttribute(Qt::WA_TranslucentBackground, true);
    m_overlay->setStyleSheet(QStringLiteral("background: transparent;"));

    auto *gl = new QWidget;
    gl->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *gl1 = new QVBoxLayout(gl);
    gl1->setContentsMargins(0, 0, 0, 0);
    gl1->setSpacing(0);
    gl1->addStretch(1);

    m_ctrlBar = new QWidget(gl);
    m_ctrlBar->setObjectName(QStringLiteral("playerOverlay"));
    m_ctrlBar->setAttribute(Qt::WA_StyledBackground, true);
    m_ctrlBar->setStyleSheet(
        QStringLiteral("QWidget#playerOverlay { background-color: rgba(15,15,15,165);"
                       " border:1px solid #2A2A2A; border-radius:8px; }"));
    auto *barLay = new QVBoxLayout(m_ctrlBar);
    barLay->setContentsMargins(10, 4, 10, 6);
    barLay->setSpacing(2);

    auto *seekRow = new QHBoxLayout;
    seekRow->setSpacing(8);
    m_ovSeek = new SeekSlider(Qt::Horizontal, m_ctrlBar);
    m_ovSeek->setCursor(Qt::PointingHandCursor);
    connect(m_ovSeek, &QSlider::sliderMoved, this,
            [this](int value) { m_player->seek(value); });
    seekRow->addWidget(m_ovSeek, 1);
    m_ovTime = new QLabel(QStringLiteral("0:00 / 0:00"), m_ctrlBar);
    m_ovTime->setStyleSheet(QStringLiteral("color:#ECECEC; font-size:11px;"));
    seekRow->addWidget(m_ovTime);
    barLay->addLayout(seekRow);

    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);

    m_ovPlayBtn = new QPushButton(m_ctrlBar);
    m_ovPlayBtn->setObjectName(QStringLiteral("iconBtn"));
    m_ovPlayBtn->setCursor(Qt::PointingHandCursor);
    m_ovPlayBtn->setIcon(nt::icon(nt::Icon::Play, nt::kText, 22));
    connect(m_ovPlayBtn, &QPushButton::clicked, this,
            &WatchPage::onPlayPause);
    btnRow->addWidget(m_ovPlayBtn);

    m_ovNextBtn = new QPushButton(m_ctrlBar);
    m_ovNextBtn->setObjectName(QStringLiteral("iconBtn"));
    m_ovNextBtn->setCursor(Qt::PointingHandCursor);
    m_ovNextBtn->setIcon(nt::icon(nt::Icon::Next, nt::kText, 20));
    connect(m_ovNextBtn, &QPushButton::clicked, this, [this]() {
        if (m_player->hasNext())
            m_player->next();
    });
    btnRow->addWidget(m_ovNextBtn);

    btnRow->addStretch(1);

    m_ovMuteBtn = new QPushButton(m_ctrlBar);
    m_ovMuteBtn->setObjectName(QStringLiteral("iconBtn"));
    m_ovMuteBtn->setCursor(Qt::PointingHandCursor);
    m_ovMuteBtn->setIcon(nt::icon(nt::Icon::Mute, nt::kText, 18));
    connect(m_ovMuteBtn, &QPushButton::clicked, this,
            &WatchPage::onToggleMute);
    btnRow->addWidget(m_ovMuteBtn);

    m_ovVolume = new QSlider(Qt::Horizontal, m_ctrlBar);
    m_ovVolume->setRange(0, 100);
    m_ovVolume->setValue(int(m_player->volume()));
    m_ovVolume->setFixedWidth(80);
    m_ovVolume->setCursor(Qt::PointingHandCursor);
    connect(m_ovVolume, &QSlider::valueChanged, this, [this](int v) {
        m_player->setVolume(v);
    });
    btnRow->addWidget(m_ovVolume);

    m_ovSubsBtn = new QPushButton(m_ctrlBar);
    m_ovSubsBtn->setObjectName(QStringLiteral("iconBtn"));
    m_ovSubsBtn->setCursor(Qt::PointingHandCursor);
    m_ovSubsBtn->setToolTip(QStringLiteral("Subtítulos (CC)"));
    connect(m_ovSubsBtn, &QPushButton::clicked, this, [this]() {
        m_subsVisible = !m_subsVisible;
        if (m_playerWidget)
            m_playerWidget->setSubVisibility(m_subsVisible);
        updateOverlayIcons();
    });
    btnRow->addWidget(m_ovSubsBtn);

    m_quality = new QPushButton(QStringLiteral("720p"), m_ctrlBar);
    m_quality->setObjectName(QStringLiteral("iconBtn"));
    m_quality->setFixedWidth(98);
    m_quality->setCursor(Qt::PointingHandCursor);
    connect(m_quality, &QPushButton::clicked, this,
            &WatchPage::toggleQualityMenu);
    btnRow->addWidget(m_quality);

    m_ovFsBtn = new QPushButton(m_ctrlBar);
    m_ovFsBtn->setObjectName(QStringLiteral("iconBtn"));
    m_ovFsBtn->setCursor(Qt::PointingHandCursor);
    m_ovFsBtn->setIcon(nt::icon(nt::Icon::Fullscreen, nt::kText, 18));
    connect(m_ovFsBtn, &QPushButton::clicked, this,
            &WatchPage::toggleFullscreen);
    btnRow->addWidget(m_ovFsBtn);

    barLay->addLayout(btnRow);
    gl1->addWidget(m_ctrlBar);
    m_ctrlBar->hide();

    m_qualityMenu = new QFrame(gl);
    m_qualityMenu->setObjectName(QStringLiteral("qualityMenu"));
    m_qualityMenu->setAttribute(Qt::WA_StyledBackground, true);
    m_qualityMenu->setStyleSheet(
        QStringLiteral("QFrame#qualityMenu { background-color:#1F1F1F;"
                       " border:1px solid #2D2D2D; border-radius:6px; }"
                       "QPushButton { background:transparent; color:#ECECEC;"
                       " border:none; text-align:left; padding:6px 8px;"
                       " border-radius:4px; }"
                       "QPushButton:hover { background:#2D2D2D; }"));
    auto *menuLay = new QVBoxLayout(m_qualityMenu);
    menuLay->setContentsMargins(4, 4, 4, 4);
    menuLay->setSpacing(1);
    static const int kQualities[] = {360, 480, 720, 1080, 1440, 2160};
    for (int i = 0; i < 6; ++i) {
        auto *item = new QPushButton(
            QStringLiteral("%1p").arg(kQualities[i]), m_qualityMenu);
        item->setFixedWidth(90);
        const int idx = i;
        connect(item, &QPushButton::clicked, this, [this, idx]() {
            setQualityIndex(idx);
            m_qualityMenu->hide();
            m_qualityMenuActive = false;
        });
        menuLay->addWidget(item);
    }
    m_qualityMenu->adjustSize();
    m_qualityMenu->hide();

    m_qualityMenuAnchor = gl;

    m_overlay->addWidget(gl);
    auto *spinner = new QLabel(QStringLiteral("Cargando…"), this);
    spinner->setAlignment(Qt::AlignCenter);
    spinner->setStyleSheet(
        QStringLiteral("background: transparent; color: #8F8F8F; font-size: 13px;"));
    m_overlay->addWidget(spinner);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setAlignment(Qt::AlignCenter);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(
        QStringLiteral("background: transparent; color: #C8C8C8; "
                       "font-size: 14px; padding: 16px;"));
    m_overlay->addWidget(m_errorLabel);
    m_overlay->setCurrentIndex(0);
    enableOverlayTracking(this, gl);

    // Ocultar los controles flotantes tras inactividad del ratón.
    m_overlayTimer = new QTimer(this);
    m_overlayTimer->setSingleShot(true);
    m_overlayTimer->setInterval(2500);
    connect(m_overlayTimer, &QTimer::timeout, this,
            &WatchPage::hideOverlayControls);

    // Tap vs doble click en el vídeo: un solo click espera el intervalo de
    // doble click; si llega un doble click se cancela y se alterna fullscreen.
    m_videoTapTimer = new QTimer(this);
    m_videoTapTimer->setSingleShot(true);
    m_videoTapTimer->setInterval(QApplication::doubleClickInterval());
    connect(m_videoTapTimer, &QTimer::timeout, this, &WatchPage::onVideoTap);

    auto *pc = new PlayerContainer(playerWidget, m_overlay, this);
    pc->setObjectName(QStringLiteral("playerArea"));
    splitOuter->addWidget(pc, 3);
    enableOverlayTracking(this, pc);
    connect(playerWidget, &PlayerWidget::videoAspectChanged, pc,
            &PlayerContainer::setVideoAspect);
    // Sin marco de foco tipo botón al hacer click sobre el vídeo.
    pc->setFocusPolicy(Qt::NoFocus);
    playerWidget->setFocusPolicy(Qt::NoFocus);
    m_overlay->setFocusPolicy(Qt::NoFocus);
    gl->setFocusPolicy(Qt::NoFocus);

    auto *topScroll = new QScrollArea(this);
    topScroll->setWidgetResizable(true);
    topScroll->setFrameShape(QFrame::NoFrame);
    m_topScroll = topScroll;
    auto *topWidget = new QWidget;
    auto *top = new QVBoxLayout(topWidget);
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(0);
    topScroll->setWidget(topWidget);

    auto *backRow = new QHBoxLayout;
    backRow->setContentsMargins(12, 8, 12, 0);
    m_back = new QPushButton(QStringLiteral("◀ Volver"), this);
    m_back->setObjectName(QStringLiteral("ghost"));
    m_back->setIcon(nt::icon(nt::Icon::Back, nt::kText, 14));
    backRow->addWidget(m_back);
    backRow->addStretch(1);
    top->addLayout(backRow);

    auto *infoWidget = new QWidget;
    infoWidget->setObjectName(QStringLiteral("watchBody"));
    auto *infoLay = new QVBoxLayout(infoWidget);
    infoLay->setContentsMargins(16, 14, 16, 8);
    infoLay->setSpacing(6);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("sectionTitle"));
    m_title->setWordWrap(true);
    infoLay->addWidget(m_title);

    m_meta = new QLabel(this);
    m_meta->setStyleSheet(QStringLiteral("color: #9aa2ad; font-size: 12px;"));
    infoLay->addWidget(m_meta);

    m_channelBtn = new QPushButton(QStringLiteral("Canal"), this);
    m_channelBtn->setObjectName(QStringLiteral("ghost"));
    m_channelBtn->setCursor(Qt::PointingHandCursor);
    m_channelBtn->setIconSize(QSize(43, 40));
    m_channelBtn->setIcon(nt::icon(nt::Icon::User, nt::kMuted, 20));
    m_channelBtn->setStyleSheet(
        QStringLiteral("QPushButton { background:transparent; border:none;"
                       " border-radius:8px; text-align:left; padding:8px;"
                       " color:#ECECEC; font-weight:600; font-size:13px; }"
                       "QPushButton:hover { background:#1F1F1F; }"));
    m_channelBtn->setMinimumHeight(56);
    infoLay->addWidget(m_channelBtn);

    m_descLabel = new QLabel(this);
    m_descLabel->setWordWrap(true);
    m_descLabel->setTextFormat(Qt::RichText);
    m_descLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse
                                         | Qt::TextSelectableByMouse);
    m_descLabel->setOpenExternalLinks(true);
    m_descLabel->setStyleSheet(
        QStringLiteral("color:#9aa2ad; font-size:12px; background:transparent;"));
    infoLay->addWidget(m_descLabel);
    top->addWidget(infoWidget);
    top->addStretch(1);

    auto *bottomWidget = new QWidget;
    m_bottomWidget = bottomWidget;
    auto *bot = new QVBoxLayout(bottomWidget);
    bot->setContentsMargins(0, 8, 16, 8);
    bot->setSpacing(6);
    auto *quTitle = new QLabel(QStringLiteral("A continuación"), this);
    quTitle->setStyleSheet(
        QStringLiteral("font-weight:600; font-size:13px; color:#ECECEC;"));
    m_nextUpList = new VideoListView(m_thumbs, this);
    m_nextUpList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_nextUpList->setSizePolicy(QSizePolicy::Expanding,
                                QSizePolicy::Expanding);
    m_nextUpList->setMinimumHeight(90);
    bot->addWidget(quTitle);
    bot->addWidget(m_nextUpList);

    auto *belowRow = new QWidget(this);
    m_belowWidget = belowRow;
    auto *row = new QHBoxLayout(belowRow);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(topScroll, 7);
    row->addWidget(bottomWidget, 3);
    splitOuter->addWidget(belowRow, 1);

    connect(m_back, &QPushButton::clicked, this,
            &WatchPage::backRequested);
    connect(m_channelBtn, &QPushButton::clicked, this, [this]() {
        if (m_player->index() < 0)
            return;
        const VideoInfo v = m_player->current();
        if (!v.channelId.isEmpty())
            emit channelRequested(v.channelId, v.channel);
    });
    connect(m_player, &PlayerController::currentChanged, this,
            &WatchPage::onCurrentChanged);
    connect(m_player, &PlayerController::queueChanged, this,
            &WatchPage::onQueueChanged);
    connect(m_player, &PlayerController::phaseChanged, this,
            &WatchPage::onPhaseChanged);
    connect(m_player, &PlayerController::positionChanged, this,
            &WatchPage::onPositionChanged);
    connect(playerWidget, &PlayerWidget::videoSizeChanged, this,
            &WatchPage::onVideoSizeChanged);
    connect(m_yt, &Yt::detailReady, this, &WatchPage::onDetailReady);
    connect(m_yt, &Yt::relatedReady, this, &WatchPage::onRelatedReady);
    connect(m_yt, &Yt::radioReady, this, &WatchPage::onRadioReady);
    connect(m_yt, &Yt::channelInfoReady, this,
            &WatchPage::onChannelInfo);
    connect(m_thumbs, &ThumbnailCache::ready, this,
            &WatchPage::onAvatarReady);
    connect(m_nextUpList, &VideoListView::videoActivated, this,
            &WatchPage::onNextUpActivated);

    m_nextUpList->showVideos(QVector<VideoInfo>(), VideoListView::ListMode);
    bottomWidget->hide();
    m_title->setText(QStringLiteral("Reproduciendo…"));
    m_meta->clear();
    m_channelBtn->setText(QStringLiteral("Canal"));
    m_descLabel->hide();
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

void WatchPage::onCurrentChanged()
{
    if (!m_player->isActive()) {
        m_title->setText(QStringLiteral("Reproduciendo…"));
        return;
    }
    m_title->setText(m_player->current().title);
    updateInfo();
    updateQueue();
    m_yt->getDetail(m_player->current().id);
    m_fetchingDetail = true;
    m_description.clear();
    m_meta->setText(QStringLiteral("Cargando detalles…"));
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

    // Siembra: solo Suggestions auto-llena la cola cuando quedan pocos ítems.
    // Radio se genera por un click explícito desde fuera de "A continuación"
    // (search, home, channel) y NO auto-recarga al agotar.
    if (mode == NextMode::Suggestions && remaining <= 5 && !cur.id.isEmpty()
        && cur.id != m_relatedVideoId) {
        m_relatedVideoId = cur.id;
        NT_DEBUG("E2E nextUp seed mode=suggestions id=%s remaining=%d\n",
                 cur.id.toUtf8().constData(), remaining);
        m_yt->relatedVideos(cur.id);
    }

    // Lista completa de la colección: el vídeo en reproducción queda marcado
    // con recuadro y los saltados/ya vistos siguen visibles y elegibles.
    m_nextUpList->showVideos(m_queue, VideoListView::ListMode);
    m_nextUpList->setCurrentRow(idx);
    if (m_bottomWidget)
        m_bottomWidget->setVisible(!m_queue.isEmpty());
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
    if (videos.isEmpty() || videoId != m_relatedVideoId)
        return;
    const VideoInfo cur = m_player->current();
    if (cur.id != m_relatedVideoId)
        return;
    NT_DEBUG("E2E nextUp radio append id=%s count=%d 1º=%s\n",
             videoId.toUtf8().constData(), (int)videos.size(),
             videos.first().title.toUtf8().constData());
    m_player->appendVideos(videos);
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
    m_meta->setText(meta.join(QStringLiteral("  •  ")));
    updateChannelRow();
}

void WatchPage::updateChannelRow()
{
    if (!m_player->isActive())
        return;
    const VideoInfo v = m_player->current();
    m_channelBtn->setText(v.channel.isEmpty() ? QStringLiteral("Canal")
                                              : v.channel);
    m_channelAvatarKey.clear();
    const QIcon placeholder = nt::icon(nt::Icon::User, nt::kMuted, 20);
    if (!v.channelId.isEmpty()) {
        m_channelAvatarKey = QStringLiteral("ch_%1").arg(v.channelId);
        const QPixmap pm = m_thumbs->pixmap(m_channelAvatarKey);
        if (!pm.isNull()) {
            m_channelBtn->setIcon(avatarWithGap(circularAvatar(pm, 40), 40, 3));
        } else {
            m_channelBtn->setIcon(placeholder);
            // Pide datos del canal (nombre y avatar) si faltan.
            m_yt->channelInfo(v.channelId);
        }
    } else {
        m_channelBtn->setIcon(placeholder);
    }
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
    if (!channel.avatarUrl.isEmpty())
        m_thumbs->requestChannelAvatar(channel.id, channel.avatarUrl);
}

void WatchPage::onAvatarReady(const QString &key)
{
    if (key != m_channelAvatarKey)
        return;
    const QPixmap pm = m_thumbs->pixmap(key);
    if (!pm.isNull())
        m_channelBtn->setIcon(avatarWithGap(circularAvatar(pm, 40), 40, 3));
}

void WatchPage::onDetailReady(const VideoInfo &video, const QString &desc,
                              qint64 likes, const QVector<FormatInfo> &formats)
{
    Q_UNUSED(formats);
    if (!m_player->isActive() || video.id != m_player->current().id)
        return;
    m_player->applyCurrentDetail(video);
    m_title->setText(m_player->current().title);
    updateChannelRow();
    m_fetchingDetail = false;
    m_description = desc;
    m_likeCount = likes;
    if (!desc.isEmpty()) {
        QString rich = desc.toHtmlEscaped();
        rich.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        const QRegularExpression urlRe(QStringLiteral(
            R"((https?://[^\s<]+))"));
        rich.replace(urlRe, QStringLiteral(
            "<a href=\"\\1\" style=\"color:#FF0033;\">\\1</a>"));
        m_descLabel->setText(QStringLiteral(
            "<style>a { text-decoration:none; }</style>") + rich);
        m_descLabel->show();
    } else {
        m_descLabel->hide();
    }
    QStringList meta;
    if (video.viewCount > 0)
        meta << viewsLabel(video.viewCount);
    if (!video.publishedAt.isEmpty()) {
        const QString p = publishedLabel(video.publishedAt);
        if (!p.isEmpty())
            meta << p;
    }
    meta << QStringLiteral("%1 me gusta").arg(likes);
    m_meta->setText(meta.join(QStringLiteral("  •  ")));
    m_cache->upsert(video);
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
    QSignalBlocker blockOv(m_ovSeek);
    m_ovSeek->setRange(0, duration > 0 ? qRound(duration) : 0);
    if (!m_ovSeek->isSeeking())
        m_ovSeek->setValue(qRound(position));
    m_ovTime->setText(QStringLiteral("%1 / %2")
                          .arg(fmtTime(position), fmtTime(duration)));
}

void WatchPage::onQualityChanged(int index)
{
    static const int qualities[] = {360, 480, 720, 1080, 1440, 2160};
    const int height = qualities[index];
    NT_DEBUG("E2E qualityChanged index=%d -> %dp\n", index, height);
    m_player->setMaxHeight(height);
    if (m_settings)
        m_settings->set(QStringLiteral("quality"), QString::number(height));
    if (m_player->isActive())
        m_player->reloadCurrent();
}

int WatchPage::qualityIndexForHeight(int height) const
{
    static const int qualities[] = {360, 480, 720, 1080, 1440, 2160};
    constexpr int n = 6;
    int best = 0;
    for (int i = 0; i < n; ++i) {
        if (qualities[i] <= height)
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
    const int idx = qualityIndexForHeight(height);
    m_qualityIndex = idx;
    m_quality->setText(QStringLiteral("%1p").arg(idxToQuality(idx)));
    NT_DEBUG("E2E combo sync %dx%d -> idx=%d\n", width, height, idx);
}

int WatchPage::idxToQuality(int idx) const
{
    static const int qualities[] = {360, 480, 720, 1080, 1440, 2160};
    if (idx >= 0 && idx < 6)
        return qualities[idx];
    return 720;
}

void WatchPage::setQualityIndex(int index)
{
    m_qualityIndex = index;
    onQualityChanged(index);
    m_quality->setText(QStringLiteral("%1p").arg(idxToQuality(index)));
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
    updateOverlayIcons();
}

void WatchPage::onNextUpActivated(int index, const VideoInfo &)
{
    m_player->playIndex(index);
}

// Fullscreen de SOLO el área de vídeo (+ barra de controles), no de la app
// completa: se ocultan los bloques de información/cola para que el área del
// reproductor llene toda la página; MainWindow se encarga de la ventana.
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
    if (m_belowWidget)
        m_belowWidget->hide();
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
    if (m_belowWidget)
        m_belowWidget->show();
    emit fullscreenToggled(false);
    NT_DEBUG("E2E fullscreen exit\n");
}

// Bucle de eventos para el overlay: cualquier movimiento del ratón (o click o
// scroll) sobre el área de vídeo muestra los controles y reinicia el temporizador.
bool WatchPage::eventFilter(QObject *watched, QEvent *event)
{
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
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        // Ignorar los MouseMove sintéticos que genera el slider de búsqueda al
        // desplazarse bajo un cursor estacionario; solo contar movimiento real.
        const QPoint gp =
            static_cast<const QMouseEvent *>(event)->globalPos();
        if (gp == m_lastMousePos && event->type() == QEvent::MouseMove)
            break;
        m_lastMousePos = gp;
        showOverlayControls();
        if (event->type() == QEvent::MouseButtonPress) {
            // Un solo click sobre el vídeo: play/pausa (espera al doble click).
            if (isVideoClick(watched))
                m_videoTapTimer->start();
            else
                m_videoTapTimer->stop();
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            m_videoTapTimer->stop();
            if (isVideoClick(watched))
                toggleFullscreen();
        }
        break;
    }
    default:
        break;
    }
    // Deja pasar el evento: el overlay no consume clics del vídeo.
    return QWidget::eventFilter(watched, event);
}

void WatchPage::onVideoTap()
{
    m_player->playPause();
}

// Considera "click sobre el vídeo" cualquier click que no apunte a la barra de
// controles flotante ni al menú de calidad, con el overlay en la vista normal.
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
    NT_DEBUG("E2E overlayHide idx=%d barVisible=%d\n",
             m_overlay->currentIndex(), m_ctrlBar->isVisible());
    m_ctrlBar->hide();
    if (m_qualityMenuActive) {
        m_qualityMenu->hide();
        m_qualityMenuActive = false;
    }
}

void WatchPage::updateOverlayIcons()
{
    if (!m_ovPlayBtn)
        return;
    m_ovPlayBtn->setIcon(nt::icon(
        m_player->isPaused() ? nt::Icon::Play : nt::Icon::Pause,
        nt::kText, 22));
    m_ovNextBtn->setEnabled(m_player->hasNext());
    m_ovNextBtn->setIcon(nt::icon(nt::Icon::Next, nt::kText, 20));
    m_ovMuteBtn->setIcon(nt::icon(
        m_player->muted() ? nt::Icon::Unmute : nt::Icon::Mute,
        nt::kText, 18));
    m_ovSubsBtn->setIcon(nt::icon(
        nt::Icon::Subtitles,
        m_subsVisible ? QColor("#FF0033") : nt::kText, 18));
    QSignalBlocker block(m_ovVolume);
    m_ovVolume->setValue(qRound(m_player->volume()));
}