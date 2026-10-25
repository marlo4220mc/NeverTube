#include "repositories.h"

#include <cstdio>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QThread>
#include <QVariant>

#include "app_database.h"

namespace {

bool exec(QSqlDatabase db, const QString &sql)
{
    QSqlQuery q(db);
    return q.exec(sql);
}

qlonglong nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace

bool databaseSelfTest(const QString &dbPath)
{
    if (dbPath.isEmpty())
        return false;

    const QStringList artifacts = {dbPath,
                                   dbPath + QLatin1String("-wal"),
                                   dbPath + QLatin1String("-shm")};
    for (const QString &f : artifacts) {
        if (QFileInfo::exists(f))
            QFile::remove(f);
    }

    AppDatabase db(dbPath);
    if (!db.open())
        return false;

    const auto yes = [&db](bool ok, const char *what) {
        if (!ok) {
            fprintf(stderr, "SELFTEST FAIL: %s (%s)\n", what,
                    db.lastError().toUtf8().constData());
        }
        return ok;
    };

    bool ok = true;

    VideoCacheRepo videoCache(db);
    HistoryRepo history(db);
    FavoritesRepo favorites(db);
    SubscriptionsRepo subscriptions(db);
    PlaylistsRepo playlists(db);
    SettingsRepo settings(db);

    const auto tick = [](qlonglong &last) {
        qlonglong now;
        do {
            now = nowMs();
        } while (now == last);
        last = now;
        QThread::msleep(2);
    };

    VideoInfo v;
    v.id = QStringLiteral("vidA");
    v.title = QStringLiteral("Título A");
    v.channel = QStringLiteral("Canal A");
    v.channelId = QStringLiteral("chanA");
    v.durationSeconds = 123;
    v.viewCount = 4567;
    v.publishedAt = QStringLiteral("2024-01-01T00:00:00Z");
    v.thumbnailUrl = QStringLiteral("/tmp/vidA.jpg");
    v.description = QStringLiteral("desc A");
    ok &= yes(videoCache.upsert(v), "cache upsert");

    VideoInfo out;
    ok &= yes(videoCache.get(QStringLiteral("vidA"), out), "cache get");
    ok &= yes(out.title == QStringLiteral("Título A")
                  && out.durationSeconds == 123 && out.viewCount == 4567
                  && out.thumbnailUrl == QLatin1String("/tmp/vidA.jpg"),
              "cache get fields");

    v.title = QStringLiteral("Título A updated");
    ok &= yes(videoCache.upsert(v), "cache upsert replace");
    ok &= yes(videoCache.get(QStringLiteral("vidA"), out)
                  && out.title == QStringLiteral("Título A updated"),
              "cache update");

    VideoInfo v2;
    v2.id = QStringLiteral("vidB");
    v2.title = QStringLiteral("Título B");
    v2.channel = QStringLiteral("Canal B");
    v2.durationSeconds = 42;
    ok &= yes(videoCache.upsert(v2), "cache upsert B");

    const QVector<VideoInfo> many =
        videoCache.getMany(QStringList() << QStringLiteral("vidA")
                                         << QStringLiteral("vidX")
                                         << QStringLiteral("vidB"));
    ok &= yes(many.size() == 2 && many[0].id == QLatin1String("vidA")
                  && many[1].id == QLatin1String("vidB"),
              "cache getMany order");

    // Live flag: survives upsert/read and updateLiveStatus clears it without
    // touching the rest of the fields.
    VideoInfo live;
    live.id = QStringLiteral("vidLive");
    live.title = QStringLiteral("Directo");
    live.channel = QStringLiteral("Canal L");
    live.durationSeconds = 0;
    live.isLive = true;
    ok &= yes(videoCache.upsert(live), "cache upsert live");
    VideoInfo liveOut;
    ok &= yes(videoCache.get(QStringLiteral("vidLive"), liveOut)
                  && liveOut.isLive && liveOut.title == QStringLiteral("Directo"),
              "cache live roundtrip");
    ok &= yes(videoCache.updateLiveStatus(QStringLiteral("vidLive"), false),
              "cache live status update");
    ok &= yes(videoCache.get(QStringLiteral("vidLive"), liveOut)
                  && !liveOut.isLive
                  && liveOut.title == QStringLiteral("Directo"),
              "cache live status cleared keeps fields");

    qlonglong lastTs = 0;
    tick(lastTs);
    ok &= yes(history.add(QStringLiteral("vidA"), 15000), "history add");
    tick(lastTs);
    ok &= yes(history.add(QStringLiteral("vidB")), "history add B");
    tick(lastTs);
    ok &= yes(history.add(QStringLiteral("vidA"), 30000), "history replace");
    ok &= yes(history.positionOf(QStringLiteral("vidA")) == 30000,
              "history position");
    ok &= yes(history.positionOf(QStringLiteral("vidX")) == 0, "history miss");
    const QStringList recent = history.recentIds(50);
    ok &= yes(recent.size() == 2 && recent.value(0) == QLatin1String("vidA")
                  && recent.value(1) == QLatin1String("vidB"),
              "history recent order");
    ok &= yes(history.remove(QStringLiteral("vidB"))
                  && history.recentIds(50).size() == 1,
              "history remove");
    ok &= yes(history.clear() && history.recentIds(50).isEmpty(), "history clear");

    ok &= yes(!favorites.isFavorite(QStringLiteral("vidA")), "fav initial");
    ok &= yes(favorites.toggle(QStringLiteral("vidA")), "fav toggle on");
    ok &= yes(favorites.isFavorite(QStringLiteral("vidA")), "fav isFav");
    ok &= yes(favorites.toggle(QStringLiteral("vidA")), "fav toggle off");
    ok &= yes(!favorites.isFavorite(QStringLiteral("vidA")), "fav removed");
    ok &= yes(favorites.toggle(QStringLiteral("vidB"))
                  && favorites.ids() == QStringList(QStringLiteral("vidB")),
              "fav ids");

    StarredRepo starred(db);
    ok &= yes(!starred.isStarred(QStringLiteral("vidA")), "star initial");
    ok &= yes(starred.toggle(QStringLiteral("vidA")), "star toggle on");
    ok &= yes(starred.isStarred(QStringLiteral("vidA")), "star isStarred");
    ok &= yes(starred.toggle(QStringLiteral("vidA")), "star toggle off");
    ok &= yes(!starred.isStarred(QStringLiteral("vidA")), "star removed");
    ok &= yes(starred.toggle(QStringLiteral("vidB"))
                  && starred.ids() == QStringList(QStringLiteral("vidB")),
              "star ids");

    WatchLaterRepo watchLater(db);
    ok &= yes(watchLater.toggle(QStringLiteral("vidA")), "wl toggle on");
    ok &= yes(watchLater.isWatchLater(QStringLiteral("vidA")), "wl is");
    ok &= yes(watchLater.toggle(QStringLiteral("vidA")), "wl toggle off");
    ok &= yes(!watchLater.isWatchLater(QStringLiteral("vidA")), "wl removed");
    ok &= yes(watchLater.toggle(QStringLiteral("vidB"))
                  && watchLater.ids().size() == 1
                  && watchLater.isWatchLater(QStringLiteral("vidB")),
              "wl ids");

    ok &= yes(subscriptions.add(QStringLiteral("chanA"),
                                QStringLiteral("Canal A"),
                                QStringLiteral("/tmp/a.png")),
              "sub add");
    ok &= yes(subscriptions.isSubscribed(QStringLiteral("chanA")), "sub isSub");
    ok &= yes(subscriptions.add(QStringLiteral("chanA"),
                                QStringLiteral("Canal A")),
              "sub add ignore dup");
    const auto subs = subscriptions.all();
    ok &= yes(subs.size() == 1 && subs[0].id == QLatin1String("chanA")
                  && subs[0].avatar == QLatin1String("/tmp/a.png"),
              "sub all");
    ok &= yes(subscriptions.remove(QStringLiteral("chanA"))
                  && !subscriptions.isSubscribed(QStringLiteral("chanA")),
              "sub remove");

    const QString pl = playlists.create(QStringLiteral("Mi lista"));
    ok &= yes(!pl.isEmpty(), "playlist create");
    tick(lastTs);
    const QString pl2 = playlists.create(QStringLiteral("Segunda"));
    ok &= yes(!pl2.isEmpty() && pl2 != pl, "playlist unique id");
    ok &= yes(playlists.rename(pl, QStringLiteral("Lista renombrada")), "playlist rename");
    const auto pls = playlists.all();
    const auto findPl = [&pls](const QString &id) -> const PlaylistItem * {
        for (const auto &p : pls)
            if (p.id == id)
                return &p;
        return nullptr;
    };
    ok &= yes(pls.size() == 2, "playlist all count");
    const PlaylistItem *renamed = findPl(pl);
    ok &= yes(renamed && renamed->title == QStringLiteral("Lista renombrada"),
              "playlist renamed stored");
    ok &= yes(playlists.addVideo(pl, QStringLiteral("vidA")), "pl add vA");
    ok &= yes(playlists.addVideo(pl, QStringLiteral("vidB")), "pl add vB");
    ok &= yes(!playlists.addVideo(pl, QStringLiteral("vidA")), "pl dup rejected");
    const QStringList plVideos = playlists.videoIds(pl);
    ok &= yes(plVideos == (QStringList()
                               << QStringLiteral("vidA")
                               << QStringLiteral("vidB")),
              "playlist order & dedup");
    ok &= yes(playlists.contains(pl, QStringLiteral("vidB"))
                  && !playlists.contains(pl, QStringLiteral("vidX")),
              "playlist contains");
    ok &= yes(playlists.removeVideo(pl, QStringLiteral("vidA"))
                  && playlists.videoIds(pl)
                         == QStringList(QStringLiteral("vidB")),
              "playlist removeVideo");
    ok &= yes(playlists.deletePlaylist(pl)
                  && !playlists.contains(pl, QStringLiteral("vidB"))
                  && playlists.all().size() == 1,
              "playlist delete");

    ok &= yes(settings.set(QStringLiteral("theme"), QStringLiteral("dark")),
              "settings set");
    QString sval;
    ok &= yes(settings.get(QStringLiteral("theme"), sval)
                  && sval == QLatin1String("dark"),
              "settings get");
    ok &= yes(!settings.get(QStringLiteral("nope"), sval), "settings null");
    ok &= yes(settings.getInt(QStringLiteral("quality"), 720) == 720,
              "settings int default");
    ok &= yes(settings.set(QStringLiteral("quality"), QStringLiteral("1080"))
                  && settings.getInt(QStringLiteral("quality"), 720) == 1080,
              "settings int stored");
    ok &= yes(settings.getString(QStringLiteral("missing"), QStringLiteral("x"))
                  == QLatin1String("x"),
              "settings string default");

    HomeCacheRepo homeCache(db);
    const QStringList secIds{QLatin1String("vidA"), QLatin1String("vidB")};
    ok &= yes(homeCache.saveSection(QStringLiteral("recommended"), secIds, 1111),
              "home cache save");
    ok &= yes(homeCache.sectionVideoIds(QStringLiteral("recommended")) == secIds,
              "home cache ids order");
    ok &= yes(homeCache.sectionFetchedAt(QStringLiteral("recommended")) == 1111,
              "home cache fetchedAt");
    // Smart refresh: replaces the whole section, never appends.
    const QStringList secIds2{QLatin1String("vidB")};
    ok &= yes(homeCache.saveSection(QStringLiteral("recommended"), secIds2, 2222),
              "home cache replace section");
    ok &= yes(homeCache.sectionVideoIds(QStringLiteral("recommended")) == secIds2
                  && homeCache.sectionVideoIds(QStringLiteral("trending")).isEmpty(),
              "home cache replaced (no merge, no leak to other sections)");
    ok &= yes(homeCache.sectionFetchedAt(QStringLiteral("recommended")) == 2222,
              "home cache replaced fetchedAt");
    // Sections are independent of one another.
    ok &= yes(homeCache.saveSection(QStringLiteral("trending"),
                                    QStringList{QLatin1String("vidX")}, 3333),
              "home cache second section");
    ok &= yes(homeCache.sectionVideoIds(QStringLiteral("recommended")) == secIds2
                  && homeCache.sectionFetchedAt(QStringLiteral("trending")) == 3333,
              "home cache sections independent");
    ok &= yes(homeCache.clearSection(QStringLiteral("trending"))
                  && homeCache.sectionVideoIds(QStringLiteral("trending")).isEmpty(),
              "home cache clear section");

    // Home reconstruction after a restart (fresh install included):
    // HomePage.saveSection persists the metadata in "videos" and only the ids
    // in home_cache; getMany(ids) must materialize the whole section again.
    VideoInfo h1;
    h1.id = QStringLiteral("homeVid1");
    h1.title = QStringLiteral("Home 1");
    h1.channel = QStringLiteral("Canal H");
    h1.durationSeconds = 60;
    h1.viewCount = 111;
    h1.thumbnailUrl = QStringLiteral("/tmp/h1.jpg");
    VideoInfo h2;
    h2.id = QStringLiteral("homeVid2");
    h2.title = QStringLiteral("Home 2");
    h2.channel = QStringLiteral("Canal H");
    h2.durationSeconds = 90;
    h2.viewCount = 222;
    h2.thumbnailUrl = QStringLiteral("/tmp/h2.jpg");
    ok &= yes(videoCache.upsert(h1) && videoCache.upsert(h2),
              "home cache metadata upsert");
    ok &= yes(homeCache.saveSection(
                  QStringLiteral("recommended"),
                  QStringList{QStringLiteral("homeVid1"),
                              QStringLiteral("homeVid2")},
                  4444),
              "home cache save with metadata");
    const QVector<VideoInfo> reconstructed = videoCache.getMany(
        homeCache.sectionVideoIds(QStringLiteral("recommended")));
    ok &= yes(reconstructed.size() == 2
                  && reconstructed[0].id == QLatin1String("homeVid1")
                  && reconstructed[0].title == QLatin1String("Home 1")
                  && reconstructed[0].durationSeconds == 60
                  && reconstructed[0].thumbnailUrl == QLatin1String("/tmp/h1.jpg")
                  && reconstructed[1].id == QLatin1String("homeVid2")
                  && reconstructed[1].title == QLatin1String("Home 2")
                  && reconstructed[1].viewCount == 222,
              "home cache reconstruction (ids + metadata)");

    // Failed/empty update: saving empty is REJECTED and does not overwrite the
    // saved section nor its fetched_at.
    const qint64 savedAt = homeCache.sectionFetchedAt(QStringLiteral("recommended"));
    ok &= yes(!homeCache.saveSection(QStringLiteral("recommended"),
                                     QStringList(), 9999),
              "home cache empty save rejected");
    ok &= yes(homeCache.sectionVideoIds(QStringLiteral("recommended"))
                      == QStringList({QLatin1String("homeVid1"),
                                      QLatin1String("homeVid2")})
                  && homeCache.sectionFetchedAt(QStringLiteral("recommended"))
                         == savedAt,
              "home cache empty save keeps ids and fetchedAt");

    // --- the downloads repo ---
    // It has to survive a round trip with the state a RESUMED download
    // needs: the bytes on disk and the indexes of the segments already
    // fetched (the unit of resume is the segment, not a byte offset).
    DownloadRepo downloads(db);
    {
        DownloadInfo d;
        d.videoId = QLatin1String("dlVid1");
        d.title = QLatin1String("Video de prueba");
        d.channel = QLatin1String("Canal");
        d.quality = QLatin1String("1080p");
        d.status = QLatin1String("downloading");
        d.progress = 0.25;
        d.bytesDone = 25;
        d.bytesTotal = 100;
        d.filePath = QLatin1String("/tmp/dl/v.ts");
        d.createdAt = 1000;
        d.updatedAt = 2000;
        d.parts = {0, 1, 2, 5};
        ok &= yes(downloads.upsert(d), "download upsert");

        DownloadInfo got;
        ok &= yes(downloads.get(QLatin1String("dlVid1"), got),
                  "download get");
        ok &= yes(got.title == QLatin1String("Video de prueba")
                      && got.channel == QLatin1String("Canal")
                      && got.quality == QLatin1String("1080p")
                      && got.status == QLatin1String("downloading")
                      && got.bytesDone == 25 && got.bytesTotal == 100
                      && got.filePath == QLatin1String("/tmp/dl/v.ts")
                      && got.parts.size() == 4 && got.parts.at(0) == 0
                      && got.parts.at(3) == 5,
                  "download round trip (estado y partes)");
        ok &= yes(got.isActive() && !got.isDone(),
                  "un download en curso esta activo y no terminado");

        // The progress is stored as a fraction AND as bytes: the bar needs
        // the bytes and the resume needs to know what is missing.
        ok &= yes(downloads.setProgress(QLatin1String("dlVid1"), 50, 200),
                  "download setProgress");
        ok &= yes(downloads.get(QLatin1String("dlVid1"), got)
                      && got.bytesDone == 50 && got.bytesTotal == 200
                      && qAbs(got.progress - 0.25) < 0.001,
                  "download progress (bytes y fraccion)");
        // A total of zero must not divide by zero.
        ok &= yes(downloads.setProgress(QLatin1String("dlVid1"), 10, 0),
                  "download setProgress sin total");
        ok &= yes(downloads.get(QLatin1String("dlVid1"), got)
                      && got.progress == 0.0,
                  "download progress sin total no revienta");

        ok &= yes(downloads.setParts(QLatin1String("dlVid1"), {1, 3, 4}),
                  "download setParts");
        ok &= yes(downloads.get(QLatin1String("dlVid1"), got)
                      && got.parts.size() == 3 && got.parts.at(1) == 3,
                  "download partes (los indices de los segmentos hechos)");

        ok &= yes(downloads.setStatus(QLatin1String("dlVid1"),
                                      QLatin1String("error"),
                                      QLatin1String("sin ffmpeg")),
                  "download setStatus");
        ok &= yes(downloads.get(QLatin1String("dlVid1"), got)
                      && got.status == QLatin1String("error")
                      && got.error == QLatin1String("sin ffmpeg"),
                  "download error guardado");
        ok &= yes(!got.isActive() && !got.isDone(),
                  "un download con error no esta activo ni terminado");

        // The list is newest first.
        DownloadInfo d2;
        d2.videoId = QLatin1String("dlVid2");
        d2.title = QLatin1String("Otro");
        d2.status = QLatin1String("done");
        d2.createdAt = 5000;
        ok &= yes(downloads.upsert(d2), "download upsert 2");
        const QVector<DownloadInfo> list = downloads.all();
        ok &= yes(list.size() == 2 && list.at(0).videoId == QLatin1String("dlVid2"),
                  "download list (mas nuevo primero)");

        ok &= yes(downloads.remove(QLatin1String("dlVid1")),
                  "download remove");
        ok &= yes(downloads.all().size() == 1, "download remove (queda uno)");
        ok &= yes(!downloads.get(QLatin1String("dlVid1"), got),
                  "download get de uno borrado");
    }

    db.close();
    if (ok)
        fprintf(stderr, "SELFTEST OK\n");
    else
        fprintf(stderr, "SELFTEST FAILED\n");
    return ok;
}

VideoCacheRepo::VideoCacheRepo(AppDatabase &db)
    : m_db(db)
{
}

bool VideoCacheRepo::upsert(const VideoInfo &v)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO videos "
        "(id, title, channel, channel_id, duration, views, published, "
        " thumb_url, description, is_live, is_short) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(v.id);
    q.addBindValue(v.title);
    q.addBindValue(v.channel);
    q.addBindValue(v.channelId);
    q.addBindValue(v.durationSeconds);
    q.addBindValue(v.viewCount);
    q.addBindValue(v.publishedAt);
    q.addBindValue(v.thumbnailUrl);
    q.addBindValue(v.description);
    q.addBindValue(v.isLive ? 1 : 0);
    q.addBindValue(v.isShort ? 1 : 0);
    return q.exec();
}

