#include "home_page.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include "../../debug.h"
#include "../../db/repositories.h"
#include "../../i18n.h"
#include "../../models.h"
#include "../../yt.h"
#include "../icons.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

namespace {

const int kGridW = 320;
const int kGridH = 258;

// Height reserved for the mini player (63 px thumbnail + margins/progress).
// It is the extra bottom space added to the scrollable content while the mini
// player is visible, so the last section stays above it.
const int kMiniBarReservedPx = 96;

// Subscriptions feed harvest per channel: maximum number of channels queried
// per refresh and the number of videos per channel (5) after which a given
// channel stops being requested. Each channel contributes up to 5 recents.
const int kFeedMaxChannels = 6;
const int kFeedPerChannel = 5;
const int kFeedTargetVideos = kFeedMaxChannels * kFeedPerChannel;

// Cache TTLs per section (ms). TTL != polling: on entering Home the cached
// content is shown immediately and, if expired, refreshed ONCE in the
// background.
const qint64 kFeedTtlMs = 30 * 60 * 1000;
const qint64 kHomeTtlMs = 60 * 60 * 1000;
const qint64 kTrendTtlMs = 2 * 60 * 60 * 1000;
const qint64 kDiscoverTtlMs = 60 * 60 * 1000;
// How often the /next re-checks whether a "Continue watching" live stream is
// still on air (cheap and without touching the player phase). Same as in
// Library.
const qint64 kLiveRecheckMs = 10 * 60 * 1000;
// Backoff for Discover's initialization generation (when no radios are
// saved): after a fruitless attempt it waits before retrying, so repeatedly
// opening Home offline doesn't hammer the network.
const qint64 kDiscoverRetryMs = 10 * 60 * 1000;
// Distinct seed radios that are attempted to be saved during the
// initialization generation (exception: only happens when there is no usable
// radio).
const int kDiscoverMinRadios = 3;

// Keys of the NETWORK sections in home_cache (HomeCacheRepo). Continue
// watching and Discover are NOT stored there: they derive from history/radios,
// which are already persistent, and Discover always re-selects at startup.
const char kCacheSecHome[] = "recommended";
const char kCacheSecFeed[] = "feed";
const char kCacheSecTrend[] = "trending";

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

// Valid entries: with id and title (more robust against degraded Innertube
// responses: videos without id, broken playlists sneaking in).
QVector<VideoInfo> sanitizeVideos(const QVector<VideoInfo> &in)
{
    QVector<VideoInfo> out;
    QSet<QString> seen;
    for (const VideoInfo &v : in) {
        if (v.id.isEmpty() || v.id.size() != 11 || seen.contains(v.id))
            continue;
        seen.insert(v.id);
        out.append(v);
    }
    return out;
}

} // namespace

