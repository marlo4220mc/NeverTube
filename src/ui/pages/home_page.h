#pragma once

#include <QHash>
#include <QScrollArea>
#include <QSet>
#include <QWidget>
#include <QVector>

#include "../../models.h"

class QLabel;
class QPushButton;
class QTimer;
class QTimer;
class QVBoxLayout;
class DiscoverRepo;
class HistoryRepo;
class HomeCacheRepo;
class SettingsRepo;
class SubscriptionsRepo;
class ThumbnailCache;
class VideoCacheRepo;
class VideoListView;
class Yt;

// NeverTube Home. Sections (fixed order; the empty ones are hidden):
//   1. ▶ Continuar viendo      : local history (≤4, progress on thumbnail)
//   2. 🔔 Nuevos suscripciones : ≤5 recent per subscribed channel (TTL 30 min)
//   3. ✨ Recomendado YouTube   : browse FEwhat_to_watch VISIONOS (≤3×8, TTL 60 min)
//   4. 🎲 Descubre             : ≤5 videos from SAVED radios (1-2 per radio,
//                                persistent SQLite source, TTL 60 min)
//   5. 🔥 Tendencias           : ≤8, trending via Innertube (TTL 2 h)
// In-memory cache with TTL: on entering Home the cached content is shown right
// away and if it expired it is refreshed ONCE in the background (single-flight,
// no polling). If the update fails or returns empty, the previous cached
// section is KEPT (memory and UI; it is neither hidden nor replaced by empty,
// nor is fetched_at refreshed). The subscription feed and the trends (Innertube)
// never disappear: if those sections end up empty after everything fails, what
// is persisted in home_cache is kept. What is persisted in the DB is replaced
// only when a VALID network response arrives (never on failure).
class HomePage : public QWidget
{
    Q_OBJECT

public:
    explicit HomePage(Yt *yt, SubscriptionsRepo *subs, HistoryRepo *history,
                      DiscoverRepo *discover, VideoCacheRepo *videoCache,
                      ThumbnailCache *thumbs, SettingsRepo *settings,
                      HomeCacheRepo *homeCache, QWidget *parent = nullptr);

    void reload();
    void refreshContinueWatching();
    void refreshDynamic();
    // Id of the video being played: Descubre does not show it (so that Home
    // does not repeat the video being watched on the playback page).
    void setCurrentVideoId(const QString &id);
    // Extra bottom margin for the scrollable content while the mini player is
    // visible, so the last section can be brought above it and seen in full.
    // Removed when it is hidden.
    void setMiniPlayerVisible(bool visible);
    // Aborts the Discover radios generation in progress (only happens as an
    // initialization exception, when there are no saved radios). Called when
    // opening a video so it does not compete with the player's "A continuación"
    // radio (Innertube.fetchRadio is single-flight).
    void interruptDiscover();
    // Invalidates the Discover cache (re-selects it or hides it if there are no
    // radios left). Called when the history changes (e.g. when videos are
    // deleted): Descubre must not keep showing selections from deleted radios.
    void invalidateDiscover();
    // Videos per row of the Home grids ("Tamaño de las miniaturas" setting:
    // 4/5/6). It is the only size lever of a grid, which is sized by the width
    // with a fixed number of columns; the width imposes a cap so that the cell
    // never drops below kMinCellW (otherwise, with 6 columns in a narrow window
    // the 2-line title gets cut off).
    void setCellColumns(int columns);
    int cellColumns() const { return m_cellColumns; }
    // Re-applies the empty-state button with the current accent (MainWindow
    // calls it when the accent color changes in Settings, without rebuilding).
    void applyAccent();

signals:
    void openRequested(const QVector<VideoInfo> &videos, int startIndex);
    void resumeRequested(const QVector<VideoInfo> &videos, int startIndex,
                         qlonglong startPositionMs);
    // Home empty state: the user asked to go to Search.
    void navigateToSearchRequested();

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onTrending(const QVector<VideoInfo> &videos);
    void onChannelVideos(const QVector<VideoInfo> &videos);
    void onFeedDone();
    void onHomeReady(const QVector<VideoInfo> &videos);
    void onRadioReady(const QString &videoId, const QVector<VideoInfo> &videos);
    void onDiscoveryAgain();

private:
    bool cachedFresh(qint64 at, qint64 ttlMs) const;
    void loadContinueWatching();
    // A cached live stream can end between visits: on re-entering Home the
    // /next is re-checked (it does not touch the player phase), with a
    // kLiveRecheckMs dedup and a per-video in-flight guard.
    void maybeRefreshLiveStatus(const VideoInfo &v);
    void onLiveDetailReady(const VideoInfo &video);
    void loadSections();    void loadFeed();
    void requestNextChannel();
    // Signature of the subscription set (sorted ids). Persisted after building
    // the feed; if it differs upon entering Home, the cached feed went stale
    // (subscribed/unsubscribed) and its rebuild is forced even if its TTL is
    // still valid.
    QString currentSubsSignature() const;
    void maybeInvalidateFeed();
    // Loads from home_cache (HomeCacheRepo) the NETWORK sections persisted on
    // the last startup: Home shows content at once and each section refreshes
    // itself when its TTL expires (Home persists everything except what is
    // already local: Continuar viendo derives from the history and Descubre
    // from the saved radios).
    void loadPersistedSections();
    // Saves (replaces, does not append) a network section into home_cache after
    // fresh data arrives. The ids are deduced from the list in displayed order.
    void saveSection(const QString &section, const QVector<VideoInfo> &videos);
    void ensureDiscover();
    // Builds and inserts into the bodyLayout the "Tu inicio aún está
    // vacío" panel. It is shown until any section has content (refreshBusy
    // controls that). Built only once (in the ctor).
    void buildEmptyState();
    // Builds the Descubre selection ONLY from saved radios (no network) and
    // shows it. If there are no useful radios, it triggers the initialization
    // generation (the only exception that touches Yt::radioVideos).
    void buildDiscoverSelection();
    void startDiscoverSeedGen();
    void fetchNextDiscoverSeed();
    void finishDiscoverSeedGen();
    void buildDiscoverSeeds();
    void showDiscover();
    void refreshBusy();
    void adjustGridHeight(VideoListView *grid);
    // Recomputes the height of the section grids (continue/feed/Recomendado/
    // Tendencias). The height is derived from the grid's real width, so the
    // sections fit their content and leave no vertical gap between one block
    // and the next.
    void applyGridSizes();
    void scheduleGridSizes();
    // Effective columns after applying the width cap (kMinCellW).
    int effectiveCellColumns() const;