bool VideoCacheRepo::get(const QString &id, VideoInfo &out) const
{
    QSqlQuery q(m_db.db());
    if (!q.prepare(QStringLiteral(
            "SELECT id, title, channel, channel_id, duration, views, "
            "published, thumb_url, description, is_live, is_short "
            "FROM videos WHERE id=? LIMIT 1")))
        return false;
    q.addBindValue(id);
    if (!q.exec() || !q.next())
        return false;
    out = fromRow(q);
    return true;
}

QVector<VideoInfo> VideoCacheRepo::getMany(const QStringList &ids) const
{
    QVector<VideoInfo> result;
    if (ids.isEmpty())
        return result;

    QStringList marks;
    for (int i = 0; i < ids.size(); ++i)
        marks << QStringLiteral("?");
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT id, title, channel, channel_id, duration, "
                             "views, published, thumb_url, description, is_live, "
                             "is_short FROM videos WHERE id IN (%1)")
                  .arg(marks.join(QStringLiteral(","))));
    for (const QString &id : ids)
        q.addBindValue(id);
    if (!q.exec())
        return result;

    QHash<QString, VideoInfo> byId;
    while (q.next())
        byId.insert(q.value(0).toString(), fromRow(q));

    result.reserve(ids.size());
    for (const QString &id : ids) {
        const auto it = byId.constFind(id);
        if (it != byId.constEnd())
            result.append(it.value());
    }
    return result;
}

