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

    // Migración de una home_cache antigua (fila por vídeo, sin fetched_at)
    // descartada por el nuevo esquema por-sección. Es caché reconstruible:
    // si existe sin la columna fetched_at se sustituye (una sola vez; con el
    // esquema nuevo el CREATE IF NOT EXISTS no la tocaría nunca más).
    if (tableExists(db, QStringLiteral("home_cache"))) {
        bool hasFetchedAt = false;
        QSqlQuery p(db);
        if (p.exec(QStringLiteral("PRAGMA table_info(home_cache)"))) {
            while (p.next()) {
                if (p.value(1).toString() == QLatin1String("fetched_at")) {
                    hasFetchedAt = true;
                    break;
                }
            }
        }
        if (!hasFetchedAt)
            exec(QStringLiteral("DROP TABLE home_cache"));
    }

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
        "CREATE TABLE IF NOT EXISTS starred ("
        "  video_id TEXT PRIMARY KEY,"
        "  added_at INTEGER NOT NULL"
        ")"));
    exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS watch_later ("
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
    // Radios guardadas para Descubre. NO se regeneran al entrar Home: se
    // construyen al reproducir vídeos (el reproductor usa la semilla como
    // Yt::radioVideos y lo que llega por radioReady se guarda aquí) y Descubre
    // solo las consume. Los metadatos de los vídeos reutilizan la tabla
    // "videos"; aquí se guardan solo los ids y posiciones.
    exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS discover_radios ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  seed_video_id TEXT NOT NULL UNIQUE,"
        "  created_at INTEGER NOT NULL,"
        "  last_used_at INTEGER NOT NULL DEFAULT 0"
        ")"));
    exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS discover_radio_videos ("
        "  radio_id INTEGER NOT NULL,"
        "  video_id TEXT NOT NULL,"
        "  position INTEGER NOT NULL,"
        "  PRIMARY KEY (radio_id, video_id)"
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
    // Caché persistente del Home por sección (refresco inteligente): solo se
    // guardan secciones de RED (recomendado/feed/tendencias). Los metadatos de
    // los vídeos reutilizan la tabla "videos" (VideoCacheRepo): aquí solo ids
    // ordenados + momento en que se guardaron (para decidir el refresco tras
    // reiniciar). Continuar viendo se deriva del historial y Descubre de las
    // radios guardadas, así que ninguna de las dos se duplica aquí.
    exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS home_cache ("
        "  section TEXT PRIMARY KEY,"
        "  video_ids TEXT NOT NULL,"
        "  fetched_at INTEGER NOT NULL"
        ")"));
}