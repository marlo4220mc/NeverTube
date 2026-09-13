#include "library_page.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLayout>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "../../db/repositories.h"
#include "../../yt.h"
#include "../icons.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

namespace {

const int kFavListH = 320;
const int kPlaylistListH = 258;
const qreal kSubsScale = 1.21;
const qreal kFavScale = 1.21;
const qreal kPlaylistScale = 0.60;

} // namespace

LibraryPage::LibraryPage(Yt *yt, FavoritesRepo *favorites, StarredRepo *starred,
                         WatchLaterRepo *watchLater, HistoryRepo *history,
                         SubscriptionsRepo *subs, VideoCacheRepo *cache,
                         ThumbnailCache *thumbs, PlaylistsRepo *playlists,
                         QWidget *parent)
    : QWidget(parent)
    , m_yt(yt)
    , m_favs(favorites)
    , m_starred(starred)
    , m_watchLater(watchLater)
    , m_history(history)
    , m_subs(subs)
    , m_cache(cache)
    , m_thumbs(thumbs)
    , m_playlists(playlists)
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
    bodyLayout->setSpacing(6);

    auto *title = new QLabel(QStringLiteral("Biblioteca"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    bodyLayout->addWidget(title);
    bodyLayout->addSpacing(14);

    auto *section = new QLabel(QStringLiteral("Suscripciones"), this);
    section->setObjectName(QStringLiteral("sectionTitle"));
    bodyLayout->addWidget(section);

    m_channelHint = new QLabel(
        QStringLiteral("Aún no te has suscrito a ningún canal. Abre un vídeo "
                       "o un canal y pulsa «Suscribirse»."),
        this);
    m_channelHint->setObjectName(QStringLiteral("sectionSub"));
    m_channelHint->setWordWrap(true);
    bodyLayout->addWidget(m_channelHint);

    auto *channelsBox = new QWidget(this);
    m_listLay = new QVBoxLayout(channelsBox);
    m_listLay->setContentsMargins(0, 0, 0, 0);
    m_listLay->setSpacing(4);
    bodyLayout->addWidget(channelsBox);

    m_favTitle = new QLabel(QStringLiteral("Videos que me gustan"), this);
    m_favTitle->setObjectName(QStringLiteral("sectionTitle"));
    bodyLayout->addWidget(m_favTitle);

    m_favList = new VideoListView(m_thumbs, this);
    m_favList->setFixedHeight(kFavListH);
    bodyLayout->addWidget(m_favList);

    m_favEmpty = new QLabel(
        QStringLiteral("Pulsa «Me gusta» en un vídeo y aparecerá aquí."), this);
    m_favEmpty->setObjectName(QStringLiteral("sectionSub"));
    m_favEmpty->setWordWrap(true);
    bodyLayout->addWidget(m_favEmpty);

    m_starTitle = new QLabel(QStringLiteral("Favoritos"), this);
    m_starTitle->setObjectName(QStringLiteral("sectionTitle"));
    bodyLayout->addWidget(m_starTitle);

    m_starList = new VideoListView(m_thumbs, this);
    bodyLayout->addWidget(m_starList);

    m_starEmpty = new QLabel(
        QStringLiteral("Pulsa la estrella en un vídeo para guardarlo aquí."),
        this);
    m_starEmpty->setObjectName(QStringLiteral("sectionSub"));
    m_starEmpty->setWordWrap(true);
    bodyLayout->addWidget(m_starEmpty);

    m_laterTitle = new QLabel(QStringLiteral("Ver más tarde"), this);
    m_laterTitle->setObjectName(QStringLiteral("sectionTitle"));
    bodyLayout->addWidget(m_laterTitle);

    m_laterList = new VideoListView(m_thumbs, this);
    bodyLayout->addWidget(m_laterList);

    m_laterEmpty = new QLabel(
        QStringLiteral("Pulsa el reloj en un vídeo para guardarlo aquí."),
        this);
    m_laterEmpty->setObjectName(QStringLiteral("sectionSub"));
    m_laterEmpty->setWordWrap(true);
    bodyLayout->addWidget(m_laterEmpty);

    auto *historyHeader = new QHBoxLayout;
    m_historyTitle = new QLabel(QStringLiteral("Historial"), this);
    m_historyTitle->setObjectName(QStringLiteral("sectionTitle"));
    historyHeader->addWidget(m_historyTitle);
    historyHeader->addStretch(1);
    m_historyAllBtn = new QPushButton(QStringLiteral("Ver todo"), this);
    m_historyAllBtn->setObjectName(QStringLiteral("ghost"));
    m_historyAllBtn->setCursor(Qt::PointingHandCursor);
    connect(m_historyAllBtn, &QPushButton::clicked, this,
            &LibraryPage::historyAllRequested);
    historyHeader->addWidget(m_historyAllBtn);
    bodyLayout->addLayout(historyHeader);

    m_historyList = new VideoListView(m_thumbs, this);
    bodyLayout->addWidget(m_historyList);

    m_historyEmpty = new QLabel(
        QStringLiteral("Los vídeos que veas aparecerán aquí."), this);
    m_historyEmpty->setObjectName(QStringLiteral("sectionSub"));
    m_historyEmpty->setWordWrap(true);
    bodyLayout->addWidget(m_historyEmpty);

    auto *playlistHeader = new QHBoxLayout;
    auto *playlistTitle = new QLabel(QStringLiteral("Playlists"), this);
    playlistTitle->setObjectName(QStringLiteral("sectionTitle"));
    playlistHeader->addWidget(playlistTitle);
    playlistHeader->addStretch(1);
    auto *newBtn = new QPushButton(QStringLiteral("+ Nueva"), this);
    newBtn->setObjectName(QStringLiteral("ghost"));
    newBtn->setCursor(Qt::PointingHandCursor);
    connect(newBtn, &QPushButton::clicked, this, &LibraryPage::createPlaylist);
    playlistHeader->addWidget(newBtn);
    bodyLayout->addLayout(playlistHeader);

    auto *playlistBox = new QWidget(this);
    m_playlistLay = new QVBoxLayout(playlistBox);
    m_playlistLay->setContentsMargins(0, 0, 0, 0);
    m_playlistLay->setSpacing(4);
    bodyLayout->addWidget(playlistBox);

    bodyLayout->addStretch(1);

    outer->addWidget(m_scroll);

    connect(m_yt, &Yt::channelInfoReady, this, &LibraryPage::onChannelInfo);
    connect(m_favList, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                emit openRequested(m_favVideos, index);
            });
    connect(m_starList, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                emit openRequested(m_starVideos, index);
            });
    connect(m_laterList, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                emit openRequested(m_laterVideos, index);
            });
    connect(m_historyList, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                const qlonglong pos =
                    (index >= 0 && index < m_historyVideos.size())
                        ? m_history->positionOf(
                              m_historyVideos.at(index).id)
                        : 0;
                const VideoInfo &v =
                    (index >= 0 && index < m_historyVideos.size())
                        ? m_historyVideos.at(index)
                        : VideoInfo();
                // Completado → recomenzar desde el inicio.
                emit resumeRequested(m_historyVideos, index,
                                     HistoryRepo::resumeStartMs(v, pos));
            });

    m_favTitle->hide();
    m_favList->hide();
    m_favEmpty->hide();
    m_starTitle->hide();
    m_starList->hide();
    m_starEmpty->hide();
    m_laterTitle->hide();
    m_laterList->hide();
    m_laterEmpty->hide();
    m_historyTitle->hide();
    m_historyList->hide();
    m_historyEmpty->hide();
    m_historyAllBtn->hide();

    reload();
}