VideoInfo VideoCacheRepo::fromRow(const QSqlQuery &q) const
{
    VideoInfo v;
    v.id = q.value(0).toString();
    v.title = q.value(1).toString();
    v.channel = q.value(2).toString();
    v.channelId = q.value(3).toString();
    v.durationSeconds = q.value(4).toInt();
    v.viewCount = q.value(5).toInt();
    v.publishedAt = q.value(6).toString();
    v.thumbnailUrl = q.value(7).toString();
    v.description = q.value(8).toString();
    v.isLive = q.value(9).toBool();
    v.isShort = q.value(10).toBool();
    return v;
}

// Live re-check when re-entering a page: clears (or leaves set) the cached
// live flag without touching the rest of the fields, which is what the /next
// refresh needs (it does not bring a duration).
bool VideoCacheRepo::updateLiveStatus(const QString &id, bool isLive)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE videos SET is_live=? WHERE id=?"));
    q.addBindValue(isLive ? 1 : 0);
    q.addBindValue(id);
    return q.exec();
}

HistoryRepo::HistoryRepo(AppDatabase &db, DiscoverRepo *discover,
                         QObject *parent)
    : QObject(parent), m_db(db), m_discover(discover)
{
}

bool HistoryRepo::add(const QString &videoId, qlonglong positionMs)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO history (video_id, watched_at, position_ms) "
        "VALUES (?,?,?)"));
    q.addBindValue(videoId);
    q.addBindValue(nowMs());
    q.addBindValue(positionMs);
    const bool ok = q.exec();
    // Notify the views (e.g. Home "Continuar viendo" and its empty state) even
    // if the insert is a position UPDATE: the watched_at order changed and the
    // list may have been reordered.
    emit changed();
    return ok;
}