HomePage::HomePage(Yt *yt, SubscriptionsRepo *subs, HistoryRepo *history,
                   DiscoverRepo *discover, VideoCacheRepo *videoCache,
                   ThumbnailCache *thumbs, SettingsRepo *settings,
                   HomeCacheRepo *homeCache, QWidget *parent)
    : QWidget(parent)
    , m_yt(yt)
    , m_subs(subs)
    , m_history(history)
    , m_discover(discover)
    , m_videoCache(videoCache)
    , m_thumbs(thumbs)
    , m_settings(settings)
    , m_homeCache(homeCache)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_body = new QWidget;
    m_scroll->setWidget(m_body);

    auto *bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout = bodyLayout;
    bodyLayout->setContentsMargins(14, 14, 14, 20);
    bodyLayout->setSpacing(6);

    auto *topRow = new QHBoxLayout;
    auto *title = new QLabel(this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    nt::bindText(title, "home.title");
    auto *refresh = new QPushButton(this);
    refresh->setObjectName(QStringLiteral("ghost"));
    nt::bindText(refresh, "home.refresh");
    refresh->setIcon(nt::icon(nt::Icon::Refresh, nt::kText, 15));
    topRow->addWidget(title);
    topRow->addStretch(1);
    topRow->addWidget(refresh);
    bodyLayout->addLayout(topRow);

    m_busy = new QLabel(this);
    m_busy->setObjectName(QStringLiteral("sectionSub"));
    nt::bindText(m_busy, "common.loading");
    m_busy->setAlignment(Qt::AlignCenter);
    m_busy->setContentsMargins(0, 20, 0, 20);
    bodyLayout->addWidget(m_busy);

    // ▶ Continue watching
    m_contTitle = new QLabel(this);
    m_contTitle->setObjectName(QStringLiteral("sectionTitle"));
    nt::bindText(m_contTitle, "home.continue");
    m_contGrid = new VideoListView(m_thumbs, this);
    m_contGrid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_contGrid->setFixedHeight(0);
    bodyLayout->addWidget(m_contTitle);
    bodyLayout->addWidget(m_contGrid);

    // 🎲 Discover
    auto *discRow = new QHBoxLayout;
    discRow->setSpacing(4);
    m_discTitle = new QLabel(this);
    m_discTitle->setObjectName(QStringLiteral("sectionTitle"));
    nt::bindText(m_discTitle, "home.discover");
    discRow->addWidget(m_discTitle);
    discRow->addStretch(1);
    m_discMoreBtn = new QPushButton(this);
    m_discMoreBtn->setObjectName(QStringLiteral("ghost"));
    nt::bindText(m_discMoreBtn, "home.another");
    discRow->addWidget(m_discMoreBtn);
    bodyLayout->addLayout(discRow);
    m_discGrid = new VideoListView(m_thumbs, this);
    m_discGrid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_discGrid->setFixedHeight(0);
    bodyLayout->addWidget(m_discGrid);

    // 🔔 New from your subscriptions
    m_feedTitle = new QLabel(this);
    m_feedTitle->setObjectName(QStringLiteral("sectionTitle"));
    nt::bindText(m_feedTitle, "home.feed");
    m_feedGrid = new VideoListView(m_thumbs, this);
    m_feedGrid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_feedGrid->setFixedHeight(0);
    bodyLayout->addWidget(m_feedTitle);
    bodyLayout->addWidget(m_feedGrid);

    // ✨ Recommended by YouTube
    m_homeTitle = new QLabel(this);
    m_homeTitle->setObjectName(QStringLiteral("sectionTitle"));
    nt::bindText(m_homeTitle, "home.recommended");
    m_homeGrid = new VideoListView(m_thumbs, this);
    m_homeGrid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_homeGrid->setFixedHeight(0);
    bodyLayout->addWidget(m_homeTitle);
    bodyLayout->addWidget(m_homeGrid);

    // 🔥 Trending
    m_trendTitle = new QLabel(this);
    m_trendTitle->setObjectName(QStringLiteral("sectionTitle"));
    nt::bindText(m_trendTitle, "home.trending");
    m_trendGrid = new VideoListView(m_thumbs, this);
    m_trendGrid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_trendGrid->setFixedHeight(0);
    bodyLayout->addWidget(m_trendTitle);
    bodyLayout->addWidget(m_trendGrid);

    outer->addWidget(m_scroll);

    // Home sections fill the whole available width (stretchy cells) with the
    // videos per row set by the "Thumbnail size" setting (4/5/6, default 5);
    // their height derives from each grid's real width.
    for (VideoListView *g : {m_contGrid, m_feedGrid, m_homeGrid,
                             m_discGrid, m_trendGrid}) {
        g->setGridFillWidth(kGridW);
        g->setGridColumns(m_cellColumns);
    }

    // Timer to recalculate grid heights after the initial layout and when
    // videos are filled in asynchronously
    m_layoutTimer = new QTimer(this);
    m_layoutTimer->setSingleShot(true);
    m_layoutTimer->setInterval(50);
    connect(m_layoutTimer, &QTimer::timeout, this,
            &HomePage::applyGridSizes);

    connect(refresh, &QPushButton::clicked, this, &HomePage::reload);
    connect(m_discMoreBtn, &QPushButton::clicked, this,
            &HomePage::onDiscoveryAgain);
    connect(m_yt, &Yt::trendingFinished, this, &HomePage::onTrending);
    connect(m_yt, &Yt::channelVideosFinished, this, &HomePage::onChannelVideos);
    connect(m_yt, &Yt::homeReady, this, &HomePage::onHomeReady);
    connect(m_yt, &Yt::radioReady, this, &HomePage::onRadioReady);
    // Every playback writes to the history (PlayerController::playIndex →
    // HistoryRepo::add). Without this connection, going back to Home after
    // playing a video doesn't refresh "Continue watching" nor hide the
    // empty-state.
    connect(m_history, &HistoryRepo::changed, this,
            &HomePage::refreshContinueWatching);
    connect(m_history, &HistoryRepo::changed, this, &HomePage::refreshBusy);

    // Live re-check for "Continue watching": the /next distinguishes "still
    // live" from "ended" (viewCountText "viéndolo ahora" vs
    // "visualizaciones"). Once it ends, the row becomes duration + progress
    // bar.
    connect(m_yt, &Yt::detailReady, this,
            [this](const VideoInfo &video, const QString &, qint64,
                   const QVector<FormatInfo> &) { onLiveDetailReady(video); });
    connect(m_yt, &Yt::detailFailed, this, [this](const QString &id) {
        if (m_liveRefreshPending.remove(id))
            m_liveRefreshedAt.insert(id, QDateTime::currentMSecsSinceEpoch());
    });

    connect(m_contGrid, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                if (index >= 0 && index < m_continuePositions.size())
                    emit resumeRequested(m_continueVideos, index,
                                         m_continuePositions.at(index));
                else
                    emit openRequested(m_continueVideos, index);
            });
    connect(m_feedGrid, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                emit openRequested(m_feed, index);
            });
    connect(m_homeGrid, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                emit openRequested(m_home, index);
            });
    connect(m_discGrid, &VideoListView::videoActivated, this,
            [this](int, const VideoInfo &video) {
                if (!video.id.isEmpty())
                    emit openRequested(QVector<VideoInfo>{video}, 0);
            });
    connect(m_trendGrid, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                emit openRequested(m_trending, index);
            });

    m_contTitle->hide();
    m_contGrid->hide();
    m_feedTitle->hide();
    m_feedGrid->hide();
    m_homeTitle->hide();
    m_homeGrid->hide();
    m_discTitle->hide();
    m_discMoreBtn->hide();
    m_discGrid->hide();
    m_trendTitle->hide();
    m_trendGrid->hide();
    m_busy->show();

    buildEmptyState();

    loadPersistedSections();
}

bool HomePage::cachedFresh(qint64 at, qint64 ttlMs) const
{
    return at > 0 && (nowMs() - at) < ttlMs;
}

