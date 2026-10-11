#pragma once

#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QVector>

#include "models.h"

class Ipv4Https;

// A row of the search box suggestions dropdown.
// query: the query that would run when clicked. detail: supporting text of
// the entity when YouTube sends one ("Cantante y compositor peruano"), empty
// in normal rows.
struct Suggestion
{
    QString query;
    QString detail;
};

// YouTube stream resolution via the Innertube API, independent of
// the UI and the player.
//
// Flow: GET /sw.js_data → visitorData → POST youtubei/v1/player (NewPipe-style
// client cascade: VISIONOS primary, then IOS,
// WEB_EMBEDDED_PLAYER, TVHTML5_SIMPLY and ANDROID_VR) → streamingData
// (hlsManifestUrl + adaptiveFormats, the latter to know which video heights
// exist without feeding mpv). fetchMaster() downloads the indicated
// master HLS (any HTTPS host).
//
// The own HTTP/HTTPS transport forces an IPv4 connection (with SNI and
// TLS verification against the target host). YouTube blocks certain
// videos over IPv6 ("Sign in to confirm you're not a bot") on this network;
// not over IPv4.
//
// It knows neither PlayerWidget, nor mpv, nor any page. Only HTTP + JSON.
class Innertube : public QObject
{
    Q_OBJECT

public:
    explicit Innertube(QObject *parent = nullptr);
    ~Innertube() override;

    bool isBusy() const;

    // Use a visitorData persisted from a previous session (if the signature is
    // still valid, it skips the GET sw.js_data at startup). Called by Yt/the app with
    // the value stored in settings; if expired, it is ignored and refreshed on its own.
    void seedVisitorData(const QString &visitorData, qint64 expiresAt);

    // "hl" language for Innertube requests (translation of titles,
    // descriptions and views). The app sets it from audio_language: es/en/pt
    // translate into that language; "original" leaves the default hl (es, as
    // it was before: avoids auto-translation into English).
    void setHl(const QString &hl);
    QString hl() const { return m_hl; }

    // Invalidates the current session (visitorData + cached signed URLs) so
    // the next player request is made with a NEW visitorData (a different
    // YouTube session). This is the recovery from the "gated" session (403
    // mid-playback on audio and/or HLS segments): a normal re-resolution
    // with forceRefresh requests a fresh pot but INSIDE the same gated
    // session and fails again; rotating the visitorData is the only thing that leaves
    // the session. It does not touch in-flight requests nor the transport.
    void rotateSession();

public slots:
    // forceRefresh=true ignores and discards the video's session cache and repeats
    // the POST /player: the signed videoplayback URLs carry a per-request
    // budget and a second playback with the same URL gives 403.
    void fetchHls(const QString &videoId, int preferredClient = -1,
                  bool forceRefresh = false);
    void fetchMaster(const QString &videoId, const QString &masterUrl);

    // Downloads the related videos ("A continuación" / recommendations) of the video
    // via POST youtubei/v1/next with the primary client (VISIONOS). It does
    // not touch the player/master flow; reuses the current visitorData.
    void fetchNext(const QString &videoId);

    // Watch-playlist "radio" YouTube Music style: POST /next with
    // playlistId RDAMVM<videoId> (+ radio params) which returns the queue of the
    // mix generated for the video. fetchRadioMore() requests the next batch with
    // the continuation token. Each video answers via radioReady().
    void fetchRadio(const QString &videoId);
    void fetchRadioMore();

    // Translated detail of the current video explicitly requested for the
    // watch (when /next is not requested to seed the queue, e.g. Radio):
    // a variant of fetchNext that only emits currentDetailReady.
    void fetchCurrentDetail(const QString &videoId);

    // Real Home (POST /browse, browseId FEwhat_to_watch) with the VISIONOS client.
    // It is the only tested endpoint that returns the feed (compactVideoRenderer/
    // shelfRenderer/verticalListRenderer) with a fresh visitorData; an
    // old persisted visitorData returns HTTP 500. That is why fetchHome()
    // forces a fresh sw.js_data (once per session) before the POST. The
    // results arrive via homeReady(); empty = no feed. fetchHomeMore()
    // requests the continuation (homeMoreReady). No retries or loop
    // regenerations.
    void fetchHome();
    void fetchHomeMore();