bool HistoryRepo::remove(const QString &videoId)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM history WHERE video_id=?"));
    q.addBindValue(videoId);
    if (!q.exec())
        return false;

    // If the video is removed from the history, its Descubre radio is removed
    // too (only if the seed is that video, not if it appears as content).
    if (m_discover)
        m_discover->clearForSeed(videoId);

    emit changed();
    return true;
}

bool HistoryRepo::clear()
{
    if (!exec(m_db.db(), QStringLiteral("DELETE FROM history")))
        return false;

    // When clearing the whole history, all Descubre radios are removed.
    if (m_discover)
        m_discover->clearAll();

    emit changed();
    return true;
}

QStringList HistoryRepo::recentIds(int limit) const
{
    QStringList ids;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "SELECT video_id FROM history ORDER BY watched_at DESC LIMIT ?"));
    q.addBindValue(QVariant(limit));
    if (!q.exec())
        return ids;
    while (q.next())
        ids << q.value(0).toString();
    return ids;
}

qlonglong HistoryRepo::positionOf(const QString &videoId) const
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "SELECT position_ms FROM history WHERE video_id=? LIMIT 1"));
    q.addBindValue(videoId);
    if (!q.exec() || !q.next())
        return 0;
    return q.value(0).toLongLong();
}

// A video is considered COMPLETED if ≥95% of its duration was watched, or if
// 15 seconds or less remain (end/credits). Relative rule instead of a fixed
// threshold: it scales with the duration.
constexpr double kCompletedFraction = 0.95;
constexpr qlonglong kCompletedRemainMs = 15000;

bool HistoryRepo::isCompleted(const VideoInfo &v, qlonglong positionMs)
{
    if (positionMs <= 0 || v.durationSeconds <= 0)
        return false;
    const qlonglong durMs = qlonglong(v.durationSeconds) * 1000;
    const bool watchedEnough = positionMs >= durMs * kCompletedFraction;
    if (watchedEnough)
        return true;
    return durMs - positionMs <= kCompletedRemainMs;
}

qlonglong HistoryRepo::resumeStartMs(const VideoInfo &v, qlonglong positionMs)
{
    if (positionMs <= 0)
        return 0;
    if (HistoryRepo::isCompleted(v, positionMs))
        return 0;
    return positionMs;
}

double HistoryRepo::progressFraction(const VideoInfo &v, qlonglong positionMs)
{
    if (positionMs <= 0 || v.durationSeconds <= 0)
        return 0.0;
    if (HistoryRepo::isCompleted(v, positionMs))
        return 0.0;
    const double f = double(positionMs) / (double(v.durationSeconds) * 1000.0);
    if (f <= 0.0 || f >= 1.0)
        return 0.0;
    return f;
}

FavoritesRepo::FavoritesRepo(AppDatabase &db)
    : m_db(db)
{
}

bool FavoritesRepo::toggle(const QString &videoId)
{
    if (isFavorite(videoId)) {
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral("DELETE FROM favorites WHERE video_id=?"));
        q.addBindValue(videoId);
        return q.exec();
    }
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "INSERT INTO favorites (video_id, added_at) VALUES (?,?)"));
    q.addBindValue(videoId);
    q.addBindValue(nowMs());
    return q.exec();
}

bool FavoritesRepo::isFavorite(const QString &videoId) const
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "SELECT 1 FROM favorites WHERE video_id=? LIMIT 1"));
    q.addBindValue(videoId);
    return q.exec() && q.next();
}

QStringList FavoritesRepo::ids() const
{
    QStringList ids;
    QSqlQuery q(m_db.db());
    if (!q.exec(QStringLiteral(
            "SELECT video_id FROM favorites ORDER BY added_at DESC")))
        return ids;
    while (q.next())
        ids << q.value(0).toString();
    return ids;
}

StarredRepo::StarredRepo(AppDatabase &db)
    : m_db(db)
{
}

bool StarredRepo::toggle(const QString &videoId)
{
    if (isStarred(videoId))
        return remove(videoId);
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "INSERT INTO starred (video_id, added_at) VALUES (?,?)"));
    q.addBindValue(videoId);
    q.addBindValue(nowMs());
    return q.exec();
}

bool StarredRepo::isStarred(const QString &videoId) const
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "SELECT 1 FROM starred WHERE video_id=? LIMIT 1"));
    q.addBindValue(videoId);
    return q.exec() && q.next();
}

QStringList StarredRepo::ids() const
{
    QStringList ids;
    QSqlQuery q(m_db.db());
    if (!q.exec(QStringLiteral(
            "SELECT video_id FROM starred ORDER BY added_at DESC")))
        return ids;
    while (q.next())
        ids << q.value(0).toString();
    return ids;
}

bool StarredRepo::remove(const QString &videoId)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM starred WHERE video_id=?"));
    q.addBindValue(videoId);
    return q.exec();
}

WatchLaterRepo::WatchLaterRepo(AppDatabase &db)
    : m_db(db)
{
}

bool WatchLaterRepo::toggle(const QString &videoId)
{
    if (isWatchLater(videoId))
        return remove(videoId);
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "INSERT INTO watch_later (video_id, added_at) VALUES (?,?)"));
    q.addBindValue(videoId);
    q.addBindValue(nowMs());
    return q.exec();
}

bool WatchLaterRepo::isWatchLater(const QString &videoId) const
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "SELECT 1 FROM watch_later WHERE video_id=? LIMIT 1"));
    q.addBindValue(videoId);
    return q.exec() && q.next();
}

QStringList WatchLaterRepo::ids() const
{
    QStringList ids;
    QSqlQuery q(m_db.db());
    if (!q.exec(QStringLiteral(
            "SELECT video_id FROM watch_later ORDER BY added_at DESC")))
        return ids;
    while (q.next())
        ids << q.value(0).toString();
    return ids;
}

bool WatchLaterRepo::remove(const QString &videoId)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM watch_later WHERE video_id=?"));
    q.addBindValue(videoId);
    return q.exec();
}

SubscriptionsRepo::SubscriptionsRepo(AppDatabase &db, QObject *parent)
    : QObject(parent), m_db(db)
{
}

bool SubscriptionsRepo::add(const QString &channelId, const QString &title,
                            const QString &avatarUrl)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO subscriptions "
        "(channel_id, title, avatar_url, added_at) VALUES (?,?,?,?)"));
    q.addBindValue(channelId);
    q.addBindValue(title);
    q.addBindValue(avatarUrl);
    q.addBindValue(nowMs());
    const bool ok = q.exec();
    if (ok)
        emit changed();
    return ok;
}

bool SubscriptionsRepo::remove(const QString &channelId)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM subscriptions WHERE channel_id=?"));
    q.addBindValue(channelId);
    const bool ok = q.exec();
    if (ok)
        emit changed();
    return ok;
}

bool SubscriptionsRepo::setAvatar(const QString &channelId,
                                  const QString &avatarUrl)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "UPDATE subscriptions SET avatar_url=? WHERE channel_id=?"));
    q.addBindValue(avatarUrl);
    q.addBindValue(channelId);
    return q.exec();
}

bool SubscriptionsRepo::isSubscribed(const QString &channelId) const
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "SELECT 1 FROM subscriptions WHERE channel_id=? LIMIT 1"));
    q.addBindValue(channelId);
    return q.exec() && q.next();
}