    Yt *m_yt;
    SubscriptionsRepo *m_subs;
    HistoryRepo *m_history;
    VideoCacheRepo *m_videoCache;
    DiscoverRepo *m_discover;
    ThumbnailCache *m_thumbs;
    SettingsRepo *m_settings;
    HomeCacheRepo *m_homeCache;

    // Auto-seed of the initialization radios: only the first time. If the
    // system already generated some radio at some point (discover_seeded), an
    // empty Descubre (e.g. after clearing the whole history) does NOT fill
    // itself: radios are only generated again when playing videos.
    bool discoverSeeded() const;
    void markDiscoverSeeded();

    QScrollArea *m_scroll;
    QWidget *m_body;
    QLabel *m_busy;
    // "Aún no hay nada para mostrar" panel: shown when ALL sections are empty
    // (no history, no subscriptions, no Descubre radios, no previous cache). It
    // closes the giant gap left by the hide() sections and tells the user what
    // to do to get started.
    QWidget *m_emptyState = nullptr;

    QLabel *m_contTitle;
    VideoListView *m_contGrid;
    QLabel *m_feedTitle;
    VideoListView *m_feedGrid;
    QLabel *m_homeTitle;
    VideoListView *m_homeGrid;
    QLabel *m_discTitle;
    QPushButton *m_discMoreBtn;
    VideoListView *m_discGrid;
    QLabel *m_trendTitle;
    VideoListView *m_trendGrid;
    // Videos per row chosen by the user (4/5/6). applyGridSizes() applies the
    // width cap to them before fixing the size of the grids.
    int m_cellColumns = 5;

    QVector<VideoInfo> m_continueVideos;
    QVector<qlonglong> m_continuePositions; // aligned with m_continueVideos
    // Live re-checks for "Continuar viendo": ids with a /next request in flight
    // and the last check time of each.
    QSet<QString> m_liveRefreshPending;
    QHash<QString, qint64> m_liveRefreshedAt;
    QVector<VideoInfo> m_feed;
    // Copy of the displayed feed taken when an update starts (loadFeed). If the
    // update ends up empty/failed, onFeedDone restores it instead of hiding the
    // section.
    QVector<VideoInfo> m_prevFeed;
    qint64 m_feedFetchedAt = 0;
    QString m_feedSubsSig;
    QVector<VideoInfo> m_home;
    qint64 m_homeFetchedAt = 0;
    // 4) 🎲 Descubre. PERSISTENT source (SQLite): the radios are generated when
    // videos are played (and saved), or by the initialization exception if
    // there are no radios. Opening Home NEVER requests radios on its own: if the
    // in-memory cache expired, videos are re-selected from the saved radios
    // (pickDiscoverVideos, no network). "↻ Otro" reshuffles the selection with
    // the already-saved radios (it does not regenerate).
    QVector<VideoInfo> m_discoverVideos;
    qint64 m_discoverFetchedAt = 0;
    qint64 m_discoverEmptyAt = 0;
    bool m_discoverBusy = false;
    QStringList m_discoverSeeds;
    QSet<QString> m_discoverTried;
    QString m_discoverSeedId;
    QVector<VideoInfo> m_trending;
    qint64 m_trendFetchedAt = 0;

    QStringList m_pendingChannels;
    int m_pendingIdx = 0;
    QHash<QString, QString> m_channelNames;
    bool m_feedBusy = false;
    bool m_homeBusy = false;
    bool m_trendBusy = false;
    QString m_currentVideoId;
    bool m_miniVisible = false;
    QVBoxLayout *m_bodyLayout = nullptr;
    QTimer *m_layoutTimer = nullptr;
};