#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "../models.h"

class AppDatabase;
class QSqlQuery;

class DiscoverRepo;

struct SubscriptionInfo
{
    QString id;
    QString title;
    QString avatar;
};

struct PlaylistItem
{
    QString id;
    QString title;
};

class VideoCacheRepo
{
public:
    explicit VideoCacheRepo(AppDatabase &db);

    bool upsert(const VideoInfo &v);
    bool get(const QString &id, VideoInfo &out) const;
    QVector<VideoInfo> getMany(const QStringList &ids) const;
    // Changes only the live flag (refresh via /next, which does not bring the
    // duration); the rest of the fields stay intact.
    bool updateLiveStatus(const QString &id, bool isLive);

private:
    VideoInfo fromRow(const QSqlQuery &q) const;

    AppDatabase &m_db;
};

class HistoryRepo : public QObject
{
    Q_OBJECT
public:
    explicit HistoryRepo(AppDatabase &db, DiscoverRepo *discover = nullptr,
                         QObject *parent = nullptr);

    bool add(const QString &videoId, qlonglong positionMs = 0);
    bool remove(const QString &videoId);
    bool clear();
    QStringList recentIds(int limit = 50) const;
    qlonglong positionOf(const QString &videoId) const;

    // A video counts as COMPLETED if its stored position means ≥95% watched
    // or ≤15 s left (ending/credits). In that case it must not be offered in
    // "Continuar viendo" and, if played from the history, it starts from the
    // beginning.
    static bool isCompleted(const VideoInfo &v, qlonglong positionMs);
    // Initial resume position for a history video: 0 if it is complete (or has
    // no progress), its position otherwise.
    static qlonglong resumeStartMs(const VideoInfo &v, qlonglong positionMs);
    // Playback fraction of a history video (0..1) for the progress bar the
    // delegate draws: 0 if it cannot be resumed (no position, no known
    // duration, already completed, or at the end). It is THE SAME rule in the
    // three places where a video with progress is shown — Home's "Continuar
    // viendo", the History page and its Library section — so a video reads the
    // same everywhere.
    static double progressFraction(const VideoInfo &v, qlonglong positionMs);

signals:
    // The history changed (new video played, position updated, manual
    // delete). HomePage connects to it to refresh "Continuar viendo" and
    // re-evaluate the empty-state as soon as there is user data.
    void changed();

private:
    AppDatabase &m_db;
    DiscoverRepo *m_discover = nullptr;
};

class FavoritesRepo
{
public:
    explicit FavoritesRepo(AppDatabase &db);

    bool toggle(const QString &videoId);
    bool isFavorite(const QString &videoId) const;
    QStringList ids() const;

private:
    AppDatabase &m_db;
};

// Like (thumb) and Favorites (star) are separate repos on purpose. This is the
// manual star, distinct from the "Me gusta" toggle.
class StarredRepo
{
public:
    explicit StarredRepo(AppDatabase &db);

    bool toggle(const QString &videoId);
    bool isStarred(const QString &videoId) const;
    QStringList ids() const;
    bool remove(const QString &videoId);

private:
    AppDatabase &m_db;
};

class WatchLaterRepo
{
public:
    explicit WatchLaterRepo(AppDatabase &db);

    bool toggle(const QString &videoId);
    bool isWatchLater(const QString &videoId) const;
    QStringList ids() const;
    bool remove(const QString &videoId);

private:
    AppDatabase &m_db;
};

class SubscriptionsRepo : public QObject
{
    Q_OBJECT

public:
    explicit SubscriptionsRepo(AppDatabase &db, QObject *parent = nullptr);

    bool add(const QString &channelId, const QString &title,
             const QString &avatarUrl = QString());
    bool setAvatar(const QString &channelId, const QString &avatarUrl);
    bool remove(const QString &channelId);
    bool isSubscribed(const QString &channelId) const;
    QVector<SubscriptionInfo> all() const;

    // The set of subscriptions changed (add/remove): anyone showing derived
    // feeds (e.g. "Nuevos de tus suscripciones" on Home) must refresh. Not
    // emitted in setAvatar: the avatar does not alter the channel list.

signals:
    void changed();

private:
    AppDatabase &m_db;
};

class PlaylistsRepo
{
public:
    explicit PlaylistsRepo(AppDatabase &db);

    QString create(const QString &title);
    bool deletePlaylist(const QString &id);
    bool rename(const QString &id, const QString &title);
    QVector<PlaylistItem> all() const;
    bool contains(const QString &playlistId, const QString &videoId) const;
    bool addVideo(const QString &playlistId, const QString &videoId);
    bool removeVideo(const QString &playlistId, const QString &videoId);
    QStringList videoIds(const QString &playlistId) const;

private:
    AppDatabase &m_db;
};

class SettingsRepo
{
public:
    explicit SettingsRepo(AppDatabase &db);

    bool get(const QString &key, QString &out) const;
    bool set(const QString &key, const QString &value);
    qlonglong getInt(const QString &key, qlonglong def = 0) const;
    QString getString(const QString &key, const QString &def = QString()) const;

private:
    AppDatabase &m_db;
};