QVector<SubscriptionInfo> SubscriptionsRepo::all() const
{
    QVector<SubscriptionInfo> result;
    QSqlQuery q(m_db.db());
    if (!q.exec(QStringLiteral(
            "SELECT channel_id, title, avatar_url FROM subscriptions "
            "ORDER BY added_at ASC")))
        return result;
    while (q.next()) {
        SubscriptionInfo s;
        s.id = q.value(0).toString();
        s.title = q.value(1).toString();
        s.avatar = q.value(2).toString();
        result.append(s);
    }
    return result;
}

PlaylistsRepo::PlaylistsRepo(AppDatabase &db)
    : m_db(db)
{
}

QString PlaylistsRepo::create(const QString &title)
{
    const QString id = QString::number(nowMs());
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "INSERT INTO playlists (id, title, created_at) VALUES (?,?,?)"));
    q.addBindValue(id);
    q.addBindValue(title);
    q.addBindValue(nowMs());
    if (!q.exec())
        return QString();
    return id;
}

bool PlaylistsRepo::deletePlaylist(const QString &id)
{
    QSqlQuery q1(m_db.db());
    q1.prepare(QStringLiteral("DELETE FROM playlist_videos WHERE playlist_id=?"));
    q1.addBindValue(id);
    if (!q1.exec())
        return false;
    QSqlQuery q2(m_db.db());
    q2.prepare(QStringLiteral("DELETE FROM playlists WHERE id=?"));
    q2.addBindValue(id);
    return q2.exec();
}

bool PlaylistsRepo::rename(const QString &id, const QString &title)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("UPDATE playlists SET title=? WHERE id=?"));
    q.addBindValue(title);
    q.addBindValue(id);
    return q.exec();
}

QVector<PlaylistItem> PlaylistsRepo::all() const
{
    QVector<PlaylistItem> result;
    QSqlQuery q(m_db.db());
    if (!q.exec(QStringLiteral(
            "SELECT id, title FROM playlists ORDER BY created_at DESC")))
        return result;
    while (q.next()) {
        PlaylistItem p;
        p.id = q.value(0).toString();
        p.title = q.value(1).toString();
        result.append(p);
    }
    return result;
}

bool PlaylistsRepo::contains(const QString &playlistId,
                             const QString &videoId) const
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "SELECT 1 FROM playlist_videos WHERE playlist_id=? AND video_id=? "
        "LIMIT 1"));
    q.addBindValue(playlistId);
    q.addBindValue(videoId);
    return q.exec() && q.next();
}

bool PlaylistsRepo::addVideo(const QString &playlistId, const QString &videoId)
{
    qlonglong pos = 0;
    {
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral(
            "SELECT COALESCE(MAX(position),0) FROM playlist_videos "
            "WHERE playlist_id=?"));
        q.addBindValue(playlistId);
        if (q.exec() && q.next())
            pos = q.value(0).toLongLong();
    }
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "INSERT INTO playlist_videos (playlist_id, video_id, position) "
        "VALUES (?,?,?)"));
    q.addBindValue(playlistId);
    q.addBindValue(videoId);
    q.addBindValue(pos + 1);
    return q.exec();
}

bool PlaylistsRepo::removeVideo(const QString &playlistId,
                                const QString &videoId)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "DELETE FROM playlist_videos WHERE playlist_id=? AND video_id=?"));
    q.addBindValue(playlistId);
    q.addBindValue(videoId);
    return q.exec();
}

QStringList PlaylistsRepo::videoIds(const QString &playlistId) const
{
    QStringList ids;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "SELECT video_id FROM playlist_videos WHERE playlist_id=? "
        "ORDER BY position ASC"));
    q.addBindValue(playlistId);
    if (!q.exec())
        return ids;
    while (q.next())
        ids << q.value(0).toString();
    return ids;
}

SettingsRepo::SettingsRepo(AppDatabase &db)
    : m_db(db)
{
}

bool SettingsRepo::get(const QString &key, QString &out) const
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT value FROM settings WHERE key=? LIMIT 1"));
    q.addBindValue(key);
    if (!q.exec() || !q.next())
        return false;
    out = q.value(0).toString();
    return true;
}

bool SettingsRepo::set(const QString &key, const QString &value)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO settings (key, value) VALUES (?,?)"));
    q.addBindValue(key);
    q.addBindValue(value);
    return q.exec();
}

qlonglong SettingsRepo::getInt(const QString &key, qlonglong def) const
{
    QString v;
    if (!get(key, v))
        return def;
    bool ok = false;
    const qlonglong parsed = v.toLongLong(&ok);
    return ok ? parsed : def;
}

QString SettingsRepo::getString(const QString &key, const QString &def) const
{
    QString v;
    if (!get(key, v))
        return def;
    return v;
}

DownloadRepo::DownloadRepo(AppDatabase &db)
    : m_db(db)
{
}

bool DownloadInfo::isActive() const
{
    return status == QLatin1String("downloading")
        || status == QLatin1String("paused")
        || status == QLatin1String("muxing");
}

bool DownloadInfo::isDone() const
{
    return status == QLatin1String("done");
}

QString DownloadRepo::partsToJson(const QVector<int> &parts)
{
    QJsonArray arr;
    for (int p : parts)
        arr.append(p);
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QVector<int> DownloadRepo::partsFromJson(const QString &json)
{
    QVector<int> out;
    if (json.isEmpty())
        return out;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray())
        return out;
    const QJsonArray arr = doc.array();
    for (const QJsonValue &v : arr) {
        // The video indexes are their own index (0, 1, 2...) and the audio
        // ones are -(index + 2), so the 0 of the video and the 0 of the
        // audio do not collide and neither of them is lost.
        const int n = v.toInt(0);
        if (n >= 0 || n <= -2)
            out.append(n);
    }
    return out;
}

// Binds a text that is never NULL: a null QString would become a NULL in
// SQLite and the NOT NULL columns of `downloads` would reject it.
static void bindText(QSqlQuery &q, const QString &value)
{
    // QString("") (empty) is NOT the same as QString() (null): the first
    // is bound as an empty string, the second as a NULL.
    q.addBindValue(value.isNull() ? QStringLiteral("") : value);
}

bool DownloadRepo::upsert(const DownloadInfo &d)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO downloads "
        "(id, video_id, title, channel, type, status, progress, file_path, "
        " error, created_at, final_path, bytes_done, bytes_total, quality, "
        " parts, position_ms, updated_at, bytes_per_second) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    bindText(q, d.videoId);
    bindText(q, d.videoId);
    bindText(q, d.title);
    bindText(q, d.channel);
    bindText(q, QStringLiteral("video"));
    bindText(q, d.status);
    q.addBindValue(d.progress);
    bindText(q, d.filePath);
    bindText(q, d.error);
    q.addBindValue(d.createdAt);
    bindText(q, d.finalPath);
    q.addBindValue(d.bytesDone);
    q.addBindValue(d.bytesTotal);
    bindText(q, d.quality);
    bindText(q, partsToJson(d.parts));
    q.addBindValue(d.positionMs);
    q.addBindValue(d.updatedAt);
    q.addBindValue(d.bytesPerSecond);
    return q.exec();
}