    // Videos of a subscribed channel (POST /browse, the channel's browseId + params of
    // the "Vídeos" tab) with the VISIONOS client. Reuses the same Home
    // compactVideoRenderer parser; they arrive via channelVideosReady(). Empty =
    // no videos.
    void fetchChannelVideos(const QString &channelId);
    // Trending (POST /browse, browseId FEtrending) with VISIONOS. They arrive via
    // trendingReady(). Nowadays YouTube removes/redirects anonymous trending
    // (400/redirect to Home), so this phase usually comes back empty and the
    // app hides the section.
    void fetchTrending();

    // Search (POST /search, VISIONOS). fetchSearch() brings the first page;
    // fetchSearchMore() the continuation. searchReady() delivers only the new
    // page (up to limit) and whether a continuation remains.
    void fetchSearch(const QString &query, int limit = 20);
    void fetchSearchMore(int limit = 20);

    // Type-ahead suggestions for the search field. Plain GET to
    // suggestqueries.google.com (the real youtube.com autocomplete), without
    // touching the Innertube session. suggestReady ALWAYS emits the requested
    // query so the UI can discard out-of-order responses while the user keeps typing.
    void fetchSuggest(const QString &query);

    // Channel card (POST /browse, "Información" tab): name, avatar,
    // subscribers and description. channelInfoReady().
    void fetchChannelInfo(const QString &channelId);

    // Channel tab (videos|shorts|live). "videos" goes through VISIONOS
    // (compactVideoRenderer); shorts/live via the WEB client, because VISIONOS
    // does not expose those tabs (it returns Home). channelTabReady() with the
    // page and whether there is more.
    void fetchChannelTab(const QString &channelId, const QString &tab,
                         int limit = 25);
    void fetchChannelTabMore(const QString &channelId, const QString &tab,
                             int limit = 25);

    // Video detail (title, channel, views, date, description and "me
    // gusta") from the /next. videoDetailReady().
    void fetchVideoDetail(const QString &videoId);

    // Playlist detail (POST /browse, browseId VL<id>): header + videos.
    void fetchPlaylist(const QString &playlistId, int limit = 50);

    // True if the last Home response brought a continuation token.
    bool hasHomeContinuation() const { return !m_homeContinuation.isEmpty(); }

    // Innertube client that produced the most recent hlsReady, or -1 if none
    // has won yet (for example after regenerating visitorData). Yt uses it to
    // remember which client managed to play and prioritize it on
    // resolution changes.
    int lastWinClient() const { return m_lastWinClient; }

    // The player response declares the media as a CURRENT LIVE BROADCAST
    // (videoDetails.isLive): an authoritative YouTube signal, not inferred from the
    // duration. Yt uses it to mark the StreamInfo so the UI enters LIVE MODE
    // instantly (and covers pure live streams with dur=0).
    bool lastIsLive() const { return m_lastIsLive; }

