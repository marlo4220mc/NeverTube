#include "watch_page.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "../../db/repositories.h"
#include "../../models.h"
#include "../../player_controller.h"
#include "../../playerwidget.h"
#include "../../yt.h"
#include "../format.h"
#include "../icons.h"
#include "../theme.h"
#include "../video_list_view.h"

#include <cstdio>

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
        overlay->setParent(this);

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        lay->addWidget(player);

        overlay->raise();
        overlay->setGeometry(rect());
        overlay->hide();
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumHeight(180);
        emitSetHeight();
    }

    QSize sizeHint() const override { return QSize(320, 180); }

    void setVideoAspect(double aspect)
    {
        if (aspect <= 0.0)
            return;
        m_aspect = aspect;
        emitSetHeight();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        m_overlay->setGeometry(rect());
        if (m_aspect > 0.0)
            emitSetHeight();
        fprintf(stderr, "E2E pc resize %dx%d\n", width(), height());
    }

private:
    void emitSetHeight()
    {
        if (m_resizing || m_aspect <= 0.0)
            return;
        const int h = qRound(width() / m_aspect);
        if (h == height())
            return;
        m_resizing = true;
        setFixedHeight(h);
        m_resizing = false;
    }

    QWidget *m_overlay;
    PlayerWidget *m_player;
    double m_aspect = 0.0;
    bool m_resizing = false;
};

} // namespace