DownloadInfo DownloadRepo::fromRow(const QSqlQuery &q) const
{
    DownloadInfo d;
    d.videoId = q.value(0).toString();
    d.title = q.value(1).toString();
    d.channel = q.value(2).toString();
    d.status = q.value(3).toString();
    d.progress = q.value(4).toDouble();
    d.filePath = q.value(5).toString();
    d.error = q.value(6).toString();
    d.createdAt = q.value(7).toLongLong();
    d.finalPath = q.value(8).toString();
    d.bytesDone = q.value(9).toLongLong();
    d.bytesTotal = q.value(10).toLongLong();
    d.quality = q.value(11).toString();
    d.parts = partsFromJson(q.value(12).toString());
    d.positionMs = q.value(13).toLongLong();
    d.updatedAt = q.value(14).toLongLong();
    d.bytesPerSecond = q.value(15).toLongLong();
    return d;
}

bool DownloadRepo::get(const QString &videoId, DownloadInfo &out) const
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "SELECT video_id, title, channel, status, progress, file_path, error, "
        "created_at, final_path, bytes_done, bytes_total, quality, parts, "
        "position_ms, updated_at, bytes_per_second "
        "FROM downloads WHERE video_id=? LIMIT 1"));
    q.addBindValue(videoId);
    if (!q.exec() || !q.next())
        return false;
    out = fromRow(q);
    return true;
}

QVector<DownloadInfo> DownloadRepo::all() const
{
    QVector<DownloadInfo> out;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "SELECT video_id, title, channel, status, progress, file_path, error, "
        "created_at, final_path, bytes_done, bytes_total, quality, parts, "
        "position_ms, updated_at, bytes_per_second "
        "FROM downloads ORDER BY created_at DESC"));
    if (!q.exec())
        return out;
    while (q.next())
        out.append(fromRow(q));
    return out;
}

bool DownloadRepo::remove(const QString &videoId)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM downloads WHERE video_id=?"));
    q.addBindValue(videoId);
    return q.exec();
}

bool DownloadRepo::setStatus(const QString &videoId, const QString &status,
                             const QString &error)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "UPDATE downloads SET status=?, error=?, updated_at=? WHERE video_id=?"));
    q.addBindValue(status);
    q.addBindValue(error);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(videoId);
    return q.exec();
}

bool DownloadRepo::setProgress(const QString &videoId, qint64 bytesDone,
                               qint64 bytesTotal)
{
    const double frac = bytesTotal > 0
        ? qBound(0.0, double(bytesDone) / double(bytesTotal), 1.0)
        : 0.0;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "UPDATE downloads SET bytes_done=?, bytes_total=?, progress=?, "
        "updated_at=? WHERE video_id=?"));
    q.addBindValue(bytesDone);
    q.addBindValue(bytesTotal);
    q.addBindValue(frac);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(videoId);
    return q.exec();
}

bool DownloadRepo::setParts(const QString &videoId, const QVector<int> &parts)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "UPDATE downloads SET parts=?, updated_at=? WHERE video_id=?"));
    q.addBindValue(partsToJson(parts));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(videoId);
    return q.exec();
}

bool DownloadRepo::setPosition(const QString &videoId, qlonglong positionMs)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "UPDATE downloads SET position_ms=?, updated_at=? WHERE video_id=?"));
    q.addBindValue(positionMs);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(videoId);
    return q.exec();
}

HomeCacheRepo::HomeCacheRepo(AppDatabase &db)
    : m_db(db)
{
}

bool HomeCacheRepo::saveSection(const QString &section,
                                const QStringList &videoIds, qint64 fetchedAt)
{
    // Empty = failed/empty update: it must not replace the previous cache nor
    // set a new fetched_at (it is a failure, not a new state).
    if (section.isEmpty() || videoIds.isEmpty())
        return false;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO home_cache (section, video_ids, fetched_at) "
        "VALUES (:section, :ids, :at)"));
    q.bindValue(QStringLiteral(":section"), section);
    q.bindValue(QStringLiteral(":ids"), videoIds.join(QLatin1Char(',')));
    q.bindValue(QStringLiteral(":at"), fetchedAt);
    return q.exec();
}

QStringList HomeCacheRepo::sectionVideoIds(const QString &section) const
{
    QStringList out;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "SELECT video_ids FROM home_cache WHERE section = :section"));
    q.bindValue(QStringLiteral(":section"), section);
    if (!q.exec() || !q.next())
        return out;
    const QString ids = q.value(0).toString();
    if (!ids.isEmpty())
        out = ids.split(QLatin1Char(','), Qt::SkipEmptyParts);
    return out;
}

qint64 HomeCacheRepo::sectionFetchedAt(const QString &section) const
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "SELECT fetched_at FROM home_cache WHERE section = :section"));
    q.bindValue(QStringLiteral(":section"), section);
    if (!q.exec() || !q.next())
        return 0;
    return q.value(0).toLongLong();
}

bool HomeCacheRepo::clearSection(const QString &section)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "DELETE FROM home_cache WHERE section = :section"));
    q.bindValue(QStringLiteral(":section"), section);
    return q.exec();
}

DiscoverRepo::DiscoverRepo(AppDatabase &db)
    : m_db(db)
{
}

bool DiscoverRepo::saveRadio(const QString &seedVideoId,
                             const QVector<VideoInfo> &videos)
{
    if (seedVideoId.isEmpty() || seedVideoId.size() != 11 || videos.isEmpty())
        return false;

    qlonglong radioId = 0;
    {
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral(
            "SELECT id FROM discover_radios WHERE seed_video_id=? LIMIT 1"));
        q.addBindValue(seedVideoId);
        if (q.exec() && q.next())
            radioId = q.value(0).toLongLong();
    }

    if (radioId == 0) {
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral(
            "INSERT INTO discover_radios (seed_video_id, created_at, "
            "last_used_at) VALUES (?,?,0)"));
        q.addBindValue(seedVideoId);
        q.addBindValue(nowMs());
        if (!q.exec())
            return false;
        radioId = q.lastInsertId().toLongLong();
    } else {
        // The radio already exists: guard so as not to overwrite or duplicate.
        return false;
    }
    if (radioId <= 0)
        return false;

    int pos = 0;
    QSet<QString> seen;
    for (const VideoInfo &v : videos) {
        if (v.id.size() != 11 || seen.contains(v.id))
            continue;
        seen.insert(v.id);
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral(
            "INSERT INTO discover_radio_videos (radio_id, video_id, position) "
            "VALUES (?,?,?)"));
        q.addBindValue(radioId);
        q.addBindValue(v.id);
        q.addBindValue(++pos);
        q.exec();
    }
    return pos > 0;
}

bool DiscoverRepo::hasRadio(const QString &seedVideoId) const
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "SELECT 1 FROM discover_radios WHERE seed_video_id=? LIMIT 1"));
    q.addBindValue(seedVideoId);
    return q.exec() && q.next();
}

QVector<DiscoverRadio> DiscoverRepo::radios() const
{
    QVector<DiscoverRadio> result;
    QSqlQuery q(m_db.db());
    if (!q.exec(QStringLiteral(
            "SELECT id, seed_video_id, created_at, last_used_at "
            "FROM discover_radios ORDER BY last_used_at ASC")))
        return result;
    while (q.next()) {
        DiscoverRadio r;
        r.id = q.value(0).toLongLong();
        r.seedVideoId = q.value(1).toString();
        r.createdAt = q.value(2).toLongLong();
        r.lastUsedAt = q.value(3).toLongLong();
        result.append(r);
    }
    return result;
}

QStringList DiscoverRepo::radioVideoIds(qlonglong radioId) const
{
    QStringList ids;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "SELECT video_id FROM discover_radio_videos WHERE radio_id=? "
        "ORDER BY position ASC"));
    q.addBindValue(radioId);
    if (!q.exec())
        return ids;
    while (q.next())
        ids << q.value(0).toString();
    return ids;
}