void LibraryPage::reload()
{
    for (int i = m_listLay->count() - 1; i >= 0; --i) {
        QLayoutItem *item = m_listLay->takeAt(i);
        if (QLayout *lay = item->layout()) {
            while (QLayoutItem *sub = lay->takeAt(0)) {
                delete sub->widget();
                delete sub;
            }
        }
        delete item->widget();
        delete item;
    }
    rebuild();
    rebuildFavorites();
    rebuildStarred();
    rebuildWatchLater();
    rebuildHistory();
    rebuildPlaylists();
}

void LibraryPage::rebuild()
{
    m_avatarLabels.clear();
    const auto channels = m_subs->all();
    m_channelHint->setVisible(channels.isEmpty());
    if (channels.isEmpty())
        return;
    for (const SubscriptionInfo &s : channels) {
        auto *row = new QHBoxLayout;
        row->setSpacing(12 * kSubsScale);
        auto *avatar = new QLabel(this);
        avatar->setFixedSize(40 * kSubsScale, 40 * kSubsScale);
        avatar->setScaledContents(true);
        avatar->setPixmap(nt::icon(nt::Icon::User, nt::kMuted, 20)
                              .pixmap(40 * kSubsScale, 40 * kSubsScale));
        row->addWidget(avatar, 0, Qt::AlignVCenter);
        const QString avatarKey = QStringLiteral("ch_%1").arg(s.id);
        const QPixmap cached = m_thumbs->pixmap(avatarKey);
        if (!cached.isNull()) {
            avatar->setPixmap(cached);
        } else if (!s.avatar.isEmpty()) {
            connect(m_thumbs, &ThumbnailCache::ready, avatar,
                    [this, avatarKey, avatar](const QString &key) {
                        if (key != avatarKey)
                            return;
                        const QPixmap pm = m_thumbs->pixmap(key);
                        if (!pm.isNull())
                            avatar->setPixmap(pm);
                    });
            m_thumbs->requestChannelAvatar(s.id, s.avatar);
        } else {
            m_avatarLabels.insert(s.id, avatar);
            m_yt->channelInfo(s.id);
        }

        auto *btn = new QPushButton(s.title, this);
        btn->setObjectName(QStringLiteral("ghost"));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setStyleSheet(
            QStringLiteral("QPushButton { background:transparent; border:none;"
                           " border-radius:8px; text-align:left; padding:4px;"
                           " color:#ECECEC; font-weight:600; font-size:13px; }"
                           "QPushButton:hover { background:#1F1F1F; }"));
        connect(btn, &QPushButton::clicked, this,
                [this, s]() { emit channelRequested(s.id, s.title); });
        row->addWidget(btn, 1);

        auto *unsub = new QPushButton(
            nt::icon(nt::Icon::Clear, nt::kMuted, 16), QString(), this);
        unsub->setToolTip(QStringLiteral("Dejar de seguir"));
        unsub->setCursor(Qt::PointingHandCursor);
        unsub->setFixedSize(28, 28);
        unsub->setIconSize(QSize(16, 16));
        unsub->setStyleSheet(
            QStringLiteral("QPushButton { background:transparent; border:none;"
                           " border-radius:14px; }"
                           "QPushButton:hover { background:#262626; }"));
        connect(unsub, &QPushButton::clicked, this, [this, s]() {
            m_subs->remove(s.id);
            reload();
        });
        row->addWidget(unsub, 0, Qt::AlignVCenter);
        m_listLay->addLayout(row);
    }
}

