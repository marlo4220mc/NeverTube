#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include "../models.h"

class AppDatabase;
class QSqlQuery;

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

private:
    VideoInfo fromRow(const QSqlQuery &q) const;

    AppDatabase &m_db;
};

class HistoryRepo
{
public:
    explicit HistoryRepo(AppDatabase &db);

    bool add(const QString &videoId, qlonglong positionMs = 0);
    bool remove(const QString &videoId);
    bool clear();
    QStringList recentIds(int limit = 50) const;
    qlonglong positionOf(const QString &videoId) const;

private:
    AppDatabase &m_db;
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

class SubscriptionsRepo
{
public:
    explicit SubscriptionsRepo(AppDatabase &db);

    bool add(const QString &channelId, const QString &title,
             const QString &avatarUrl = QString());
    bool remove(const QString &channelId);
    bool isSubscribed(const QString &channelId) const;
    QVector<SubscriptionInfo> all() const;

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

bool databaseSelfTest(const QString &dbPath);