WatchPage::WatchPage(Yt *yt, PlayerController *player, PlayerWidget *playerWidget,
                     VideoCacheRepo *cache, FavoritesRepo *favorites,
                     SettingsRepo *settings, ThumbnailCache *thumbs,
                     QWidget *parent)
    : QWidget(parent)
    , m_yt(yt)
    , m_player(player)
    , m_cache(cache)
    , m_favs(favorites)
    , m_settings(settings)
    , m_thumbs(thumbs)
{
    auto *splitOuter = new QVBoxLayout(this);
    splitOuter->setContentsMargins(0, 0, 0, 0);
    splitOuter->setSpacing(0);

    m_overlay = new QStackedWidget(this);

    auto *gl = new QWidget;
    auto *gl1 = new QVBoxLayout(gl);
    gl1->setContentsMargins(16, 16, 16, 16);
    gl1->setSpacing(0);
    gl1->addStretch(1);
    m_quality = new QComboBox(this);
    m_quality->addItems(QStringList() << "360p" << "480p" << "720p" << "1080p"
                                      << "1440p" << "2160p");
    m_quality->setCurrentIndex(2);
    m_quality->setFixedWidth(100);
    auto *qualityRow = new QHBoxLayout;
    qualityRow->addStretch(1);
    qualityRow->addWidget(m_quality);
    gl1->addLayout(qualityRow);
    gl->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_quality->setAttribute(Qt::WA_TransparentForMouseEvents, false);

    m_overlay->addWidget(gl);
    auto *spinner = new QLabel(QStringLiteral("Cargando…"), this);
    spinner->setAlignment(Qt::AlignCenter);
    spinner->setStyleSheet(
        QStringLiteral("background: transparent; color: #8F8F8F; font-size: 13px;"));
    m_overlay->addWidget(spinner);
    m_overlay->setCurrentIndex(0);

    auto *pc = new PlayerContainer(playerWidget, m_overlay, this);
    pc->setObjectName(QStringLiteral("playerArea"));
    splitOuter->addWidget(pc, 3);
    connect(playerWidget, &PlayerWidget::videoAspectChanged, pc,
            &PlayerContainer::setVideoAspect);

    auto *topScroll = new QScrollArea(this);
    topScroll->setWidgetResizable(true);
    topScroll->setFrameShape(QFrame::NoFrame);
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

    m_channelBtn = new QPushButton(this);
    m_channelBtn->setObjectName(QStringLiteral("ghost"));
    m_channelBtn->setStyleSheet(
        QStringLiteral("text-align:left; padding: 8px 0px; color:#ECECEC; "
                       "font-weight:600; font-size:13px;"));
    infoLay->addWidget(m_channelBtn);

    auto *controls = new QHBoxLayout;
    controls->setSpacing(4);
    m_playPauseBtn = new QPushButton(this);
    m_playPauseBtn->setObjectName(QStringLiteral("iconBtn"));
    m_nextBtn = new QPushButton(this);
    m_nextBtn->setObjectName(QStringLiteral("iconBtn"));
    m_muteBtn = new QPushButton(this);
    m_muteBtn->setObjectName(QStringLiteral("iconBtn"));
    m_favBtn = new QPushButton(this);
    m_favBtn->setObjectName(QStringLiteral("iconBtn"));
    controls->addWidget(m_playPauseBtn);
    controls->addWidget(m_nextBtn);
    controls->addWidget(m_muteBtn);
    controls->addStretch(1);
    controls->addWidget(m_favBtn);
    controls->addStretch(8);
    infoLay->addLayout(controls);

    m_slider = new QSlider(Qt::Horizontal, this);
    infoLay->addWidget(m_slider);
    m_time = new QLabel(QStringLiteral("0:00 / 0:00"), this);
    m_time->setStyleSheet(QStringLiteral("color:#8F8F8F; font-size:11px;"));
    infoLay->addWidget(m_time);

    auto *descRow = new QHBoxLayout;
    m_descLabel = new QLabel(this);
    m_descLabel->setWordWrap(true);
    m_descLabel->setStyleSheet(QStringLiteral("color:#9aa2ad; font-size:12px;"));
    m_descLabel->setMaximumHeight(60);
    m_descToggle = new QPushButton(this);
    m_descToggle->setObjectName(QStringLiteral("ghost"));
    m_descToggle->setFixedWidth(100);
    descRow->addWidget(m_descLabel, 1);
    descRow->addWidget(m_descToggle);
    infoLay->addLayout(descRow);
    top->addWidget(infoWidget);
    top->addStretch(1);

    splitOuter->addWidget(topScroll, 1);

    auto *bottomWidget = new QWidget;
    auto *bot = new QVBoxLayout(bottomWidget);
    bot->setContentsMargins(16, 8, 16, 8);
    bot->setSpacing(6);
    auto *quTitle = new QLabel(QStringLiteral("A continuación"), this);
    quTitle->setStyleSheet(
        QStringLiteral("font-weight:600; font-size:13px; color:#ECECEC;"));
    m_nextUpList = new VideoListView(m_thumbs, this);
    m_nextUpList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_nextUpList->setFixedHeight(120);
    bot->addWidget(quTitle);
    bot->addWidget(m_nextUpList);
    splitOuter->addWidget(bottomWidget, 1);

    connect(m_back, &QPushButton::clicked, this,
            &WatchPage::backRequested);
    connect(m_playPauseBtn, &QPushButton::clicked, this,
            &WatchPage::onPlayPause);
    connect(m_muteBtn, &QPushButton::clicked, this,
            &WatchPage::onToggleMute);
    connect(m_favBtn, &QPushButton::clicked, this,
            &WatchPage::onFavToggle);
    connect(m_quality,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &WatchPage::onQualityChanged);
    connect(m_slider, &QSlider::sliderReleased, this, [this]() {
        m_player->seek(m_slider->value());
    });
    connect(m_descToggle, &QPushButton::clicked, this, [this]() {
        m_descExpanded = !m_descExpanded;
        m_descLabel->setMaximumHeight(m_descExpanded ? 16777215 : 60);
        m_descToggle->setText(m_descExpanded
                                  ? QStringLiteral("Ocultar")
                                  : QStringLiteral("Mostrar más"));
    });
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
    connect(m_yt, &Yt::detailReady, this, &WatchPage::onDetailReady);
    connect(m_nextUpList, &VideoListView::videoActivated, this,
            &WatchPage::onNextUpActivated);

    m_nextUpList->showVideos(QVector<VideoInfo>(), VideoListView::ListMode);
    bottomWidget->hide();
    m_title->setText(QStringLiteral("Reproduciendo…"));
    m_meta->clear();
    m_channelBtn->setText(QStringLiteral("Canal"));
    m_descLabel->hide();
    m_descToggle->hide();
}