QStringList DiscoverRepo::radioVideoIdsForSeed(const QString &seedVideoId) const
{
    QStringList ids;
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "SELECT drv.video_id FROM discover_radio_videos drv "
        "JOIN discover_radios dr ON dr.id = drv.radio_id "
        "WHERE dr.seed_video_id=? ORDER BY drv.position ASC"));
    q.addBindValue(seedVideoId);
    if (!q.exec())
        return ids;
    while (q.next())
        ids << q.value(0).toString();
    return ids;
}

void DiscoverRepo::markUsed(qlonglong radioId)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral(
        "UPDATE discover_radios SET last_used_at=? WHERE id=?"));
    q.addBindValue(nowMs());
    q.addBindValue(radioId);
    q.exec();
}

bool DiscoverRepo::deleteRadio(qlonglong radioId)
{
    {
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral(
            "DELETE FROM discover_radio_videos WHERE radio_id=?"));
        q.addBindValue(radioId);
        q.exec();
    }
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM discover_radios WHERE id=?"));
    q.addBindValue(radioId);
    return q.exec();
}

int DiscoverRepo::count() const
{
    QSqlQuery q(m_db.db());
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM discover_radios"))
        || !q.next())
        return 0;
    return q.value(0).toInt();
}

void DiscoverRepo::clearForSeed(const QString &seedVideoId)
{
    {
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral(
            "DELETE FROM discover_radio_videos WHERE radio_id IN ("
            "SELECT id FROM discover_radios WHERE seed_video_id=?)"));
        q.addBindValue(seedVideoId);
        q.exec();
    }
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM discover_radios WHERE seed_video_id=?"));
    q.addBindValue(seedVideoId);
    q.exec();
}

void DiscoverRepo::clearAll()
{
    exec(m_db.db(), QStringLiteral("DELETE FROM discover_radio_videos"));
    exec(m_db.db(), QStringLiteral("DELETE FROM discover_radios"));
}

QVector<VideoInfo> pickDiscoverVideos(DiscoverRepo &discover,
                                      VideoCacheRepo &cache,
                                      const QString &excludeId, int maxVideos,
                                      int perRadio)
{
    QVector<VideoInfo> picked;
    const QVector<DiscoverRadio> radios = discover.radios();
    if (radios.isEmpty())
        return picked;

    // Exclude radios whose seed is no longer in the history (extra safety).
    QSet<QString> seedEnHistorial;
    {
        QStringList ids;
        for (const DiscoverRadio &r : radios)
            ids << r.seedVideoId;
        QSqlQuery q(discover.db().db());
        if (ids.isEmpty())
            return picked;
        QStringList marks;
        for (int i = 0; i < ids.size(); ++i)
            marks << QStringLiteral("?");
        q.prepare(QStringLiteral(
            "SELECT video_id FROM history WHERE video_id IN (%1)")
                      .arg(marks.join(QStringLiteral(","))));
        for (const QString &id : ids)
            q.addBindValue(id);
        if (q.exec())
            while (q.next())
                seedEnHistorial << q.value(0).toString();
    }

    // Variety: from the least-used radios (the most recent windows) a random
    // sample is shuffled. Thus the same ones are not always repeated nor regenerated.
    const int n = qMin(radios.size(), 8);
    QVector<DiscoverRadio> candidates;
    for (int i = 0; i < n; ++i)
        candidates.append(radios.at(i));
    for (int i = candidates.size() - 1; i > 0; --i) {
        const int j = QRandomGenerator::global()->bounded(i + 1);
        candidates.swapItemsAt(i, j);
    }

    for (const DiscoverRadio &r : candidates) {
        if (picked.size() >= maxVideos)
            break;
        // Skip if the seed is no longer in the history (safety measure).
        if (!seedEnHistorial.contains(r.seedVideoId)) {
            discover.deleteRadio(r.id);
            continue;
        }
        const QStringList ids = discover.radioVideoIds(r.id);
        if (ids.isEmpty()) {
            discover.deleteRadio(r.id);
            continue;
        }
        const QVector<VideoInfo> vids = cache.getMany(ids);
        if (vids.isEmpty()) {
            discover.deleteRadio(r.id);
            continue;
        }
        // Exclude radios whose seed is no longer in the history (extra safety).
        QSqlQuery q(discover.db().db());
        q.prepare(QStringLiteral("SELECT 1 FROM history WHERE video_id=?"));
        q.addBindValue(r.seedVideoId);
        if (!q.exec() || !q.next()) {
            discover.deleteRadio(r.id);
            continue;
        }
        discover.markUsed(r.id);
        int added = 0;
        for (const VideoInfo &v : vids) {
            if (picked.size() >= maxVideos || added >= perRadio)
                break;
            if (!excludeId.isEmpty() && v.id == excludeId)
                continue;
            bool dup = false;
            for (const VideoInfo &p : picked) {
                if (p.id == v.id) {
                    dup = true;
                    break;
                }
            }
            if (dup)
                continue;
            picked.append(v);
            ++added;
        }
    }
    return picked;
}