    // Path of the resolution cache (hls+master) JSON file on disk.
    // Public so the application reset can delete it with the
    // same path the persistence uses.
    static QString hlsCacheFilePath();

signals:
    // videoHeights: video heights (unique, ascending) present in the
    // response player's adaptiveFormats. Empty if it could not be extracted.
    void hlsReady(const QString &videoId, const QString &hlsUrl,
                  const QVector<int> &videoHeights);
    void masterReady(const QString &videoId, const QByteArray &masterBody);
    // Complete adaptiveFormats (with a direct url per format) of the
    // /player response. Yt consumes it to build the single-video "edge" master
    // + audio from Innertube; if empty (or its entries bring no
    // url), Yt falls back to the classic hlsManifestUrl → fetchMaster mode.
    void formatsReady(const QString &videoId, const QJsonArray &adaptiveFormats,
                      int durationSeconds);
    void failed(const QString &videoId, const QString &reason);
    // Related videos of the requested video via /next. Empty list if it could
    // not be extracted or the response does not bring them.
    void relatedReady(const QString &videoId, const QVector<VideoInfo> &videos);
    // Queue of the requested video's mix/radio via /next (RDAMVM). Empty videos if
    // the client returned no queue (then the app decides to mix or continue).
    void radioReady(const QString &videoId, const QVector<VideoInfo> &videos);
    // Detail of the current video returned by /next: title and description in the
    // requested "hl" language (YouTube translation). Empty description =
    // the client did not include the metadata renderer of the video being played.
    void currentDetailReady(const QString &videoId, const QString &title,
                            const QString &description);
    // Home feed (browse FEwhat_to_watch). Empty = the endpoint did not return
    // videos (500 with an old visitorData, layout change, no feed
    // available).
    void homeReady(const QVector<VideoInfo> &videos);
    void homeMoreReady(const QVector<VideoInfo> &videos);
    // Videos of a channel (browse "Vídeos" tab) and Trending (browse
    // FEtrending). Empty = no results.
    void channelVideosReady(const QString &channelId,
                            const QVector<VideoInfo> &videos);
    void trendingReady(const QVector<VideoInfo> &videos);
    // Search results page (searchReady) and a channel tab
    // (channelTabReady). hasMore indicates whether YouTube issued a continuation token.
    void searchReady(const QString &query, const QVector<VideoInfo> &videos,
                     bool hasMore);
    // Type-ahead suggestions for searchQuery. Empty if the request failed or
    // the body brought no valid queries: the UI hides the dropdown.
    void suggestReady(const QString &searchQuery,
                      const QVector<Suggestion> &suggestions);
    void channelTabReady(const QString &channelId, const QString &tab,
                         const QVector<VideoInfo> &videos, bool hasMore);
    // Channel card (name, avatar, subscribers, description).
    void channelInfoReady(const ChannelInfo &channel);
    // Detail of the current video (translated title/description + likes) from the /next.
    void videoDetailReady(const VideoInfo &video, const QString &description,
                          qint64 likeCount);
    // Header + videos of a playlist.
    void playlistReady(const PlaylistInfo &info,
                       const QVector<VideoInfo> &videos);
    // visitorData renewed or used for the first time; the app persists it to
    // avoid the GET sw.js_data on the next startup.
    void visitorDataReady(const QString &visitorData, qint64 expiresAt);

private:
    void fetchVisitorData();
    void requestPlayer();
    void requestNext();
    void requestRadio();
    // Second request of the radio fallback: the same /next with the WEB
    // client, which returns lockupViewModel (with views and date) where VISIONOS
    // returns compactVideoRenderer (no date).
    void requestRadioWeb(const QString &videoId);
    void handleRadioWeb(const QString &videoId, bool ok, const QByteArray &body);
    // Launches the radio queued by fetchRadio() while another was in flight
    // (single-flight). Called at the terminal points of a radio, BEFORE the
    // finished radio is emitted: the player's request then takes the slot
    // ahead of the Descubre generator's next seed (see fetchRadio).
    void promoteQueuedRadio();
    void requestHome(const QString &continuation);
    void requestChannel();
    void requestTrending();
    void handleHome(bool ok, const QString &error, const QByteArray &body,
                    bool more);
    void handleChannel(bool ok, const QString &error, const QByteArray &body);
    void handleTrending(bool ok, const QString &error, const QByteArray &body);
    void requestSearch(bool more);
    void handleSuggest(const QString &query, bool ok, const QByteArray &body);
    void handleSearch(const QString &query, bool ok, const QString &error,
                      const QByteArray &body, bool more);
    void requestChannelInfo();
    void handleChannelInfo(const QString &channelId, bool ok,
                           const QString &error, const QByteArray &body);
    void requestChannelTab(const QString &channelId, const QString &tab,
                           bool more);
    void handleChannelTab(const QString &channelId, const QString &tab, bool ok,
                          const QString &error, const QByteArray &body,
                          bool more);
    void requestVideoDetail();
    void handleVideoDetail(const QString &videoId, bool ok,
                           const QString &error, const QByteArray &body);
    void requestPlaylist();
    void handlePlaylist(const QString &playlistId, bool ok,
                        const QString &error, const QByteArray &body);
    void handleNext(const QString &videoId, bool ok, const QString &error,
                    const QByteArray &body);
    void handleRadio(const QString &videoId, bool ok, const QString &error,
                     const QByteArray &body);
    void useFallbackVisitorData();
    // One retry per phase of the Home sections: if the browse (Home,
    // channel, trending) responds 500/empty with the current persisted
    // visitorData, visitorData is refreshed ONCE and the phase is re-queued (the
    // dispatcher relaunches it). Avoids the forced sw.js_data refresh on every
    // startup; a real failure adds barely one GET + retry.
    bool retryBrowseWithFreshVisitor(bool *retried, bool *pending);
    // True if the current visitorData is usable (non-empty and not expired). When it is
    // false, the corresponding phase must set its *Pending, call
    // fetchVisitorData() and return: the dispatcher will relaunch it.
    bool hasFreshVisitor() const;
    void dispatchPending();
    void handlePlayer(bool ok, const QString &error, const QByteArray &body);
    void handleMaster(bool ok, const QString &error, const QByteArray &body);
    void fail(const QString &reason);
    QVector<VideoInfo> parseRelated(const QJsonObject &root) const;
    // compactVideoRenderer/videoRenderer renderer → VideoInfo. Fills
    // publishedAt with an approximate timestamp if there is only relative
    // text (e.g. "hace 3 días"). Used in Home, channels and trending.
    VideoInfo videoFromHomeRenderer(const QJsonObject &item) const;
    // Walks the tree looking for compactVideoRenderer/videoRenderer (without
    // duplicating ids) up to maxCount, respecting the response order.
    QVector<VideoInfo> collectVideoRenderers(const QJsonObject &root,
                                             int maxCount) const;
    // Playback mix queue (playlistPanelVideoRenderer) + continuation
    // token to request more batches. Empty out if the client brings no
    // playback panel in the /next.
    void parseWatchQueue(const QJsonObject &root, QVector<VideoInfo> *out,
                         QString *continuation) const;
    // Parser of the Home feed (FEwhat_to_watch): walks the verified layout
    // singleColumnBrowseResultsRenderer → sectionListRenderer →
    // shelfRenderer → verticalListRenderer → compactVideoRenderer, limiting to
    // kHomeShelfCap shelves × kHomeShelfVideos videos. If no shelves are
    // found (different layout), it collects up to kHomeShelfVideos. Leaves the
    // first continuation token (if any) in *continuation.
    QVector<VideoInfo> parseHomeBrowse(const QJsonObject &root,
                                       QString *continuation) const;
    // /search page: compactVideoRenderer of the sectionListRenderer +
    // first continuation token. Limited to maxCount.
    QVector<VideoInfo> parseSearchBrowse(const QJsonObject &root, int maxCount,
                                         QString *continuation) const;
    // Modern renderers of the WEB client: shortsLockupViewModel and
    // lockupViewModel (channel shorts/live tabs). Walks the tree without
    // duplicating ids; maxCount limits the page.
    QVector<VideoInfo> collectWebLockups(const QJsonObject &root,
                                         int maxCount) const;
    // Channel card from the "Información" browse: c4TabbedHeaderRenderer
    // (title, avatar, subscribers, handle) + channelAboutMetadataRenderer
    // (description, country, canonical url).
    static void parseChannelInfo(const QJsonObject &root, ChannelInfo *info);
    // Detail from the /next: videoMetadataRenderer (title, views, date, channel,
    // description) + likeButtonRenderer (likes).
    static void parseVideoDetail(const QJsonObject &root, VideoInfo *video,
                                 QString *description, qint64 *likeCount);
    // Playlist (browse VL<id>): playlistHeaderRenderer + playlistVideoRenderer.
    static void parsePlaylistBrowse(const QJsonObject &root, PlaylistInfo *info,
                                    QVector<VideoInfo> *videos);
    // First continuation token appearing in the tree (any
    // layout: continuationCommand / nextContinuationData /
    // reloadContinuationData). Empty if none.
    static QString firstContinuationToken(const QJsonObject &root);
    // Emits hlsReady/masterReady from the cache (current signature) using the
    // single-request generator/waiter. If there is no valid cache or there is a
    // request in flight for that videoId, it does nothing and the in-flight
    // request ends up re-emitting hlsReady/masterReady to everyone waiting.
    bool serveCachedHls(const QString &videoId);
    bool serveCachedMaster(const QString &videoId);
    qint64 expireFromUrl(const QString &url) const;