// "Empty" panel: shown when there is NO user content (no history, no
// subscriptions, no Discover radios). In that case the app starts unable to
// offer Continue watching / Discover / New from your subscriptions, and the
// panel guides the user instead of leaving them staring at a Home with only
// "Recommended" and "Trending". It hides as soon as any of those sections
// appears (refreshBusy controls it).
void HomePage::buildEmptyState()
{
    auto *emptyBox = new QFrame(m_body);
    // Styled by the global QSS (QFrame#popupDialog), like the app's other
    // four popups. It used to also carry an inline rule that lowered it to
    // kPanel (#181818) on top of the rule's kPopover (#202020): it was the
    // only popup with that background, it looked like drift and not a
    // decision.
    emptyBox->setObjectName(QStringLiteral("popupDialog"));
    auto *emptyLay = new QVBoxLayout(emptyBox);
    emptyLay->setContentsMargins(28, 24, 28, 24);
    emptyLay->setSpacing(8);
    // App logo (brand) above the title: gives the empty state an identity
    // instead of a mute box. If the resource is missing, it's simply skipped.
    if (const QPixmap logo = nt::appLogo(72); !logo.isNull()) {
        auto *logoLabel = new QLabel(emptyBox);
        logoLabel->setObjectName(QStringLiteral("emptyStateLogo"));
        logoLabel->setPixmap(logo);
        logoLabel->setAlignment(Qt::AlignCenter);
        emptyLay->addWidget(logoLabel);
    }
    auto *emptyTitle = new QLabel(emptyBox);
    emptyTitle->setObjectName(QStringLiteral("emptyStateTitle"));
    emptyTitle->setAlignment(Qt::AlignCenter);
    nt::bindText(emptyTitle, "home.emptyTitle");
    auto *emptyBody = new QLabel(emptyBox);
    nt::bindText(emptyBody, "home.emptyBody");
    emptyBody->setObjectName(QStringLiteral("emptyStateBody"));
    emptyBody->setWordWrap(true);
    emptyBody->setAlignment(Qt::AlignCenter);
    emptyLay->addWidget(emptyTitle);
    emptyLay->addWidget(emptyBody);
    auto *emptyRow = new QHBoxLayout;
    emptyRow->setSpacing(8);
    emptyRow->addStretch(1);
    auto *goSearch = new QPushButton(emptyBox);
    nt::bindText(goSearch, "home.goSearch");
    goSearch->setCursor(Qt::PointingHandCursor);
    goSearch->setObjectName(QStringLiteral("accentChip"));
    connect(goSearch, &QPushButton::clicked, this,
            [this]() { emit navigateToSearchRequested(); });
    emptyRow->addWidget(goSearch);
    emptyRow->addStretch(1);
    emptyLay->addLayout(emptyRow);
    // It is inserted into bodyLayout JUST BEFORE "Continue watching". If that
    // section is hidden, the panel takes its place and, when it closes, the
    // sections regroup naturally with the layout's spacing.
    if (!m_bodyLayout)
        return;
    int idx = m_bodyLayout->indexOf(m_contTitle);
    if (idx < 0)
        idx = m_bodyLayout->count();
    m_bodyLayout->insertWidget(idx, emptyBox);
    m_emptyState = emptyBox;
}


// At startup the persisted Home is loaded to show it instantly:
// Recommended/Feed/Trending come from home_cache (metadata resolved through
// the "videos" cache); its loaded fetched_at later decides, in
// loadSections(), whether the section needs a refresh (only an expired one is
// re-requested).
void HomePage::loadPersistedSections()
{
    if (!m_homeCache)
        return;
    auto load = [this](const char *section, QVector<VideoInfo> &out,
                       qint64 &fetchedAt) {
        const QStringList ids = m_homeCache->sectionVideoIds(
            QLatin1String(section));
        if (ids.isEmpty())
            return;
        const QVector<VideoInfo> videos = m_videoCache->getMany(ids);
        if (videos.isEmpty())
            return;
        const qint64 at = m_homeCache->sectionFetchedAt(
            QLatin1String(section));
        if (at <= 0)
            return;
        out = videos;
        fetchedAt = at;
    };
    load(kCacheSecHome, m_home, m_homeFetchedAt);
    load(kCacheSecFeed, m_feed, m_feedFetchedAt);
    load(kCacheSecTrend, m_trending, m_trendFetchedAt);
    if (!m_home.isEmpty()) {
        m_homeGrid->showVideos(m_home, VideoListView::GridMode);
        adjustGridHeight(m_homeGrid);
        m_homeTitle->setVisible(true);
        m_homeGrid->setVisible(true);
    }
    if (!m_feed.isEmpty()) {
        m_feedGrid->showVideos(m_feed, VideoListView::GridMode);
        adjustGridHeight(m_feedGrid);
        m_feedTitle->setVisible(true);
        m_feedGrid->setVisible(true);
    }
    if (!m_trending.isEmpty()) {
        m_trendGrid->showVideos(m_trending, VideoListView::GridMode);
        adjustGridHeight(m_trendGrid);
        m_trendTitle->setVisible(true);
        m_trendGrid->setVisible(true);
    }
    if (!m_home.isEmpty() || !m_feed.isEmpty() || !m_trending.isEmpty())
        m_layoutTimer->start();
}