// One download: the state of a file that is being fetched (or that was).
//
// The `status` is a FIXED vocabulary in code, never a translated text (the
// rule of AGENTS.md): pending, downloading, paused, muxing, done, error. The
// UI translates it for display, but nothing ever compares a translated
// string.
class DownloadInfo
{
public:
    QString videoId;
    QString title;
    QString channel;
    QString quality;        // "1080p", "vp9", ... (label only)
    QString status;         // the fixed vocabulary above
    double progress = 0.0;  // 0..1
    qint64 bytesDone = 0;
    qint64 bytesTotal = 0;
    QString filePath;       // the PARTIAL (a .ts per track): where the bytes are
    QString finalPath;      // the finished mp4
    QString error;
    QVector<int> parts;     // indexes of the segments already downloaded
    qlonglong positionMs = 0;  // where the local playback stopped
    qint64 bytesPerSecond = 0;  // the speed of the download, while it runs
    qlonglong createdAt = 0;
    qlonglong updatedAt = 0;

    bool isActive() const;  // downloading / paused / muxing
    bool isDone() const;     // the mp4 is there and usable
};

// Persists the downloads. The table has been in the database since the
// beginning of the app (empty, nobody used it) and the downloader extends it
// with the state it needs to RESUME: the bytes already on disk and the
// indexes of the segments already downloaded.
//
// The unit of resume is the SEGMENT (the master is already fragmented by
// YouTube with segments of ~1.3 MB, so each one fits in a single request),
// so the state is a set of segment indexes, not a set of byte offsets.
class DownloadRepo
{
public:
    explicit DownloadRepo(AppDatabase &db);

    bool upsert(const DownloadInfo &d);
    bool get(const QString &videoId, DownloadInfo &out) const;
    QVector<DownloadInfo> all() const;  // newest first
    bool remove(const QString &videoId);
    bool setStatus(const QString &videoId, const QString &status,
                   const QString &error = QString());
    bool setProgress(const QString &videoId, qint64 bytesDone,
                     qint64 bytesTotal);
    bool setParts(const QString &videoId, const QVector<int> &parts);
    bool setPosition(const QString &videoId, qlonglong positionMs);

private:
    DownloadInfo fromRow(const QSqlQuery &q) const;
    static QString partsToJson(const QVector<int> &parts);
    static QVector<int> partsFromJson(const QString &json);

    AppDatabase &m_db;
};

// Persistent Home cache per network section (Recomendado/Feed).
// Stores only ordered ids + fetched_at; the metadata lives in "videos"
// (VideoCacheRepo). saveSection REPLACES the whole section: each network
// response is the current list of that section, content is never appended to
// an old list (avoids duplicates and stale content). An empty list is REJECTED
// (a failed/empty update must not trample the previous cache nor stamp a new
// fetched_at).
class HomeCacheRepo
{
public:
    explicit HomeCacheRepo(AppDatabase &db);

    // Saves/replaces the section. Returns false (and writes nothing) if
    // videoIds is empty or it is not a valid section.
    bool saveSection(const QString &section, const QStringList &videoIds,
                     qint64 fetchedAt);
    QStringList sectionVideoIds(const QString &section) const;
    qint64 sectionFetchedAt(const QString &section) const;
    bool clearSection(const QString &section);

private:
    AppDatabase &m_db;
};

struct DiscoverRadio
{
    qlonglong id = 0;
    QString seedVideoId;
    qlonglong createdAt = 0;
    qlonglong lastUsedAt = 0;
};

// Radios saved from Descubre (a persistent source: they are not generated when
// opening Home, they are built while playing videos and stored here). The
// video metadata reuses the "videos" table (VideoCacheRepo).
class DiscoverRepo
{
public:
    explicit DiscoverRepo(AppDatabase &db);

    // Saves (or replaces) the radio of a seed: one radio per seed, it is not
    // duplicated if it already exists. Returns true if it ended up saved with
    // content.
    bool saveRadio(const QString &seedVideoId, const QVector<VideoInfo> &videos);
    bool hasRadio(const QString &seedVideoId) const;
    // Radios ordered by last_used_at ASC (least used first, for variety).
    QVector<DiscoverRadio> radios() const;
    // Video ids of a radio in position order. The metadata is resolved with
    // VideoCacheRepo::getMany over the "videos" cache.
    QStringList radioVideoIds(qlonglong radioId) const;
    // Video ids of the radio saved for a seed (position order), to reuse it in
    // "A continuación" without asking YouTube for it again.
    QStringList radioVideoIdsForSeed(const QString &seedVideoId) const;
    void markUsed(qlonglong radioId);
    bool deleteRadio(qlonglong radioId);
    int count() const;
    AppDatabase &db() const { return m_db; }

    // Transactional cleanup with HistoryRepo: when deleting a video from the
    // history, it deletes the radio whose seed is that video.
    void clearForSeed(const QString &seedVideoId);
    // When clearing the whole history, it deletes every radio.
    void clearAll();

private:
    AppDatabase &m_db;
};

// Video selection for Descubre using ONLY the stored radios (no network). It
// takes up to maxVideos from the least-used saved radios (shuffled): up to
// perRadio videos from each, without repeating ids and excluding excludeId.
// Radios without a useful video (deleted from "videos" or empty) are removed.
// Returns what it got: it may be < maxVideos if there is no valid content, an
// empty list if there are no radios.
QVector<VideoInfo> pickDiscoverVideos(DiscoverRepo &discover,
                                      VideoCacheRepo &cache,
                                      const QString &excludeId,
                                      int maxVideos = 5, int perRadio = 2);

bool databaseSelfTest(const QString &dbPath);
bool discoverSelfTest(const QString &dbPath);