void LibraryPage::rebuildFavorites()
{
    const QStringList ids = m_favs->ids();
    m_favVideos = m_cache->getMany(ids);
    if (m_favVideos.isEmpty()) {
        if (ids.isEmpty()) {
            m_favTitle->hide();
            m_favList->hide();
            m_favEmpty->show();
        } else {
            m_favTitle->show();
            m_favList->show();
            m_favList->showVideos(QVector<VideoInfo>(),
                                  VideoListView::ListMode);
            m_favEmpty->hide();
        }
        return;
    }
    m_favTitle->show();
    m_favList->show();
    m_favEmpty->hide();
    m_favList->setScale(kFavScale);
    const int favH = qMin(m_favVideos.size(), 3) * 86 * kFavScale;
    m_favList->setFixedHeight(favH);
    m_favList->showVideos(m_favVideos, VideoListView::ListMode);
}

void LibraryPage::rebuildStarred()
{
    const QStringList ids = m_starred->ids();
    m_starVideos = m_cache->getMany(ids);
    if (m_starVideos.isEmpty()) {
        m_starTitle->hide();
        m_starList->hide();
        m_starEmpty->show();
        return;
    }
    m_starTitle->show();
    m_starEmpty->hide();
    m_starList->show();
    m_starList->setScale(kFavScale);
    m_starList->setFixedHeight(qMin(m_starVideos.size(), 3) * 86 * kFavScale);
    m_starList->showVideos(m_starVideos, VideoListView::ListMode);
}

void LibraryPage::rebuildWatchLater()
{
    const QStringList ids = m_watchLater->ids();
    m_laterVideos = m_cache->getMany(ids);
    if (m_laterVideos.isEmpty()) {
        m_laterTitle->hide();
        m_laterList->hide();
        m_laterEmpty->show();
        return;
    }
    m_laterTitle->show();
    m_laterEmpty->hide();
    m_laterList->show();
    m_laterList->setScale(kFavScale);
    m_laterList->setFixedHeight(qMin(m_laterVideos.size(), 3) * 86 * kFavScale);
    m_laterList->showVideos(m_laterVideos, VideoListView::ListMode);
}

