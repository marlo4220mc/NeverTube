#include "home_page.h"

#include <cstdio>

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "../../db/repositories.h"
#include "../../models.h"
#include "../../yt.h"
#include "../icons.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

namespace {

const int kGridW = 320;
const int kGridH = 258;

} // namespace

HomePage::HomePage(Yt *yt, SubscriptionsRepo *subs, ThumbnailCache *thumbs,
                   QWidget *parent)
    : QWidget(parent)
    , m_yt(yt)
    , m_subs(subs)
    , m_thumbs(thumbs)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_body = new QWidget;
    m_scroll->setWidget(m_body);

    auto *bodyLayout = new QVBoxLayout(m_body);
    bodyLayout->setContentsMargins(16, 16, 16, 32);
    bodyLayout->setSpacing(8);

    auto *topRow = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Inicio"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    auto *refresh = new QPushButton(QStringLiteral("  Actualizar"), this);
    refresh->setObjectName(QStringLiteral("ghost"));
    refresh->setIcon(nt::icon(nt::Icon::Refresh, nt::kText, 15));
    topRow->addWidget(title);
    topRow->addStretch(1);
    topRow->addWidget(refresh);
    bodyLayout->addLayout(topRow);

    m_busy = new QLabel(QStringLiteral("Cargando…"), this);
    m_busy->setObjectName(QStringLiteral("sectionSub"));
    m_busy->setAlignment(Qt::AlignCenter);
    m_busy->setContentsMargins(0, 48, 0, 48);
    bodyLayout->addWidget(m_busy);

    m_feedTitle = new QLabel(QStringLiteral("Recientes de tus suscripciones"),
                             this);
    m_feedTitle->setObjectName(QStringLiteral("sectionTitle"));
    m_feedGrid = new VideoListView(m_thumbs, this);
    m_feedGrid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_feedGrid->setFixedHeight(kGridH);
    bodyLayout->addWidget(m_feedTitle);
    bodyLayout->addWidget(m_feedGrid);

    m_trendTitle = new QLabel(QStringLiteral("Tendencias"), this);
    m_trendTitle->setObjectName(QStringLiteral("sectionTitle"));
    m_trendGrid = new VideoListView(m_thumbs, this);
    m_trendGrid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_trendGrid->setFixedHeight(kGridH);
    bodyLayout->addWidget(m_trendTitle);
    bodyLayout->addWidget(m_trendGrid);

    m_banner = new QLabel(this);
    m_banner->setWordWrap(true);
    m_banner->setStyleSheet(
        QStringLiteral("QLabel { background: #1F1F1F; color: #9aa2ad; "
                       "border: 1px solid #262626; border-radius: 10px; "
                       "padding: 16px; }"));
    m_banner->setText(QStringLiteral(
        "No se encontró yt-dlp en el sistema.\nInstálalo con tu gestor de "
        "paquetes (p. ej. \"sudo pacman -S yt-dlp\" o \"pip install yt-dlp\") "
        "y reinicia la app. También puedes definir la ruta del binario en "
        "Ajustes."));
    bodyLayout->addWidget(m_banner);

    outer->addWidget(m_scroll);

    connect(refresh, &QPushButton::clicked, this, &HomePage::reload);
    connect(m_yt, &Yt::trendingFinished, this, &HomePage::onTrending);
    connect(m_yt, &Yt::channelVideosFinished, this, &HomePage::onChannelVideos);

    connect(m_feedGrid, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                emit openRequested(m_feed, index);
            });
    connect(m_trendGrid, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                emit openRequested(m_trending, index);
            });

    m_feedTitle->hide();
    m_feedGrid->hide();
    m_trendTitle->hide();
    m_trendGrid->hide();
    m_banner->hide();
    m_busy->show();
}

void HomePage::setYtAvailable(bool ok)
{
    m_ytAvailable = ok;
    refreshBusy();
}

void HomePage::reload()
{
    loadTrending();
    loadFeed();
}

void HomePage::loadTrending()
{
    m_loadingHome = true;
    refreshBusy();
    m_yt->trending(30);
}

void HomePage::onTrending(const QVector<VideoInfo> &videos)
{
    m_loadingHome = false;
    fprintf(stderr, "E2E trending count=%d\n", (int)videos.size());
    m_trending = videos;
    m_trendGrid->showVideos(m_trending, VideoListView::GridMode);
    adjustGridHeight(m_trendGrid);
    m_trendTitle->show();
    m_trendGrid->show();
    refreshBusy();
}

void HomePage::loadFeed()
{
    const auto channels = m_subs->all();
    m_pendingChannels.clear();
    m_feed.clear();
    m_feedBusy = true;
    if (channels.isEmpty()) {
        m_feedBusy = false;
        m_feedTitle->hide();
        m_feedGrid->hide();
        refreshBusy();
        return;
    }
    for (const SubscriptionInfo &s : channels) {
        m_pendingChannels << s.id;
        if (!s.avatar.isEmpty())
            m_thumbs->requestChannelAvatar(s.id, s.avatar);
    }
    m_pendingIdx = 0;
    refreshBusy();
    requestNextChannel();
}

void HomePage::requestNextChannel()
{
    if (m_pendingIdx < m_pendingChannels.size()) {
        m_yt->channelVideos(m_pendingChannels.at(m_pendingIdx), 10);
    } else {
        m_feedBusy = false;
        onFeedDone();
    }
}

void HomePage::onChannelVideos(const QVector<VideoInfo> &videos)
{
    m_feed += videos;
    m_pendingIdx++;
    requestNextChannel();
}

void HomePage::onFeedDone()
{
    std::stable_sort(m_feed.begin(), m_feed.end(), [](const VideoInfo &a,
                                                      const VideoInfo &b) {
        const QDateTime ad = QDateTime::fromString(a.publishedAt, Qt::ISODate);
        const QDateTime bd = QDateTime::fromString(b.publishedAt, Qt::ISODate);
        if (ad.isValid() && bd.isValid())
            return bd < ad;
        if (ad.isValid() != bd.isValid())
            return ad.isValid();
        return false;
    });
    m_feedGrid->showVideos(m_feed, VideoListView::GridMode);
    adjustGridHeight(m_feedGrid);
    m_feedTitle->setVisible(!m_feed.isEmpty());
    m_feedGrid->setVisible(!m_feed.isEmpty());
    refreshBusy();
}

void HomePage::refreshBusy()
{
    const bool loading = m_loadingHome || m_feedBusy;
    const bool empty = m_trending.isEmpty() && m_feed.isEmpty();
    m_busy->setVisible(loading && empty);
    m_banner->setVisible(!m_ytAvailable && m_trending.isEmpty());
}

void HomePage::adjustGridHeight(VideoListView *grid)
{
    const int count = grid->videos().size();
    if (count == 0) {
        grid->setFixedHeight(kGridH);
        return;
    }
    const int cols = qMax(1, qMax(grid->width(), 1) / kGridW);
    const int rows = (count + cols - 1) / cols;
    grid->setFixedHeight(rows * kGridH);
}

void HomePage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    adjustGridHeight(m_feedGrid);
    adjustGridHeight(m_trendGrid);
}