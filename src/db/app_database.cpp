#include "app_database.h"

#include "../debug.h"
#include "../xdg_dirs.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

namespace {

// The folder that the OLD resolution used (Qt's DocumentsLocation) when it is not
// the one of the system AND it really has a database: that is what has to move.
QString legacyDatabaseFolder(const QString &systemDocs)
{
    const QString qtDocs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (qtDocs.isEmpty() || qtDocs == systemDocs)
        return QString();
    const QString legacy = qtDocs + QStringLiteral("/mini_tube");
    const QString db = legacy + QStringLiteral("/mini_tube.db");
    return QFileInfo::exists(db) ? legacy : QString();
}

} // namespace

bool migrateLegacyDatabase(const QString &from, const QString &to)
{
    if (from.isEmpty() || to.isEmpty() || from == to)
        return false;
    if (!QFileInfo(from).isDir() || QFileInfo::exists(to))
        return false;
    if (!QDir().mkpath(QFileInfo(to).absolutePath()))
        return false;
    if (!QDir().rename(from, to))
        return false;
    NT_DEBUG("E2E db de %s movida a %s\n", from.toUtf8().constData(),
             to.toUtf8().constData());
    return true;
}

AppDatabase::AppDatabase(const QString &explicitPath)
{
    if (!explicitPath.isEmpty()) {
        m_path = explicitPath;
        return;
    }
    // The documents folder of the SYSTEM, in any language. It is the SAME
    // resolution as the folder of the downloads (../xdg_dirs.h): plain
    // QStandardPaths answers the English "Documents" when XDG_CONFIG_HOME is
    // custom, and the database is the other thing the app writes in a folder of
    // the person.
    QString base = xdg::documentsFolder();
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (base.isEmpty())
        base = QDir::homePath();
    QString folder = base + QStringLiteral("/mini_tube");
    // A database that an old version left in the English "Documents" is MOVED
    // (once) to the folder of the system. If the move is not possible the old
    // folder is kept: the history is never lost.
    const QString legacy = legacyDatabaseFolder(base);
    if (!legacy.isEmpty() && !QFileInfo::exists(folder)
        && !migrateLegacyDatabase(legacy, folder))
        folder = legacy;
    m_path = folder + QStringLiteral("/mini_tube.db");
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
        // Two writers of the same file (two instances of the app, a copy of the
        // exe of another version, a database opened by a tool) got SQLITE_BUSY
        // AT ONCE and the write was lost. With a timeout the second one waits
        // instead of failing. SingleInstance keeps a second app from starting,
        // but it cannot cover everything: this is the safety net.
        if (!q.exec(QStringLiteral("PRAGMA busy_timeout=5000;")))
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

    // Migration from an old home_cache (one row per video, no fetched_at)
    // rejected by the new per-section schema. It is rebuildable cache:
    // if it exists without the fetched_at column it is dropped (once; with the
    // new schema the CREATE IF NOT EXISTS would never touch it again).
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
        "  description TEXT NOT NULL DEFAULT '',"
        "  is_live INTEGER NOT NULL DEFAULT 0,"
        "  is_short INTEGER NOT NULL DEFAULT 0"
        ")"));
    // Migration of the video cache from before is_live (live streams): the
    // new column is added with ALTER (cache with data, it is not dropped).
    if (tableExists(db, QStringLiteral("videos"))) {
        bool hasIsLive = false;
        QSqlQuery pv(db);
        if (pv.exec(QStringLiteral("PRAGMA table_info(videos)"))) {
            while (pv.next()) {
                if (pv.value(1).toString() == QLatin1String("is_live")) {
                    hasIsLive = true;
                    break;
                }
            }
        }
        if (!hasIsLive)
            exec(QStringLiteral(
                "ALTER TABLE videos ADD COLUMN is_live INTEGER NOT NULL DEFAULT 0"));
        // The same for the Shorts (the vertical videos): the lists that come
        // from the database leave them out.
        bool hasIsShort = false;
        QSqlQuery ps(db);
        if (ps.exec(QStringLiteral("PRAGMA table_info(videos)"))) {
            while (ps.next()) {
                if (ps.value(1).toString() == QLatin1String("is_short")) {
                    hasIsShort = true;
                    break;
                }
            }
        }
        if (!hasIsShort)
            exec(QStringLiteral(
                "ALTER TABLE videos ADD COLUMN is_short INTEGER NOT NULL DEFAULT 0"));
    }
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
    // Radios saved for Discover. NOT regenerated when entering Home: they are
    // built while playing videos (the player uses the seed as Yt::radioVideos
    // and whatever arrives via radioReady is saved here) and Discover only
    // consumes them. Video metadata reuses the "videos" table; here only the
    // ids and positions are stored.
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
    // Migration of the downloads table: it was created empty (with the
    // downloader not built yet) and the downloader needs to persist the
    // state of a download that is NOT just a fraction.
    //
    //  final_path   the finished mp4 (file_path is the PARTIAL, a .ts per
    //                track, that the mux reads)
    //  bytes_done   how many bytes are already on disk (the progress bar
    //                needs a real number, and a resumed download needs to
    //                know what is missing)
    //  bytes_total  the expected size, known BEFORE starting from the
    //                BANDWIDTH attribute of the master
    //  quality      "1080p", "vp9", ... for the row label
    //  parts        JSON with the indexes of the segments already
    //                downloaded: the unit of resume is the SEGMENT, so this
    //                is what makes "continue where it stopped" possible
    //  position_ms  where the local playback stopped (the local playback
    //                does NOT go through the history, which is about
    //                YouTube videos)
    //  updated_at   last change, to order the list
    if (tableExists(db, QStringLiteral("downloads"))) {
        struct Column
        {
            const char *name;
            const char *definition;
        };
        static const Column kNewColumns[] = {
            {"final_path", "TEXT"},
            {"bytes_done", "INTEGER NOT NULL DEFAULT 0"},
            {"bytes_total", "INTEGER NOT NULL DEFAULT 0"},
            {"quality", "TEXT"},
            {"parts", "TEXT"},
            {"position_ms", "INTEGER NOT NULL DEFAULT 0"},
            {"updated_at", "INTEGER NOT NULL DEFAULT 0"},
            {"bytes_per_second", "INTEGER NOT NULL DEFAULT 0"},
        };
        for (const Column &col : kNewColumns) {
            QSqlQuery pv(db);
            bool has = false;
            if (pv.exec(QStringLiteral("PRAGMA table_info(downloads)"))) {
                while (pv.next()) {
                    if (pv.value(1).toString() == QLatin1String(col.name)) {
                        has = true;
                        break;
                    }
                }
            }
            if (!has) {
                exec(QStringLiteral("ALTER TABLE downloads ADD COLUMN %1 %2")
                         .arg(QLatin1String(col.name),
                              QLatin1String(col.definition)));
            }
        }
    }
    exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS settings ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ")"));
    // Persistent per-section Home cache (smart refresh): only NETWORK sections
    // are stored (recommended/feed). Video metadata reuses the
    // "videos" table (VideoCacheRepo): here only ordered ids + the moment they
    // were saved (to decide the refresh after a restart). Continue watching is
    // derived from history and Discover from the saved radios, so neither of
    // the two is duplicated here.
    exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS home_cache ("
        "  section TEXT PRIMARY KEY,"
        "  video_ids TEXT NOT NULL,"
        "  fetched_at INTEGER NOT NULL"
        ")"));
}