bool discoverSelfTest(const QString &dbPath)
{
    if (dbPath.isEmpty())
        return false;

    const QStringList artifacts = {dbPath,
                                   dbPath + QLatin1String("-wal"),
                                   dbPath + QLatin1String("-shm")};
    for (const QString &f : artifacts) {
        if (QFileInfo::exists(f))
            QFile::remove(f);
    }

    const auto yes = [](bool ok, const char *what) {
        if (!ok)
            fprintf(stderr, "SELFTEST FAIL: %s\n", what);
        return ok;
    };

    bool ok = true;

    const auto mkVideo = [](int n, const QString &title) {
        VideoInfo v;
        v.id = QStringLiteral("video%1").arg(n, 6, 10, QLatin1Char('0'));
        v.title = title;
        v.channel = QStringLiteral("Canal %1").arg(n);
        return v;
    };

    AppDatabase db(dbPath);
    if (!db.open())
        return yes(false, "discover db open");
    DiscoverRepo discover(db);
    VideoCacheRepo cache(db);
    HistoryRepo history(db, &discover);

    // Metadata in the cache so the radios are resolvable.
    for (int i = 0; i < 12; ++i)
        cache.upsert(mkVideo(i, QStringLiteral("Título %1").arg(i)));

    ok &= yes(discover.count() == 0, "discover empieza vacío");

    QVector<VideoInfo> radioA;
    for (int i = 0; i < 4; ++i)
        radioA.append(mkVideo(i, QStringLiteral("Título %1").arg(i)));
    ok &= yes(discover.saveRadio(QStringLiteral("seedAAAAAAA"), radioA),
              "discover save radio");
    ok &= yes(discover.hasRadio(QStringLiteral("seedAAAAAAA")),
              "discover hasRadio");
    ok &= yes(discover.count() == 1, "discover count");
    // The seed must be in the history: the pickDiscoverVideos filter only uses
    // radios whose seed video is still in the history.
    history.add(QStringLiteral("seedAAAAAAA"));

    const auto radiosA = discover.radios();
    ok &= yes(!radiosA.isEmpty(), "discover radios list");
    const QStringList idsA = radiosA.isEmpty()
        ? QStringList()
        : discover.radioVideoIds(radiosA.first().id);
    ok &= yes(idsA.size() == 4
                  && idsA[0] == QLatin1String("video000000")
                  && idsA[3] == QLatin1String("video000003"),
              "discover radio ids orden");
    ok &= yes(discover.radioVideoIdsForSeed(QStringLiteral("seedAAAAAAA"))
                      == idsA,
              "discover radio ids por semilla (reuso)");

    const int savedBefore = discover.count();
    ok &= yes(!discover.saveRadio(QStringLiteral("seedAAAAAAA"),
                                  radioA.mid(0, 3)),
              "discover re-save misma semilla rechazado");
    ok &= yes(discover.count() == savedBefore,
              "discover sin duplicar semilla");
    const auto radiosAfterResave = discover.radios();
    ok &= yes(!radiosAfterResave.isEmpty()
                  && discover.radioVideoIds(radiosAfterResave.first().id)
                             .size() == 4,
              "discover re-save NO reemplaza contenido");
    discover.saveRadio(QStringLiteral("seedAAAAAAA"), radioA);

    const auto radiosMark = discover.radios();
    ok &= yes(!radiosMark.isEmpty(), "discover radios mark base");
    if (!radiosMark.isEmpty()) {
        const DiscoverRadio r0 = radiosMark.first();
        ok &= yes(r0.lastUsedAt == 0, "discover last_used_at inicial 0");
        QThread::msleep(2); // avoids a millisecond collision with created_at
        discover.markUsed(r0.id);
        const DiscoverRadio r0b = discover.radios().first();
        ok &= yes(r0b.lastUsedAt > r0.createdAt, "discover markUsed");
    }

    const QVector<VideoInfo> sola =
        pickDiscoverVideos(discover, cache, QString());
    ok &= yes(sola.size() == 2, "discover 1 radio -> hasta 2 vídeos");

    // Raising an exception and repeated selection must not add radios.
    ok &= yes(pickDiscoverVideos(discover, cache, QString()).size() == 2,
              "discover re-selección reutiliza radios");

    // Seeds B and C: radios with partially different content.
    QVector<VideoInfo> radioB;
    radioB.append(mkVideo(2, QStringLiteral("Título 2")));
    radioB.append(mkVideo(3, QStringLiteral("Título 3")));
    radioB.append(mkVideo(4, QStringLiteral("Título 4")));
    radioB.append(mkVideo(5, QStringLiteral("Título 5")));
    ok &= yes(discover.saveRadio(QStringLiteral("seedBBBBBBB"), radioB),
              "discover save radio B");
    QVector<VideoInfo> radioC;
    radioC.append(mkVideo(6, QStringLiteral("Título 6")));
    radioC.append(mkVideo(7, QStringLiteral("Título 7")));
    radioC.append(mkVideo(8, QStringLiteral("Título 8")));
    ok &= yes(discover.saveRadio(QStringLiteral("seedCCCCCCC"), radioC),
              "discover save radio C");
    history.add(QStringLiteral("seedBBBBBBB"));
    history.add(QStringLiteral("seedCCCCCCC"));

    const int count3 = discover.count();
    QVector<VideoInfo> picks;
    QSet<QString> seenPicks;
    bool dupes = false;
    for (int round = 0; round < 3; ++round) {
        picks = pickDiscoverVideos(discover, cache, QStringLiteral("video000000"));
        ok &= yes(picks.size() <= 5, "discover máximo 5");
        QSet<QString> roundSeen;
        for (const VideoInfo &v : picks) {
            if (roundSeen.contains(v.id))
                dupes = true;
            roundSeen.insert(v.id);
            if (v.id == QLatin1String("video000000"))
                dupes = true; // the excluded one must not appear
        }
        seenPicks.unite(roundSeen);
        ok &= yes(discover.count() == count3,
                  "discover re-uso no genera radios");
    }
    ok &= yes(!dupes, "discover sin duplicados ni excluido");

    // 1-2 per radio rule: with 3 radios and perRadio=2 there must be 4-5.
    ok &= yes(picks.size() >= 4, "discover 3 radios -> 4-5 vídeos");

    // Persistence: close and reopen the database.
    db.close();
    AppDatabase db2(dbPath);
    if (!db2.open())
        return yes(false, "discover db reopen");
    DiscoverRepo discover2(db2);
    ok &= yes(discover2.count() == count3,
              "discover persiste tras reiniciar");
    ok &= yes(discover2.hasRadio(QStringLiteral("seedAAAAAAA")),
              "discover radio persistida");

    // ── History ↔ Descubre integration ──────────────────────────────────
    DiscoverRepo purger(db2);
    VideoCacheRepo cache2(db2);
    HistoryRepo history2(db2, &purger);

    // 1) Deleting a video that appears INSIDE other radios does not remove
    //    them. "video000003" is in A and in B; the radio (seed) is what counts.
    ok &= yes(history2.remove(QStringLiteral("video000003")),
              "history intern remove");
    ok &= yes(purger.count() == count3,
              "discover intacta si se borra vídeo interno");
    ok &= yes(purger.hasRadio(QStringLiteral("seedBBBBBBB"))
                  && purger.hasRadio(QStringLiteral("seedCCCCCCC")),
              "radios internas siguen tras borrar vídeo interno");

    // 2) Deleting the SEED video removes only its own radio.
    ok &= yes(history2.remove(QStringLiteral("seedAAAAAAA")),
              "history seed remove");
    ok &= yes(!purger.hasRadio(QStringLiteral("seedAAAAAAA")),
              "radio del seed eliminada");
    ok &= yes(purger.count() == count3 - 1,
              "solo se elimina la radio del seed");
    ok &= yes(purger.hasRadio(QStringLiteral("seedBBBBBBB"))
                  && purger.hasRadio(QStringLiteral("seedCCCCCCC")),
              "otras radios intactas");
    // No orphan records after deleting the seed radio.
    QSqlQuery orphanSeed(db2.db());
    ok &= yes(orphanSeed.exec(QStringLiteral(
                  "SELECT COUNT(*) FROM discover_radio_videos "
                  "WHERE radio_id NOT IN (SELECT id FROM discover_radios)"))
                  && orphanSeed.next()
                  && orphanSeed.value(0).toInt() == 0,
              "limpieza sin huérfanos");

    // 3) Descubre does not use deleted radios again (selection after deletion).
    QVector<VideoInfo> picksPostA;
    for (int round = 0; round < 2; ++round) {
        picksPostA = pickDiscoverVideos(purger, cache2, QString());
        ok &= yes(discover2.count() == count3 - 1,
                  "borrado no genera radios nuevas");
    }
    ok &= yes(picksPostA.size() >= 2, "descubre aún selecciona de B/C");

    // 4) Deleting ALL the history leaves Descubre empty and with no records.
    ok &= yes(history2.clear(), "history clear");
    ok &= yes(purger.count() == 0, "descubre vacío tras borrar historial");
    QSqlQuery orphanCheck(db2.db());
    ok &= yes(orphanCheck.exec(
                  QStringLiteral("SELECT COUNT(*) FROM discover_radio_videos"))
                  && orphanCheck.next()
                  && orphanCheck.value(0).toInt() == 0,
              "discover_radio_videos vacío tras borrar historial");
    ok &= yes(pickDiscoverVideos(purger, cache2, QString()).isEmpty(),
              "descubre no usa radios eliminadas");
    ok &= yes(purger.count() == 0, "borrado no genera radios");

    // A broken radio (videos with no metadata in the cache) is removed on selection.
    QVector<VideoInfo> radioRota;
    radioRota.append(mkVideo(9, QStringLiteral("Título 9")));
    ok &= yes(purger.saveRadio(QStringLiteral("seedDDDDDDD"), radioRota),
              "discover save radio D");
    history2.add(QStringLiteral("seedDDDDDDD")); // valid seed: only the metadata is missing
    // The metadata cache of radio D is emptied to simulate an invalid one.
    QSqlQuery del(db2.db());
    del.exec(QStringLiteral("DELETE FROM videos WHERE id='video000009'"));
    const int beforeInvalid = purger.count();
    pickDiscoverVideos(purger, cache2, QString());
    ok &= yes(purger.count() == beforeInvalid - 1,
              "discover radio inválida se descarta");

    return ok;
}