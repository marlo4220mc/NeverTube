#include "repositories.h"

#include <cstdio>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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

    const auto tick = [&db](qlonglong &last) {
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
        " thumb_url, description) VALUES (?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(v.id);
    q.addBindValue(v.title);
    q.addBindValue(v.channel);
    q.addBindValue(v.channelId);
    q.addBindValue(v.durationSeconds);
    q.addBindValue(v.viewCount);
    q.addBindValue(v.publishedAt);
    q.addBindValue(v.thumbnailUrl);
    q.addBindValue(v.description);
    return q.exec();
}

bool VideoCacheRepo::get(const QString &id, VideoInfo &out) const
{
    QSqlQuery q(m_db.db());
    if (!q.prepare(QStringLiteral(
            "SELECT id, title, channel, channel_id, duration, views, "
            "published, thumb_url, description FROM videos WHERE id=? LIMIT 1")))
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
                             "views, published, thumb_url, description "
                             "FROM videos WHERE id IN (%1)")
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
    return v;
}

HistoryRepo::HistoryRepo(AppDatabase &db)
    : m_db(db)
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
    return q.exec();
}

bool HistoryRepo::remove(const QString &videoId)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM history WHERE video_id=?"));
    q.addBindValue(videoId);
    return q.exec();
}

bool HistoryRepo::clear()
{
    return exec(m_db.db(), QStringLiteral("DELETE FROM history"));
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

SubscriptionsRepo::SubscriptionsRepo(AppDatabase &db)
    : m_db(db)
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
    return q.exec();
}

bool SubscriptionsRepo::remove(const QString &channelId)
{
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("DELETE FROM subscriptions WHERE channel_id=?"));
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