void HomePage::saveSection(const QString &section,
                           const QVector<VideoInfo> &videos)
{
    if (!m_homeCache || videos.isEmpty())
        return;
    // Full metadata goes to "videos" (VideoCacheRepo): home_cache only stores
    // ids + timestamp, so on reopening the app getMany(ids) rebuilds the
    // section even on a fresh install where nothing has ever been played.
    for (const VideoInfo &v : videos)
        m_videoCache->upsert(v);
    QStringList ids;
    ids.reserve(videos.size());
    for (const VideoInfo &v : videos)
        ids << v.id;
    m_homeCache->saveSection(section, ids, nowMs());
}

void HomePage::setCurrentVideoId(const QString &id)
{
    m_currentVideoId = id;
}

void HomePage::reload()
{
    loadContinueWatching();
    loadSections();
}

void HomePage::refreshContinueWatching()
{
    // The history is written every kPositionFlushMs (20 s) while playing, and
    // this slot is connected to its changed(). Rebuilding the grid with the
    // page HIDDEN (playback goes to the mini player, the normal case) is
    // wasted work: on entering Home, onNavButton calls refreshDynamic() and
    // only then is it rebuilt with fresh data.
    if (!isVisible())
        return;
    loadContinueWatching();
}

void HomePage::refreshDynamic()
{
    loadContinueWatching();
    maybeInvalidateFeed();
    buildDiscoverSelection();
}

// Re-applies the page accent. The empty-state button no longer carries its
// own stylesheet: it is QPushButton#accentChip, and nt::styleSheet() is fully
// regenerated on every accent change, so the button repaints itself and there
// is nothing left to do here. The method is kept because MainWindow calls it
// on all four pages alike.
void HomePage::applyAccent()
{
}

QString HomePage::currentSubsSignature() const
{
    const auto channels = m_subs->all();
    QStringList ids;
    ids.reserve(channels.size());
    for (const SubscriptionInfo &s : channels)
        ids << s.id;
    ids.sort();
    return ids.join(QLatin1Char(','));
}

void HomePage::maybeInvalidateFeed()
{
    if (!m_settings)
        return;
    const QString sig = currentSubsSignature();
    if (sig == m_settings->getString(QStringLiteral("feed_subs_sig")))
        return;
    NT_DEBUG("E2E feed subs cambiaron: invalidando caché\n");
    m_feedFetchedAt = 0;
    if (!m_feedBusy)
        loadFeed();
}

// 1) ▶ Continue watching: local history, no network. Progress between 0 and
//    100%, with known duration, ordered by last played (max 4).
void HomePage::loadContinueWatching()
{
    m_continueVideos.clear();
    m_continuePositions.clear();
    const QStringList recent = m_history->recentIds(50);
    QVector<VideoInfo> out;
    QVector<QPair<QString, double>> progress;
    for (const QString &id : recent) {
        if (out.size() >= 4)
            break;
        const qlonglong pos = m_history->positionOf(id);
        if (pos <= 0)
            continue;
        VideoInfo v;
        if (!m_videoCache->get(id, v))
            continue;
        const double frac = HistoryRepo::progressFraction(v, pos);
        // A live stream is included even without a fraction (zero duration or
        // position stuck at the edge): it shows the "En directo" badge instead
        // of the bar. Its status is re-checked on re-entry
        // (maybeRefreshLiveStatus) and, once it ended, it becomes a normal
        // video with a bar again.
        if (frac <= 0.0 && !v.isLive)
            continue;
        out.append(v);
        m_continuePositions.append(pos);
        if (frac > 0.0)
            progress.append({id, frac});
        if (v.isLive)
            maybeRefreshLiveStatus(v);
    }
    m_continueVideos = out;
    m_contGrid->showVideos(m_continueVideos, VideoListView::GridMode);
    for (const auto &p : progress)
        m_contGrid->setProgress(p.first, p.second);
    adjustGridHeight(m_contGrid);
    m_contTitle->setVisible(!out.isEmpty());
    m_contGrid->setVisible(!out.isEmpty());
}

// A cached live stream may end between visits. Re-parsing the renderers would
// only clarify it when that surface is requested again; here, on re-entering
// Home, the /next (getDetail: does NOT touch the player phase) re-checks the
// flag of each live stream in "Continue watching". The /next distinguishes
// "still live" ("N usuarios viéndolo ahora") from "ended" ("N
// visualizaciones"); the TOTAL duration of an ended live stream is only given
// by the player response on playback, so on ending the last known duration
// is kept.
void HomePage::maybeRefreshLiveStatus(const VideoInfo &v)
{
    if (!v.isLive || m_liveRefreshPending.contains(v.id))
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_liveRefreshedAt.value(v.id, 0) < kLiveRecheckMs)
        return;
    m_liveRefreshPending.insert(v.id);
    m_yt->getDetail(v.id);
}

void HomePage::onLiveDetailReady(const VideoInfo &video)
{
    if (!m_liveRefreshPending.remove(video.id))
        return;
    m_liveRefreshedAt.insert(video.id, QDateTime::currentMSecsSinceEpoch());
    if (video.isLive)
        return; // still on air: re-checked in 10 minutes
    // It ended: the flag is turned off and "Continue watching" repaints it
    // as a normal video (duration + the user's position bar).
    if (m_videoCache->updateLiveStatus(video.id, false))
        loadContinueWatching();
}