void LibraryPage::rebuildHistory()
{
    const QStringList ids = m_history->recentIds(10);
    m_historyVideos = m_cache->getMany(ids);
    if (m_historyVideos.isEmpty()) {
        m_historyTitle->hide();
        m_historyList->hide();
        m_historyEmpty->show();
        m_historyAllBtn->hide();
        return;
    }
    m_historyTitle->show();
    m_historyEmpty->hide();
    m_historyList->show();
    m_historyAllBtn->show();
    m_historyList->setScale(kFavScale);
    m_historyList->setFixedHeight(
        qMin(m_historyVideos.size(), 3) * 86 * kFavScale);
    m_historyList->showVideos(m_historyVideos, VideoListView::ListMode);
    for (const VideoInfo &v : m_historyVideos) {
        const qlonglong pos = m_history->positionOf(v.id);
        if (pos <= 0 || v.durationSeconds <= 0)
            continue;
        if (HistoryRepo::isCompleted(v, pos))
            continue;
        const double frac =
            double(pos) / (double(v.durationSeconds) * 1000.0);
        if (frac > 0.0 && frac < 1.0)
            m_historyList->setProgress(v.id, frac);
    }
}

void LibraryPage::onChannelInfo(const ChannelInfo &channel)
{
    QLabel *avatar = m_avatarLabels.value(channel.id);
    if (!avatar || channel.avatarUrl.isEmpty())
        return;
    m_subs->setAvatar(channel.id, channel.avatarUrl);
    m_avatarLabels.remove(channel.id);
    const QString avatarKey = QStringLiteral("ch_%1").arg(channel.id);
    const QPixmap cached = m_thumbs->pixmap(avatarKey);
    if (!cached.isNull()) {
        avatar->setPixmap(cached);
        return;
    }
    connect(m_thumbs, &ThumbnailCache::ready, avatar,
            [this, avatarKey, avatar](const QString &key) {
                if (key != avatarKey)
                    return;
                const QPixmap pm = m_thumbs->pixmap(key);
                if (!pm.isNull())
                    avatar->setPixmap(pm);
            });
    m_thumbs->requestChannelAvatar(channel.id, channel.avatarUrl);
}

void LibraryPage::rebuildPlaylists()
{
    if (!m_playlistLay)
        return;
    while (QLayoutItem *item = m_playlistLay->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const auto playlists = m_playlists->all();
    for (const PlaylistItem &p : playlists) {
        const QStringList ids = m_playlists->videoIds(p.id);
        const QVector<VideoInfo> videos = m_cache->getMany(ids);

        auto *btn = new QPushButton(this);
        btn->setObjectName(QStringLiteral("ghost"));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setText(QStringLiteral("%1  (%2)").arg(p.title).arg(videos.size()));
        btn->setStyleSheet(
            QStringLiteral("QPushButton { background:transparent; border:none;"
                           " border-radius:8px; text-align:left; padding:4px;"
                           " color:#ECECEC; font-weight:600; font-size:13px; }"
                           "QPushButton:hover { background:#1F1F1F; }"));
        connect(btn, &QPushButton::clicked, this,
                [this, p]() { emit playlistRequested(p.id, p.title); });
        m_playlistLay->addWidget(btn);

        if (videos.isEmpty())
            continue;

        auto *preview = new VideoListView(m_thumbs, this);
        preview->setScale(kPlaylistScale);
        preview->showVideos(videos, VideoListView::GridMode);
        preview->setFixedHeight(kPlaylistListH);
        connect(preview, &VideoListView::videoActivated, this,
                [this, videos](int index, const VideoInfo &) {
                    emit openRequested(videos, index);
                });
        m_playlistLay->addWidget(preview);
    }
}

void LibraryPage::createPlaylist()
{
    bool ok = false;
    const QString title =
        QInputDialog::getText(this, QStringLiteral("Nueva playlist"),
                              QStringLiteral("Nombre:"), QLineEdit::Normal,
                              QString(), &ok);
    if (!ok || title.trimmed().isEmpty())
        return;
    if (!m_playlists->create(title.trimmed()).isEmpty())
        rebuildPlaylists();
}