void WatchPage::setWatchData(const QVector<VideoInfo> &queue, int startIndex)
{
    m_queue = queue;
    m_queueStart = startIndex;
    updateQueue();
}

void WatchPage::onCurrentChanged()
{
    if (!m_player->isActive()) {
        m_title->setText(QStringLiteral("Reproduciendo…"));
        return;
    }
    m_descExpanded = false;
    m_descLabel->setMaximumHeight(60);
    m_descToggle->hide();
    m_title->setText(m_player->current().title);
    updateInfo();
    updateFavIcon();
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
        return;
    }
    const int idx = m_player->index();
    if (idx + 1 >= m_queue.size()) {
        m_nextUpList->clearVideos();
    } else {
        m_nextUp = m_queue.mid(idx + 1);
        m_nextUpList->showVideos(m_nextUp, VideoListView::ListMode);
    }
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
    m_channelBtn->setText(v.channel.isEmpty() ? QStringLiteral("Canal")
                                              : v.channel);
}

void WatchPage::updateFavIcon()
{
    if (!m_player->isActive())
        return;
    const bool fav = m_favs->isFavorite(m_player->current().id);
    m_favBtn->setIcon(nt::icon(
        fav ? nt::Icon::StarFilled : nt::Icon::Star,
        fav ? QColor("#FF0033") : nt::kMuted, 18));
    m_favBtn->setToolTip(fav ? QStringLiteral("Quitar de favoritos")
                             : QStringLiteral("Añadir a favoritos"));
}

void WatchPage::onDetailReady(const VideoInfo &video, const QString &desc,
                              qint64 likes, const QVector<FormatInfo> &formats)
{
    Q_UNUSED(formats);
    if (!m_player->isActive() || video.id != m_player->current().id)
        return;
    m_fetchingDetail = false;
    m_description = desc;
    m_likeCount = likes;
    if (!desc.isEmpty()) {
        m_descLabel->setText(desc);
        m_descLabel->show();
        m_descToggle->show();
    } else {
        m_descLabel->hide();
        m_descToggle->hide();
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
    if (phase == QLatin1String("loading")) {
        m_overlay->setCurrentIndex(1);
        m_playPauseBtn->hide();
    } else {
        m_overlay->setCurrentIndex(0);
        m_playPauseBtn->show();
    }
    m_playPauseBtn->setIcon(nt::icon(
        m_player->isPaused() ? nt::Icon::Play : nt::Icon::Pause,
        nt::kText, 22));
    m_nextBtn->setEnabled(m_player->hasNext());
    m_nextBtn->setIcon(nt::icon(nt::Icon::Next, nt::kText, 20));
    updateFavIcon();
}

void WatchPage::onPositionChanged(double position, double duration)
{
    QSignalBlocker block(m_slider);
    m_slider->setRange(0, duration > 0 ? qRound(duration) : 0);
    m_slider->setValue(qRound(position));
    m_time->setText(QStringLiteral("%1 / %2")
                        .arg(fmtTime(position), fmtTime(duration)));
}

void WatchPage::onQualityChanged(int index)
{
    static const int qualities[] = {360, 480, 720, 1080, 1440, 2160};
    const int height = qualities[index];
    m_player->setMaxHeight(height);
    if (m_settings)
        m_settings->set(QStringLiteral("quality"), QString::number(height));
    if (m_player->isActive())
        m_player->reloadCurrent();
}

void WatchPage::onPlayPause()
{
    m_player->playPause();
}

void WatchPage::onToggleMute()
{
    m_player->toggleMute();
    m_muteBtn->setIcon(nt::icon(
        m_player->muted() ? nt::Icon::Unmute : nt::Icon::Mute,
        nt::kMuted, 20));
}

void WatchPage::onFavToggle()
{
    if (!m_player->isActive())
        return;
    const VideoInfo v = m_player->current();
    m_favs->toggle(v.id);
    updateFavIcon();
}

void WatchPage::onNextUpActivated(int index, const VideoInfo &)
{
    const int global = m_player->index() + 1 + index;
    m_player->playIndex(global);
}