// Loads/refreshes all network sections. Each one: if the cache is fresh it
// requests nothing; if expired, it requests ONCE (single-flight via m_*Busy)
// in the background while the cached content is shown.
void HomePage::loadSections()
{
    // 2) 🔔 subscriptions
    maybeInvalidateFeed();
    if (!cachedFresh(m_feedFetchedAt, kFeedTtlMs) && !m_feedBusy)
        loadFeed();

    // 3) ✨ Recommended by YouTube
    if (!cachedFresh(m_homeFetchedAt, kHomeTtlMs) && !m_homeBusy) {
        m_homeBusy = true;
        NT_DEBUG("E2E home fetchHome\n");
        m_yt->home();
    }

    // 🎲 Discover (only when there is a valid seed)
    ensureDiscover();

    // 5) 🔥 Trending
    if (!cachedFresh(m_trendFetchedAt, kTrendTtlMs) && !m_trendBusy) {
        m_trendBusy = true;
        m_yt->trending(8);
    }

    refreshBusy();
}

void HomePage::loadFeed()
{
    const auto channels = m_subs->all();
    m_feedSubsSig = currentSubsSignature();
    m_pendingChannels.clear();
    // The section currently shown is kept: if the refresh ends up
    // empty/failed, onFeedDone restores it instead of hiding it or replacing
    // it with an empty one.
    m_prevFeed = m_feed;
    m_feed.clear();
    m_feedBusy = true;
    if (channels.isEmpty()) {
        m_feedBusy = false;
        m_feedTitle->hide();
        m_feedGrid->hide();
        m_feedFetchedAt = nowMs();
        if (m_settings)
            m_settings->set(QStringLiteral("feed_subs_sig"), m_feedSubsSig);
        refreshBusy();
        return;
    }
    m_channelNames.clear();
    for (const SubscriptionInfo &s : channels) {
        m_channelNames.insert(s.id, s.title);
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
    if (m_pendingIdx >= m_pendingChannels.size()
        || m_pendingIdx >= kFeedMaxChannels
        || m_feed.size() >= kFeedTargetVideos) {
        m_feedBusy = false;
        onFeedDone();
    } else {
        m_yt->channelVideos(m_pendingChannels.at(m_pendingIdx), 10);
    }
}

void HomePage::onChannelVideos(const QVector<VideoInfo> &videos)
{
    QVector<VideoInfo> patched = videos;
    // The channel's "Videos" tab items carry no channel name: it is filled
    // in from the requested subscription (Innertube).
    const QString cid = m_pendingIdx < m_pendingChannels.size()
                            ? m_pendingChannels.at(m_pendingIdx)
                            : QString();
    for (VideoInfo &v : patched) {
        if (v.id.isEmpty())
            continue;
        if (v.channel.isEmpty() && !cid.isEmpty())
            v.channel = m_channelNames.value(cid);
        if (v.channelId.isEmpty())
            v.channelId = cid;
    }
    // Up to 5 recents per channel, sorting that channel by date.
    std::stable_sort(patched.begin(), patched.end(), [](const VideoInfo &a,
                                                        const VideoInfo &b) {
        const QDateTime ad = QDateTime::fromString(a.publishedAt, Qt::ISODate);
        const QDateTime bd = QDateTime::fromString(b.publishedAt, Qt::ISODate);
        if (ad.isValid() && bd.isValid())
            return bd < ad;
        if (ad.isValid() != bd.isValid())
            return ad.isValid();
        return false;
    });
    if (patched.size() > kFeedPerChannel)
        patched.resize(kFeedPerChannel);
    m_feed += patched;
    m_pendingIdx++;
    requestNextChannel();
}

void HomePage::onFeedDone()
{
    m_feedBusy = false;
    QVector<VideoInfo> fresh = sanitizeVideos(m_feed);
    if (fresh.isEmpty()) {
        // Empty/failed refresh: the previously cached section is kept (memory
        // and UI), not replaced by an empty one nor fetched_at refreshed.
        m_feed = m_prevFeed;
        m_prevFeed.clear();
        NT_DEBUG("E2E feed vacío: se conserva la sección anterior n=%d\n",
                 (int)m_feed.size());
        refreshBusy();
        ensureDiscover();
        return;
    }
    std::stable_sort(fresh.begin(), fresh.end(), [](const VideoInfo &a,
                                                    const VideoInfo &b) {
        const QDateTime ad = QDateTime::fromString(a.publishedAt, Qt::ISODate);
        const QDateTime bd = QDateTime::fromString(b.publishedAt, Qt::ISODate);
        if (ad.isValid() && bd.isValid())
            return bd < ad;
        if (ad.isValid() != bd.isValid())
            return ad.isValid();
        return false;
    });
    if (fresh.size() > kFeedTargetVideos)
        fresh.resize(kFeedTargetVideos);
    m_feed = fresh;
    m_prevFeed.clear();
    m_feedFetchedAt = nowMs();
    saveSection(QLatin1String(kCacheSecFeed), m_feed);
    if (m_settings)
        m_settings->set(QStringLiteral("feed_subs_sig"), m_feedSubsSig);
    m_feedGrid->showVideos(m_feed, VideoListView::GridMode);
    adjustGridHeight(m_feedGrid);
    const bool vis = !m_feed.isEmpty();
    m_feedTitle->setVisible(vis);
    m_feedGrid->setVisible(vis);
    NT_DEBUG("E2E feed setVisible=%d height=%d visible=%d\n", vis, m_feedGrid->height(), m_feedGrid->isVisible());
    // Recalculate heights after the layout settles
    m_layoutTimer->start();
    refreshBusy();
    ensureDiscover();
}

void HomePage::onHomeReady(const QVector<VideoInfo> &videos)
{
    NT_DEBUG("E2E home ready count=%d\n", (int)videos.size());
    m_homeBusy = false;
    const QVector<VideoInfo> fresh = sanitizeVideos(videos);
    if (!fresh.isEmpty()) {
        m_home = fresh;
        m_homeFetchedAt = nowMs();
        saveSection(QLatin1String(kCacheSecHome), m_home);
        m_homeGrid->showVideos(m_home, VideoListView::GridMode);
        adjustGridHeight(m_homeGrid);
        const bool vis = !m_home.isEmpty();
        m_homeTitle->setVisible(vis);
        m_homeGrid->setVisible(vis);
        NT_DEBUG("E2E home setVisible=%d height=%d visible=%d\n", vis, m_homeGrid->height(), m_homeGrid->isVisible());
        // Recalculate heights after the layout settles
        m_layoutTimer->start();
    } else {
        // Failed/empty refresh: the previously cached section is kept (memory
        // and UI), without replacing it with an empty one nor touching
        // fetched_at.
        NT_DEBUG("E2E home vacío: se conserva la sección anterior n=%d\n",
                 (int)m_home.size());
    }
    refreshBusy();
    ensureDiscover();
}

void HomePage::onTrending(const QVector<VideoInfo> &videos)
{
    NT_DEBUG("E2E trending count=%d\n", (int)videos.size());
    if (!videos.isEmpty()) {
        NT_DEBUG("E2E trending first id='%s' size=%d title='%s'\n", 
                 videos[0].id.toUtf8().constData(), videos[0].id.size(),
                 videos[0].title.left(40).toUtf8().constData());
    }
    m_trendBusy = false;
    QVector<VideoInfo> fresh = sanitizeVideos(videos);
    NT_DEBUG("E2E trending after sanitize=%d\n", (int)fresh.size());
    if (!fresh.isEmpty()) {
        if (fresh.size() > 8)
            fresh.resize(8);
        m_trending = fresh;
        m_trendFetchedAt = nowMs();
        saveSection(QLatin1String(kCacheSecTrend), m_trending);
        m_trendGrid->showVideos(m_trending, VideoListView::GridMode);
        adjustGridHeight(m_trendGrid);
        const bool vis = !m_trending.isEmpty();
        m_trendTitle->setVisible(vis);
        m_trendGrid->setVisible(vis);
        NT_DEBUG("E2E trending setVisible=%d height=%d visible=%d\n", vis, m_trendGrid->height(), m_trendGrid->isVisible());
        m_layoutTimer->start();
    } else {
        // Failed/empty refresh: the previously cached section is kept (memory
        // and UI), without replacing it with an empty one nor touching
        // fetched_at.
        NT_DEBUG("E2E trending vacío: se conserva la sección anterior n=%d\n",
                 (int)m_trending.size());
    }
    refreshBusy();
    ensureDiscover();
}

// 4) 🎲 Discover: PERSISTENT source (SQLite). Opening Home NEVER generates
// radios by itself: if the in-memory cache expired, up to 5 videos are
// re-selected from the ALREADY SAVED radios (pickDiscoverVideos, no network).
// Radios are saved when videos are played (MainWindow.onRadioReadyDiscover)
// and, as an initialization EXCEPTION, when no usable radio exists the
// section itself generates them from available seeds (history/recommended/
// subscriptions/trending) and saves them before consuming them.
void HomePage::ensureDiscover()
{
    if (m_discoverBusy)
        return;
    if (!m_discoverVideos.isEmpty()
        && cachedFresh(m_discoverFetchedAt, kDiscoverTtlMs)) {
        showDiscover();
        return;
    }
    buildDiscoverSelection();
}

// Discover selection consuming ONLY the saved radios. Radios without usable
// videos are discarded by the pick itself. If none remain, the initialization
// generation is triggered (with backoff), the only path where Home touches
// radioVideos() and only the first time the app is seeded (see
// discoverSeeded).
void HomePage::buildDiscoverSelection()
{
    m_discoverVideos = pickDiscoverVideos(*m_discover, *m_videoCache,
                                          m_currentVideoId);
    m_discoverFetchedAt = nowMs();
    m_discoverEmptyAt = 0;
    showDiscover();
    if (m_discoverVideos.isEmpty() && m_discover->count() == 0 && m_yt
        && !discoverSeeded()
        && !cachedFresh(m_discoverEmptyAt, kDiscoverRetryMs)) {
        startDiscoverSeedGen();
    } else if (m_discoverVideos.isEmpty()) {
        m_discoverEmptyAt = nowMs();
    }
}

bool HomePage::discoverSeeded() const
{
    return m_settings
        && m_settings->getInt(QStringLiteral("discover_seeded"), 0) == 1;
}

void HomePage::markDiscoverSeeded()
{
    if (m_settings)
        m_settings->set(QStringLiteral("discover_seeded"),
                        QStringLiteral("1"));
}

// Seeds for the radio generation of the initialization exception, varied and
// shuffled: recent history (reuses the local metadata cache), recommended,
// subscriptions and trending. The video being played and duplicates are
// excluded.
void HomePage::buildDiscoverSeeds()
{
    m_discoverSeeds.clear();
    QSet<QString> seen;
    auto addVideos = [&](const QVector<VideoInfo> &list) {
        for (const VideoInfo &v : list) {
            if (v.id.size() != 11 || v.id == m_currentVideoId
                || seen.contains(v.id))
                continue;
            seen.insert(v.id);
            m_discoverSeeds << v.id;
        }
    };
    addVideos(m_home);
    addVideos(m_feed);
    addVideos(m_trending);
    const QStringList recent = m_history->recentIds(20);
    for (const QString &id : recent) {
        if (seen.contains(id) || id == m_currentVideoId)
            continue;
        VideoInfo v;
        if (!m_videoCache->get(id, v) || v.id.size() != 11)
            continue;
        seen.insert(id);
        m_discoverSeeds << v.id;
    }
    for (int i = m_discoverSeeds.size() - 1; i > 0; --i) {
        const int j = QRandomGenerator::global()->bounded(i + 1);
        m_discoverSeeds.swapItemsAt(i, j);
    }
    NT_DEBUG("E2E discover gen seeds=%d\n", (int)m_discoverSeeds.size());
}

void HomePage::startDiscoverSeedGen()
{
    m_discoverTried.clear();
    m_discoverSeedId.clear();
    buildDiscoverSeeds();
    if (m_discoverSeeds.isEmpty()) {
        m_discoverEmptyAt = nowMs();
        m_discoverBusy = false;
        refreshBusy();
        return;
    }
    m_discoverBusy = true;
    refreshBusy();
    fetchNextDiscoverSeed();
}

// Requests the radio of the next seed. ONE at a time:
// Innertube.fetchRadio() is single-flight. Seeds that already have a saved
// radio are skipped (not regenerated). Stops once kDiscoverMinRadios are
// gathered.
void HomePage::fetchNextDiscoverSeed()
{
    if (m_discover->count() >= kDiscoverMinRadios) {
        finishDiscoverSeedGen();
        return;
    }
    while (!m_discoverSeeds.isEmpty()) {
        const QString seed = m_discoverSeeds.takeFirst();
        if (m_discoverTried.contains(seed))
            continue;
        if (m_discover->hasRadio(seed))
            continue;
        m_discoverTried.insert(seed);
        m_discoverSeedId = seed;
        NT_DEBUG("E2E discover gen fetch seed=%s\n",
                 seed.toUtf8().constData());
        m_yt->radioVideos(seed);
        return;
    }
    finishDiscoverSeedGen();
}

// In radioReady only the radios requested by Discover itself
// (m_discoverSeedId) matter. The player's radios ("A continuación") arrive
// with a different seed and are ignored (MainWindow saves those separately).
// The radio is saved and the next seed is tried until kDiscoverMinRadios are
// gathered.
void HomePage::onRadioReady(const QString &videoId,
                            const QVector<VideoInfo> &videos)
{
    if (videoId != m_discoverSeedId)
        return;
    m_discoverSeedId.clear();
    const QVector<VideoInfo> valid = sanitizeVideos(videos);
    NT_DEBUG("E2E discover gen radio seed=%s n=%d\n",
             videoId.toUtf8().constData(), (int)valid.size());
    if (!valid.isEmpty()) {
        for (const VideoInfo &v : valid)
            m_videoCache->upsert(v);
        m_discover->saveRadio(videoId, valid);
    }
    if (m_discover->count() >= kDiscoverMinRadios)
        finishDiscoverSeedGen();
    else
        fetchNextDiscoverSeed();
}

void HomePage::finishDiscoverSeedGen()
{
    m_discoverSeedId.clear();
    m_discoverSeeds.clear();
    m_discoverBusy = false;
    if (m_discover->count() == 0)
        m_discoverEmptyAt = nowMs(); // no results: retry with backoff
    else
        markDiscoverSeeded(); // seeding already happened: don't auto-fill again
    refreshBusy();
    buildDiscoverSelection();
}

// "↻ Another": re-selects using the ALREADY SAVED radios (no regeneration,
// no network requests). Only if no usable radio remains does it fall into the
// initialization exception.
void HomePage::onDiscoveryAgain()
{
    interruptDiscover();
    m_discoverVideos.clear();
    m_discoverFetchedAt = 0;
    ensureDiscover();
}

void HomePage::interruptDiscover()
{
    m_discoverSeedId.clear();
    m_discoverSeeds.clear();
    m_discoverBusy = false;
}

void HomePage::invalidateDiscover()
{
    m_discoverVideos.clear();
    m_discoverFetchedAt = 0;
    m_discoverEmptyAt = 0;
    if (m_discoverBusy)
        return;
    buildDiscoverSelection();
}

void HomePage::showDiscover()
{
    const bool vis = !m_discoverVideos.isEmpty();
    m_discGrid->showVideos(m_discoverVideos, VideoListView::GridMode);
    adjustGridHeight(m_discGrid);
    m_discTitle->setVisible(vis);
    m_discMoreBtn->setVisible(vis);
    m_discGrid->setVisible(vis);
    NT_DEBUG("E2E discover setVisible=%d count=%d height=%d\n", vis ? 1 : 0,
             (int)m_discoverVideos.size(), m_discGrid->height());
    m_layoutTimer->start();
}

void HomePage::setMiniPlayerVisible(bool visible)
{
    if (m_miniVisible == visible)
        return;
    m_miniVisible = visible;
    if (!m_bodyLayout)
        return;
    const int extra = visible ? kMiniBarReservedPx : 0;
    NT_DEBUG("E2E miniVisible=%d bottomMargin=%d\n", visible ? 1 : 0,
             20 + extra);
    m_bodyLayout->setContentsMargins(14, 14, 14, 20 + extra);
}

void HomePage::refreshBusy()
{
    // PERSONALIZED content: history counts even if the video is not in
    // "Continue watching" (e.g. fully watched or left at the start, which
    // m_continueVideos excludes). refreshBusy is cheap; recentIds(1) is a
    // local query, so the DB is asked directly instead of relying on
    // already-rendered lists.
    const bool hasHistory = m_history && !m_history->recentIds(1).isEmpty();
    const bool anyUserContent = hasHistory || !m_feed.isEmpty()
        || !m_discoverVideos.isEmpty();
    const bool anyContent = anyUserContent || !m_home.isEmpty()
        || !m_trending.isEmpty();
    const bool loading = m_feedBusy || m_homeBusy || m_trendBusy
        || m_discoverBusy;
    m_busy->setVisible(loading && !anyContent);
    // Empty-state: shown when there is NO personalized user content (history,
    // subscriptions, Discover radios) — the home only has Recommended and/or
    // Trending, which come from the network. It covers the visual gap left by
    // the hidden "Continue watching", "Discover" and "New from your
    // subscriptions", and guides the user.
    if (m_emptyState)
        m_emptyState->setVisible(!anyUserContent);
    NT_DEBUG("E2E refreshBusy: history=%d feed=%d disc=%d cont=%d empty=%d\n",
             hasHistory ? 1 : 0, m_feed.size(), m_discoverVideos.size(),
             m_continueVideos.size(),
             m_emptyState ? (m_emptyState->isVisible() ? 1 : 0) : -1);
}

void HomePage::adjustGridHeight(VideoListView *grid)
{
    const int count = grid->videos().size();
    if (count == 0) {
        // 0 videos: the grid is about to be hidden; we leave it at height 0
        // so it doesn't consume the huge gap in the scroll (otherwise a
        // visual void remains before the next section).
        grid->setFixedHeight(0);
        return;
    }
    const int w = grid->width();
    const int cols = qMax(1, grid->gridColumns());
    const int rows = (count + cols - 1) / cols;
    // Effective cell height: if the grid fills the width, the cell scaled
    // (16:9 from the real width); otherwise the base one.
    const int cellH = grid->gridSize().height() > 0
                          ? grid->gridSize().height()
                          : kGridH;
    // In IconMode rows go exactly one after another, with no gaps (measured:
    // cell 177x195 -> rect 177x195 and step 195), so the content height is
    // exactly rows*cellH. What matters is that cellH is the REAL cell size:
    // see VideoListView::applyGridGeometry.
    const int newH = rows * cellH;
    NT_DEBUG("E2E adjustGridHeight count=%d width=%d cols=%d rows=%d cellH=%d height=%d\n",
             count, w, cols, rows, cellH, newH);
    grid->setFixedHeight(newH);
}

// Effective columns: the ones the user asked for, clipped when the window is
// too narrow for the cell to keep the full text block. Without the clamp,
// "Small" (6) in a 700 px window would give 113 px cells with the title and
// channel clipped. The rule lives in VideoListView::clampGridColumns (pure
// function, tested in --selftest-grid).
//
// The width is NOT taken from a section's grid: grids are hidden when their
// section is empty and a hidden widget has its viewport narrowed (98 px
// instead of 760 with the window at 1280). With an empty base that always
// forced the 3-column minimum and the "Thumbnail size" setting was ignored.
// The scroll is measured instead: it is always visible and shares its width
// with every grid; if it is not visible yet (startup), the page width minus
// m_bodyLayout's margins is used as a fallback.
int HomePage::effectiveCellColumns() const
{
    int vw = 0;
    if (m_scroll && m_scroll->isVisible())
        vw = m_scroll->viewport()->width();
    if (vw <= 0)
        vw = qMax(0, width() - 2 * 14);
    return VideoListView::clampGridColumns(m_cellColumns, vw);
}

void HomePage::setCellColumns(int columns)
{
    if (columns <= 0 || columns == m_cellColumns)
        return;
    m_cellColumns = columns;
    applyGridSizes();
}

void HomePage::applyGridSizes()
{
    NT_DEBUG("E2E applyGridSizes called\n");
    // First the effective columns (with the width clamp) and the cell size
    // derived from them; then each section's heights, which use that size.
    // Applying it the other way around would leave the heights one step
    // behind.
    const int cols = effectiveCellColumns();
    for (VideoListView *g : {m_contGrid, m_feedGrid, m_homeGrid,
                             m_discGrid, m_trendGrid})
        if (g)
            g->setGridColumns(cols);
    adjustGridHeight(m_contGrid);
    adjustGridHeight(m_feedGrid);
    adjustGridHeight(m_homeGrid);
    adjustGridHeight(m_discGrid);
    adjustGridHeight(m_trendGrid);
    NT_DEBUG("E2E applyGridSizes done: cols=%d cont=%d feed=%d home=%d disc=%d trend=%d\n",
             cols, m_contGrid->height(), m_feedGrid->height(),
             m_homeGrid->height(), m_discGrid->height(), m_trendGrid->height());
}

void HomePage::scheduleGridSizes()
{
    QTimer::singleShot(0, this, &HomePage::applyGridSizes);
}

void HomePage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    applyGridSizes();
}