    // Translated detail of the current video: normal POST /next and emits
    // currentDetailReady with the videoMetadataRenderer (title and description
    // in the active "hl"). No client cascade (VISIONOS primary).
    void requestCurrentDetail();
    // Title and description of the /next's videoMetadataRenderer, in the
    // requested "hl" language. Empty output if the client did not include the renderer.
    static void parseCurrentDetail(const QJsonObject &root, QString *title,
                                   QString *description);

    // Persistence of the resolution cache (hls+master) to disk, to
    // reopen already-resolved videos without repeating POST player + GET master
    // while the URL signature remains valid. Same pattern as the
    // persisted visitorData (SettingsRepo), but in JSON.
    void saveHlsCacheToDisk() const;
    void loadHlsCacheFromDisk();

    Ipv4Https *m_http = nullptr;
    QString m_videoId;
    QString m_visitorData;
    // "hl" language of the requests (translation of titles/descriptions by the
    // app from audio_language). "es" is the historical default (fix for
    // original titles against auto-translation into English).
    QString m_hl = QStringLiteral("es");
    qint64 m_visitorExpiresAt = 0;
    bool m_retryingVisitor = false;
    int m_clientIndex = 0;
    int m_lastWinClient = -1;
    // videoDetails.isLive of the most recent player response (current
    // resolution). Copied into the cache so a reuse of the same
    // one keeps the authoritative signal.
    bool m_lastIsLive = false;
    // True if the current visitorData was freshly obtained in this session
    // (sw.js_data). The player/next/radio phases request it at startup; the
    // Home sections (home/feed/trending) use the persisted current one directly
    // and only refresh if a browse responds 500/empty
    // (single retry via m_*VisitorRetried).
    bool m_visitorDataFreshRun = false;
    // Pending after refreshing visitorData: the dispatcher launches the phases
    // that were left waiting (player first). m_visitorInFlight avoids launching two
    // GET sw.js_data at once when player + next/radio request visitorData
    // almost simultaneously.
    bool m_playerPending = false;
    bool m_visitorInFlight = false;

