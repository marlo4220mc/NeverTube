#include "app_database.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

AppDatabase::AppDatabase(const QString &explicitPath)
{
    if (!explicitPath.isEmpty()) {
        m_path = explicitPath;
        return;
    }
    const QString docs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString base = docs.isEmpty() ? QDir::homePath() : docs;
    m_path = base + QStringLiteral("/mini_tube/mini_tube.db");
}

AppDatabase::~AppDatabase()
{
    close();
}

QString AppDatabase::path() const
{
    return m_path;
}

QString AppDatabase::lastError() const
{
    return m_lastError;
}

QSqlDatabase AppDatabase::db() const
{
    return m_db;
}

bool AppDatabase::isOpen() const
{
    return m_db.isValid() && m_db.isOpen();
}

bool AppDatabase::open()
{
    if (isOpen())
        return true;

    const QFileInfo fi(m_path);
    if (!fi.dir().exists())
        QDir().mkpath(fi.dir().absolutePath());

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                     QStringLiteral("main"));
    m_db.setDatabaseName(m_path);
    if (!m_db.open()) {
        m_lastError = m_db.lastError().text();
        return false;
    }

    {
        QSqlQuery q(m_db);
        if (!q.exec(QStringLiteral("PRAGMA journal_mode=WAL;")))
            m_lastError = q.lastError().text();
        if (!q.exec(QStringLiteral("PRAGMA synchronous=NORMAL;")))
            m_lastError = q.lastError().text();
    }

    if (!tableExists(m_db, QStringLiteral("videos")))
        createSchema(m_db);

    return true;
}

void AppDatabase::close()
{
    if (m_db.isValid())
        m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(QStringLiteral("main"));
}

bool AppDatabase::tableExists(QSqlDatabase &db, const QString &name) const
{
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?"));
    q.addBindValue(QVariant(name));
    return q.exec() && q.next();
}

void AppDatabase::createSchema(QSqlDatabase &db)
{
    auto exec = [&db](const QString &sql) {
        QSqlQuery q(db);
        return q.exec(sql);
    };

    exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS videos ("
        "  id TEXT PRIMARY KEY,"
        "  title TEXT NOT NULL,"
        "  channel TEXT NOT NULL DEFAULT '',"
        "  channel_id TEXT NOT NULL DEFAULT '',"
        "  duration INTEGER NOT NULL DEFAULT 0,"
        "  views INTEGER NOT NULL DEFAULT 0,"
        "  published TEXT,"
        "  thumb_url TEXT,"
        "  description TEXT NOT NULL DEFAULT ''"
        ")"));
    exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS history ("
        "  video_id TEXT PRIMARY KEY,"
        "  watched_at INTEGER NOT NULL,"
        "  position_ms INTEGER NOT NULL DEFAULT 0"
        ")"));
    exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS favorites ("
        "  video_id TEXT PRIMARY KEY,"
        "  added_at INTEGER NOT NULL"
        ")"));
    exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS subscriptions ("
        "  channel_id TEXT PRIMARY KEY,"
        "  title TEXT NOT NULL,"
        "  avatar_url TEXT,"
        "  added_at INTEGER NOT NULL"
        ")"));
    exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS playlists ("
        "  id TEXT PRIMARY KEY,"
        "  title TEXT NOT NULL,"
        "  created_at INTEGER NOT NULL"
        ")"));
    exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS playlist_videos ("
        "  playlist_id TEXT NOT NULL,"
        "  video_id TEXT NOT NULL,"
        "  position INTEGER NOT NULL,"
        "  PRIMARY KEY (playlist_id, video_id)"
        ")"));
    exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS downloads ("
        "  id TEXT PRIMARY KEY,"
        "  video_id TEXT NOT NULL,"
        "  title TEXT NOT NULL,"
        "  channel TEXT NOT NULL DEFAULT '',"
        "  type TEXT NOT NULL,"
        "  status TEXT NOT NULL,"
        "  progress REAL NOT NULL DEFAULT 0,"
        "  file_path TEXT,"
        "  error TEXT,"
        "  created_at INTEGER NOT NULL"
        ")"));
    exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS settings ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ")"));
}