    // Session cache per videoId: signed hlsManifestUrl, master body and
    // its expiration (from the expire parameter of the signed URL). Avoids repeating
    // POST player + GET master on resolution changes of the same video.
    struct HlsCacheEntry
    {
        QString hlsUrl;
        QByteArray masterBody;
        QVector<int> heights; // adaptiveFormats heights (to filter the master)
        qint64 expiresAt = 0; // Unix moment after which the signature expires
        int clientIndex = -1;
        // adaptiveFormats of the /player (with direct url) + video duration:
        // they allow rebuilding the edge master without repeating the POST player.
        QJsonArray adaptiveFormats;
        int durationSeconds = 0;
        bool isLive = false;
    };
    QHash<QString, HlsCacheEntry> m_hlsCache;
    // Single-flight per phase: if there is already an HLS or master request in flight
    // for m_flightVideoId, requests for the same videoId are resolved
    // when the first one finishes (Yt's guards filter by generation and all of
    // them receive the same hlsReady/masterReady/failed signals).
    QString m_flightVideoId;
    bool m_hlsInFlight = false;
    bool mMasterInFlight = false;
    qint64 m_hlsStartMs = 0;
    qint64 m_masterStartMs = 0;

    // Related videos request (/next). Independent of the player flow; uses
    // the same visitorData. m_nextPending is set when visitorData must be
    // refreshed before the POST can be launched.
    bool m_nextPending = false;
    bool m_nextInFlight = false;
    QString m_nextVideoId;
    int m_nextClientIndex = 0;
    QVector<VideoInfo> m_bestNext;

    // Radio queue request (/next with playlistId RDAMVM). Shares
    // visitorData and the same transport. The continuation token allows
    // requesting mix batches while it plays.
    bool m_radioPending = false;
    bool m_radioInFlight = false;
    QString m_radioVideoId;
    // Video whose fetchRadio arrived while another radio was in flight
    // (single-flight) and is therefore queued instead of dropped: promoted by
    // promoteQueuedRadio() at the next terminal point.
    QString m_radioQueuedId;
    QString m_radioContinuation;
    // Recommendation mix from the first client without an RDAMVM panel (fallback).
    QVector<VideoInfo> m_radioFallback;
    // Mix VISIONOS returned when there was no panel: kept while the
    // WEB version (with views and date) is requested and is the backup if that one fails.
    QVector<VideoInfo> m_radioVisionFallback;
    int m_radioClientIndex = 0;

    // Translated detail of the current video (normal /next): title and description in
    // the requested "hl". Independent of the queue: never emits
    // relatedReady nor touches the seeding. m_currentDetailPending marks the wait
    // for the visitorData refresh.
    bool m_currentDetailPending = false;
    bool m_currentDetailInFlight = false;
    QString m_currentDetailVideoId;

    // Real Home request (/browse FEwhat_to_watch). Independent of the
    // player flow; uses fresh visitorData. m_homeContinuation stores the
    // token for fetchHomeMore(); m_homePending marks the wait for the
    // visitorData refresh.
    bool m_homePending = false;
    bool m_homeInFlight = false;
    bool m_homeMoreInFlight = false;
    QString m_homeContinuation;
    bool m_homeVisitorRetried = false;

    // Channel videos request (browse "Vídeos" tab). One at a time
    // (HomePage chains them); uses fresh visitorData and the same transport.
    bool m_channelPending = false;
    bool m_channelInFlight = false;
    QString m_channelId;
    QString m_channelContinuation;
    bool m_channelVisitorRetried = false;

    // Trending request (browse FEtrending). Independent; if it fails/
    // responds empty, the app hides the section.
    bool m_trendPending = false;
    bool m_trendInFlight = false;
    bool m_trendVisitorRetried = false;

    // Search (/search). First page and continuation; the stored token
    // enables fetchSearchMore().
    bool m_searchPending = false;
    bool m_searchInFlight = false;
    bool m_searchVisitorRetried = false;
    QString m_searchQuery;
    QString m_searchContinuation;
    int m_searchLimit = 20;
    // Type-ahead suggestions. No paging or retry state: it is
    // a single one-shot request by prefix. Only the in-flight query is stored to
    // discard responses that arrive late (the user keeps typing).
    bool m_suggestInFlight = false;
    QString m_suggestQuery;
    // Channel card (browse "Información").
    bool m_chInfoPending = false;
    bool m_chInfoInFlight = false;
    bool m_chInfoVisitorRetried = false;
    QString m_chInfoId;
    // Channel tabs (videos via VISIONOS; shorts/live via WEB). The UI requests
    // all 3 at once, so each (channel, tab) has its own state: a single set of
    // members caused concurrent responses to be labeled with the last
    // requested tab.
    struct ChannelTabState
    {
        QString channelId;
        QString tab;
        bool inFlight = false;
        bool moreInFlight = false;
        bool pending = false;
        bool visitorRetried = false;
        QString continuation;
        int limit = 25;
    };
    QHash<QString, ChannelTabState> m_tabStates;
    static QString tabKey(const QString &channelId, const QString &tab);
    // Video detail (/next).
    bool m_detailPending = false;
    bool m_detailInFlight = false;
    bool m_detailVisitorRetried = false;
    QString m_detailId;
    // Playlist detail (browse VL<id>).
    bool m_plPending = false;
    bool m_plInFlight = false;
    bool m_plVisitorRetried = false;
    QString m_plId;
};

// Isolated selftest of the sw.js_data → visitorData → player →
// hlsManifestUrl → adaptiveFormats heights chain. Does not touch the player.
// Returns true if the whole chain responds with HLS and heights.
bool innertubeSelfTest();

// Diagnostic probe of the real Home (POST /browse, browseId FEwhat_to_watch).
// It is NOT the app's Home and does NOT touch HomePage nor the player: it only investigates
// whether the Home endpoint responds for the existing clients (VISIONOS
// first, then IOS/WEB/ANDROID as a diagnostic fallback), with or without
// persisted visitorData (NEVERTUBE_HOME_VISITOR). Prints HTTP status,
// timings, size, renderers, videos, continuations and anti-bot challenges.
bool homeBrowseSelfTest();

// Selftest of the Home feeds via Innertube: videos of a channel (browse
// "Vídeos" tab + params; channel configurable with NEVERTUBE_TEST_CHANNEL) and
// Trending (FEtrending). Verifies HTTP status, size, renderers, real
// videos (id, title, duration), order, duplicates, limit and the empty/error
// behavior. Does not touch the player nor SQLite.
bool feedsProtocolSelfTest();

// Selftest of the Innertube browse endpoints: search, channel card,
// tabs (videos/shorts/live), video detail and playlist.
bool innertubeBrowseSelfTest();

// Probe of the suggestion endpoints (type-ahead of the search field).
// Measures the endpoint that the type-ahead USES (C1, suggestqueries with
// client=youtube) against two quality references, with the same prefix and the
// same network. Prints status, latency, size and how many suggestions are
// a real QUERY. Also runs suggestParseSelfTest() (offline) to pin down the
// parser behavior. The PASS/FAIL is of C1: that is the one the app uses.
bool suggestSelfTest();

// OFFLINE selftest of /complete/search parsing (no network or GPU, runs in CI).
// Pins down the three points where this endpoint can break silently:
//   1) the window.google.ac.h(...) wrapper is stripped and the JSON is read;
//   2) items WITHOUT a query (or with non-string text) are discarded, they do not
//      come out as blank rows;
//   3) a body that is not the known format (error HTML, empty JSON,
//      changed wrapper) returns an empty list and does NOT crash.
bool suggestParseSelfTest();

// OFFLINE selftest of the relative dates parser (relativeToIso). Pins down the real
// bug in which "hace N semanas" fell into the SECONDS branch — because "sem…"
// starts with 's' just like "seg…" —, the date came out N seconds from "now" and
// the app showed videos from 2/4 weeks ago as "hoy" (Home, channel, search,
// queue, playlist and Player). No network or GPU: runs in CI.
bool relativeDateSelfTest();
