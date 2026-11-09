#include "data_transfer.h"

#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVector>

#include "db/app_database.h"
#include "db/repositories.h"

#include "miniz.h"

#include <cstdio>
#include <cstring>

// Data transfer (export / import): pure parsing and formatting. No repos, no
// DB, no network. See the header for the format contract (NewPipe JSON +
// Google Takeout CSV/ZIP).

namespace data_transfer
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Extracts the channel id from a NewPipe-style URL
// ("https://www.youtube.com/channel/<id>"). Returns empty when not found.
static QString channelIdFromUrl(const QString &url)
{
    const QString marker = QLatin1String("/channel/");
    const int i = url.indexOf(marker);
    if (i < 0)
        return QString();
    const QString tail = url.mid(i + marker.size());
    const int slash = tail.indexOf(QLatin1Char('/'));
    const QString id = slash < 0 ? tail : tail.left(slash);
    // A channel id is 24 characters (e.g. "UCsXVk37bltHxD1rDPwtNM8Q").
    if (id.size() != 24)
        return QString();
    return id;
}

// Splits a CSV line into fields, respecting simple double-quoted fields.
static QStringList splitCsvLine(const QString &line)
{
    QStringList out;
    QString cur;
    bool inQuotes = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (c == QLatin1Char('"')) {
            inQuotes = !inQuotes;
        } else if (c == QLatin1Char(',') && !inQuotes) {
            out << cur.trimmed();
            cur.clear();
        } else {
            cur += c;
        }
    }
    out << cur.trimmed();
    return out;
}

// The base name (without the directory) of a path inside a zip.
static QString baseName(const QString &path)
{
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}

// ---------------------------------------------------------------------------
// EXPORT
// ---------------------------------------------------------------------------

QByteArray exportJson(const QVector<Subscription> &subscriptions,
                      const NevertubeData &nevertube,
                      const QString &appVersion, int appVersionInt)
{
    QJsonArray subs;
    for (const Subscription &s : subscriptions) {
        QJsonObject o;
        o.insert(QStringLiteral("service_id"), s.serviceId);
        o.insert(QStringLiteral("url"), s.url);
        o.insert(QStringLiteral("name"), s.name);
        subs.append(o);
    }

    QJsonObject root;
    root.insert(QStringLiteral("app_version"), appVersion);
    root.insert(QStringLiteral("app_version_int"), appVersionInt);
    root.insert(QStringLiteral("subscriptions"), subs);

    // The "nevertube" block: NewPipe's ImportExportJsonHelper ignores it (it
    // only reads "subscriptions"), so the file stays a valid NewPipe backup
    // while carrying NeverTube's own data for a full reinstall.
    QJsonObject nt;
    if (!nevertube.playlists.isEmpty()) {
        QJsonArray pls;
        for (const Playlist &p : nevertube.playlists) {
            QJsonObject po;
            po.insert(QStringLiteral("title"), p.title);
            QJsonArray videos;
            for (const QString &id : p.videoIds)
                videos.append(id);
            po.insert(QStringLiteral("videos"), videos);
            pls.append(po);
        }
        nt.insert(QStringLiteral("playlists"), pls);
    }
    if (!nevertube.watchLater.isEmpty()) {
        QJsonArray wl;
        for (const QString &id : nevertube.watchLater)
            wl.append(id);
        nt.insert(QStringLiteral("watch_later"), wl);
    }
    if (!nevertube.historyBreaks.isEmpty()) {
        QJsonArray h;
        for (const HistoryEntry &e : nevertube.historyBreaks) {
            QJsonObject ho;
            ho.insert(QStringLiteral("video_id"), e.videoId);
            ho.insert(QStringLiteral("position_ms"), e.positionMs);
            h.append(ho);
        }
        nt.insert(QStringLiteral("history"), h);
    }
    if (!nevertube.favorites.isEmpty()) {
        QJsonArray f;
        for (const QString &id : nevertube.favorites)
            f.append(id);
        nt.insert(QStringLiteral("favorites"), f);
    }
    if (!nevertube.starred.isEmpty()) {
        QJsonArray s;
        for (const QString &id : nevertube.starred)
            s.append(id);
        nt.insert(QStringLiteral("starred"), s);
    }
    root.insert(QStringLiteral("nevertube"), nt);

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

// ---------------------------------------------------------------------------
// IMPORT — NewPipe / NeverTube JSON
// ---------------------------------------------------------------------------

ImportResult importNewpipeJson(const QByteArray &json)
{
    ImportResult res;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (doc.isNull() || !doc.isObject()) {
        res.error = err.errorString().isEmpty()
                        ? QStringLiteral("invalid json")
                        : err.errorString();
        return res;
    }
    const QJsonObject root = doc.object();

    // Subscriptions: the only thing NewPipe reads.
    const QJsonArray subs = root.value(QStringLiteral("subscriptions")).toArray();
    for (const QJsonValue &v : subs) {
        const QJsonObject o = v.toObject();
        if (o.isEmpty())
            continue;
        Subscription s;
        s.serviceId = o.value(QStringLiteral("service_id")).toInt();
        s.url = o.value(QStringLiteral("url")).toString();
        s.name = o.value(QStringLiteral("name")).toString();
        if (s.url.isEmpty())
            continue;
        res.subscriptions.append(s);
    }

    // The NeverTube block (ignored by NewPipe).
    const QJsonObject nt = root.value(QStringLiteral("nevertube")).toObject();
    if (!nt.isEmpty()) {
        res.hasNevertube = true;
        const QJsonArray pls = nt.value(QStringLiteral("playlists")).toArray();
        for (const QJsonValue &pv : pls) {
            const QJsonObject po = pv.toObject();
            Playlist p;
            p.title = po.value(QStringLiteral("title")).toString();
            const QJsonArray videos = po.value(QStringLiteral("videos")).toArray();
            for (const QJsonValue &vid : videos) {
                const QString id = vid.toString();
                if (!id.isEmpty())
                    p.videoIds << id;
            }
            if (!p.title.isEmpty() || !p.videoIds.isEmpty())
                res.nevertube.playlists.append(p);
        }
        const QJsonArray wl = nt.value(QStringLiteral("watch_later")).toArray();
        for (const QJsonValue &v : wl) {
            const QString id = v.toString();
            if (!id.isEmpty())
                res.nevertube.watchLater << id;
        }
        const QJsonArray h = nt.value(QStringLiteral("history")).toArray();
        for (const QJsonValue &hv : h) {
            const QJsonObject ho = hv.toObject();
            HistoryEntry e;
            e.videoId = ho.value(QStringLiteral("video_id")).toString();
            e.positionMs = ho.value(QStringLiteral("position_ms")).toVariant().toLongLong();
            if (!e.videoId.isEmpty())
                res.nevertube.historyBreaks.append(e);
        }
        const QJsonArray f = nt.value(QStringLiteral("favorites")).toArray();
        for (const QJsonValue &v : f) {
            const QString id = v.toString();
            if (!id.isEmpty())
                res.nevertube.favorites << id;
        }
        const QJsonArray s = nt.value(QStringLiteral("starred")).toArray();
        for (const QJsonValue &v : s) {
            const QString id = v.toString();
            if (!id.isEmpty())
                res.nevertube.starred << id;
        }
    }

    return res;
}

// ---------------------------------------------------------------------------
// IMPORT — Google Takeout subscriptions CSV (loose file)
// ---------------------------------------------------------------------------

// The header of the CSV varies by locale, but the columns are ALWAYS in the
// order Channel Id, Channel Url, Channel Title (the same trick
// YoutubeSubscriptionExtractor uses). So we skip the header and read columns
// 0, 1, 2 of every row.
ImportResult importSubscriptionsCsv(const QByteArray &csv)
{
    ImportResult res;
    const QString text = QString::fromUtf8(csv);
    const QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.isEmpty()) {
        res.error = QStringLiteral("empty file");
        return res;
    }
    // Skip the header line (line 0).
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList fields = splitCsvLine(lines.at(i));
        if (fields.size() < 3)
            continue;
        QString url = fields.at(1);
        // Normalize http:// to https://, like NewPipe does.
        if (url.startsWith(QLatin1String("http://")))
            url = QLatin1String("https://") + url.mid(7);
        if (!url.startsWith(QLatin1String("https://www.youtube.com/channel/")))
            continue;
        if (channelIdFromUrl(url).size() != 24)
            continue;
        Subscription s;
        s.serviceId = 0;  // YouTube
        s.url = url;
        s.name = fields.at(2);
        res.subscriptions.append(s);
    }
    if (res.subscriptions.isEmpty() && res.error.isEmpty())
        res.warning = QStringLiteral("no subscriptions found");
    return res;
}

// ---------------------------------------------------------------------------
// IMPORT — Google Takeout playlist / watch later CSV
// ---------------------------------------------------------------------------

ImportResult importPlaylistCsv(const QByteArray &csv, const QString &fileName)
{
    ImportResult res;
    const QString text = QString::fromUtf8(csv);
    const QStringList lines = text.split(QLatin1Char('\n'));
    if (lines.size() < 2)
        return res;

    // Exclude Takeout metadata index files (playlists.csv, Listas de reproducción.csv)
    const QString header = lines.at(0).toLower();
    if (header.contains(QLatin1String("playlist id"))
        || header.contains(QString::fromUtf8("id de lista")))
        return res;

    // Clean title from fileName by stripping ".csv" and "-videos" / "-vídeos"
    QString title = baseName(fileName);
    if (title.endsWith(QLatin1String(".csv"), Qt::CaseInsensitive))
        title.chop(4);
    if (title.endsWith(QString::fromUtf8("-vídeos"), Qt::CaseInsensitive))
        title.chop(7);
    else if (title.endsWith(QLatin1String("-videos"), Qt::CaseInsensitive))
        title.chop(7);

    QStringList ids;
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList f = splitCsvLine(lines.at(i));
        if (!f.isEmpty()) {
            const QString id = f.at(0).trimmed();
            if (!id.isEmpty())
                ids << id;
        }
    }
    if (ids.isEmpty())
        return res;

    const QString lowerTitle = title.toLower();
    if (lowerTitle == QLatin1String("watch later")
        || lowerTitle.startsWith(QLatin1String("watch later"))
        || lowerTitle.contains(QLatin1String("ver m"))
        || lowerTitle.contains(QLatin1String("ver mas"))) {
        res.nevertube.watchLater = ids;
        res.hasNevertube = true;
    } else if (lowerTitle == QLatin1String("liked videos")
               || lowerTitle.startsWith(QLatin1String("liked videos"))
               || lowerTitle.contains(QLatin1String("me gusta"))
               || lowerTitle.contains(QLatin1String("favorit"))) {
        res.nevertube.favorites = ids;
        res.hasNevertube = true;
    } else {
        Playlist p;
        p.title = title.trimmed();
        p.videoIds = ids;
        res.nevertube.playlists.append(p);
        res.hasNevertube = true;
    }
    return res;
}

// ---------------------------------------------------------------------------
// IMPORT — Google Takeout .zip
// ---------------------------------------------------------------------------

ImportResult importTakeoutZip(const QByteArray &zipData)
{
    ImportResult res;
    if (zipData.isEmpty()) {
        res.error = QStringLiteral("empty zip data");
        return res;
    }

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zipData.constData(), zipData.size(), 0)) {
        res.error = QStringLiteral("cannot open zip");
        return res;
    }

    const int fileCount = static_cast<int>(mz_zip_reader_get_num_files(&zip));

    // 1. Subscriptions: content-based detection across all CSV files in the zip.
    // Handles English (subscriptions.csv), Spanish (suscripciones.csv), French
    // (abonnements.csv), etc., picking the file with the most subscriptions.
    for (int i = 0; i < fileCount; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory)
            continue;
        const QString filePath = QString::fromUtf8(stat.m_filename);
        if (!filePath.endsWith(QLatin1String(".csv"), Qt::CaseInsensitive))
            continue;
        const QString lower = filePath.toLower();
        if (lower.contains(QLatin1String("playlist")) || lower.contains(QLatin1String("lista"))
            || lower.contains(QLatin1String("metadatos")) || lower.contains(QLatin1String("comentario"))
            || lower.contains(QLatin1String("historial")) || lower.contains(QLatin1String("canal")))
            continue;

        size_t uncompSize = 0;
        void *data = mz_zip_reader_extract_to_heap(&zip, i, &uncompSize, 0);
        if (data) {
            const QByteArray csvBytes(static_cast<const char *>(data), static_cast<int>(uncompSize));
            mz_free(data);
            const ImportResult subs = importSubscriptionsCsv(csvBytes);
            if (subs.subscriptions.size() > res.subscriptions.size())
                res.subscriptions = subs.subscriptions;
        }
    }
    // Fallback: search any CSV whose filename contains subscription hints
    if (res.subscriptions.isEmpty()) {
        for (int i = 0; i < fileCount; ++i) {
            mz_zip_archive_file_stat stat;
            if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory)
                continue;
            const QString filePath = QString::fromUtf8(stat.m_filename);
            if (!filePath.endsWith(QLatin1String(".csv"), Qt::CaseInsensitive))
                continue;
            const QString b = baseName(filePath).toLower();
            if (b.contains(QLatin1String("subscri")) || b.contains(QLatin1String("suscrip"))
                || b.contains(QLatin1String("abonn"))) {
                size_t uncompSize = 0;
                void *data = mz_zip_reader_extract_to_heap(&zip, i, &uncompSize, 0);
                if (data) {
                    const QByteArray csvBytes(static_cast<const char *>(data), static_cast<int>(uncompSize));
                    mz_free(data);
                    const ImportResult subs = importSubscriptionsCsv(csvBytes);
                    if (subs.subscriptions.size() > res.subscriptions.size())
                        res.subscriptions = subs.subscriptions;
                }
            }
        }
    }

    // 2. Playlists & Watch later: every CSV under any directory containing
    // "playlist" or "lista" (e.g. "playlists/" or "listas de reproducción/").
    for (int i = 0; i < fileCount; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory)
            continue;
        const QString filePath = QString::fromUtf8(stat.m_filename);
        if (!filePath.endsWith(QLatin1String(".csv"), Qt::CaseInsensitive))
            continue;
        const QString path = filePath;
        const QString lower = path.toLower();
        if (!lower.contains(QLatin1String("playlist")) && !lower.contains(QLatin1String("lista")))
            continue;

        size_t uncompSize = 0;
        void *data = mz_zip_reader_extract_to_heap(&zip, i, &uncompSize, 0);
        if (data) {
            const QByteArray content(static_cast<const char *>(data), static_cast<int>(uncompSize));
            mz_free(data);
            const ImportResult plRes = importPlaylistCsv(content, baseName(path));
            if (plRes.hasNevertube) {
                res.nevertube.playlists.append(plRes.nevertube.playlists);
                res.nevertube.watchLater.append(plRes.nevertube.watchLater);
                res.nevertube.favorites.append(plRes.nevertube.favorites);
                res.hasNevertube = true;
            }
        }
    }

    mz_zip_reader_end(&zip);

    const bool hasSubs = !res.subscriptions.isEmpty();
    const bool hasPlaylists = res.hasNevertube;
    if (!hasSubs && !hasPlaylists)
        res.warning = QStringLiteral("no subscriptions or playlists found in the zip");
    else if (!hasSubs)
        res.warning = QStringLiteral("no subscriptions found in the zip; imported playlists");

    return res;
}

// ---------------------------------------------------------------------------
// IMPORT — dispatcher
// ---------------------------------------------------------------------------

ImportResult importFile(const QByteArray &data, const QString &fileName)
{
    const QString lowerName = fileName.toLower();
    if (lowerName.endsWith(QLatin1String(".zip")))
        return importTakeoutZip(data);
    if (lowerName.endsWith(QLatin1String(".json")))
        return importNewpipeJson(data);

    // Loose CSV (or other text):
    // 1. Try subscriptions CSV
    const ImportResult csvSubs = importSubscriptionsCsv(data);
    if (!csvSubs.subscriptions.isEmpty())
        return csvSubs;

    // 2. Try playlist / watch later CSV
    const ImportResult csvPl = importPlaylistCsv(data, fileName);
    if (csvPl.hasNevertube)
        return csvPl;

    // 3. Fall back to JSON
    const ImportResult json = importNewpipeJson(data);
    if (!json.error.isEmpty())
        return csvSubs;
    return json;
}

// ---------------------------------------------------------------------------
// Channel Verification Parser
// ---------------------------------------------------------------------------

ChannelCheckResult parseChannelCheckResponse(const QByteArray &json,
                                             const QString &fallbackName)
{
    ChannelCheckResult res;
    res.title = fallbackName;

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (doc.isNull() || !doc.isObject())
        return res;

    const QJsonObject root = doc.object();

    // YouTube returns an "alerts" array when a channel has been terminated,
    // deleted, banned, or does not exist.
    if (root.contains(QStringLiteral("alerts"))) {
        const QJsonArray alerts = root.value(QStringLiteral("alerts")).toArray();
        if (!alerts.isEmpty())
            return res; // isAlive remains false
    }

    // A live channel has either c4TabbedHeaderRenderer or channelMetadataRenderer
    const QJsonObject header = root.value(QStringLiteral("header")).toObject();
    const QJsonObject tabbed = header.value(QStringLiteral("c4TabbedHeaderRenderer")).toObject();
    if (!tabbed.isEmpty()) {
        res.isAlive = true;
        const QString t = tabbed.value(QStringLiteral("title")).toString();
        if (!t.isEmpty())
            res.title = t;
        const QJsonArray thumbs = tabbed.value(QStringLiteral("avatar")).toObject()
                                      .value(QStringLiteral("thumbnails")).toArray();
        if (!thumbs.isEmpty())
            res.avatarUrl = thumbs.last().toObject().value(QStringLiteral("url")).toString();
        return res;
    }

    const QJsonObject metadata = root.value(QStringLiteral("metadata")).toObject();
    const QJsonObject chMeta = metadata.value(QStringLiteral("channelMetadataRenderer")).toObject();
    if (!chMeta.isEmpty()) {
        res.isAlive = true;
        const QString t = chMeta.value(QStringLiteral("title")).toString();
        if (!t.isEmpty())
            res.title = t;
        const QJsonArray thumbs = chMeta.value(QStringLiteral("avatar")).toObject()
                                      .value(QStringLiteral("thumbnails")).toArray();
        if (!thumbs.isEmpty())
            res.avatarUrl = thumbs.last().toObject().value(QStringLiteral("url")).toString();
        return res;
    }

    return res;
}

// ---------------------------------------------------------------------------
// Selftest
// ---------------------------------------------------------------------------

namespace
{
// Builds a small in-memory zip (STORED, no compression) with the given entries.
// Used by the selftest so it does not depend on an external zip tool.
QByteArray buildTestZip(const QList<QPair<QString, QByteArray>> &entries)
{
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    mz_zip_writer_init_heap(&zip, 0, 0);
    for (const auto &e : entries) {
        const QByteArray name = e.first.toUtf8();
        mz_zip_writer_add_mem(&zip, name.constData(), e.second.constData(),
                              static_cast<size_t>(e.second.size()), MZ_NO_COMPRESSION);
    }
    void *buf = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
    QByteArray res(static_cast<const char *>(buf), static_cast<int>(size));
    mz_zip_writer_end(&zip);
    return res;
}
}  // namespace

bool dataTransferSelfTest()
{
    int failures = 0;
    const auto expect = [&failures](bool cond, const char *what) {
        if (!cond) {
            ++failures;
            std::fprintf(stderr, "  data-transfer FAIL: %s\n", what);
        }
    };

    // 1. Export round-trip: build a payload, export it, re-import it.
    {
        QVector<Subscription> subs;
        Subscription s;
        s.serviceId = 0;
        s.url = QStringLiteral("https://www.youtube.com/channel/UCsXVk37bltHxD1rDPwtNM8Q");
        s.name = QStringLiteral("Kurzgesagt \u2013 In a Nutshell");
        subs.append(s);
        s.url = QStringLiteral("https://www.youtube.com/channel/UCEOXxzW2vU0P-0THehuIIeg");
        s.name = QStringLiteral("CaptainDisillusion");
        subs.append(s);

        NevertubeData nt;
        Playlist p;
        p.title = QStringLiteral("Mi lista");
        p.videoIds << QStringLiteral("dQw4w9WgXcQ") << QStringLiteral("abcdefghijk");
        nt.playlists.append(p);
        nt.watchLater << QStringLiteral("later1");
        HistoryEntry h;
        h.videoId = QStringLiteral("hist1");
        h.positionMs = 12345;
        nt.historyBreaks.append(h);
        nt.favorites << QStringLiteral("fav1");
        nt.starred << QStringLiteral("star1");

        const QByteArray json = exportJson(subs, nt, QStringLiteral("0.2.0"), 3);
        expect(!json.isEmpty(), "exportJson produces bytes");

        const QJsonDocument doc = QJsonDocument::fromJson(json);
        expect(doc.isObject(), "exportJson is a JSON object");
        expect(doc.object().contains(QStringLiteral("subscriptions")),
               "exportJson has subscriptions[]");
        expect(doc.object().contains(QStringLiteral("nevertube")),
               "exportJson has nevertube");

        const ImportResult back = importNewpipeJson(json);
        expect(back.error.isEmpty(), "re-import of export has no error");
        expect(back.subscriptions.size() == 2, "re-import reads 2 subscriptions");
        expect(back.subscriptions.at(0).url == subs.at(0).url,
               "re-import keeps the channel url");
        expect(back.subscriptions.at(0).name == subs.at(0).name,
               "re-import keeps the channel name");
        expect(back.hasNevertube, "re-import detects the nevertube block");
        expect(back.nevertube.playlists.size() == 1, "re-import reads the playlist");
        expect(back.nevertube.playlists.at(0).title == QStringLiteral("Mi lista"),
               "re-import keeps the playlist title");
        expect(back.nevertube.playlists.at(0).videoIds.size() == 2,
               "re-import keeps the playlist videos");
        expect(back.nevertube.watchLater.size() == 1, "re-import keeps watch later");
        expect(back.nevertube.historyBreaks.size() == 1, "re-import keeps history");
        expect(back.nevertube.historyBreaks.at(0).positionMs == 12345,
               "re-import keeps the history position");
        expect(back.nevertube.favorites.size() == 1, "re-import keeps favorites");
        expect(back.nevertube.starred.size() == 1, "re-import keeps starred");
    }

    // 2. Google Takeout subscriptions CSV (en and es headers).
    {
        const char *csvEn =
            "Channel Id,Channel Url,Channel Title\n"
            "UC1JTQBa5QxZCpXrFSkMxmPw,http://www.youtube.com/channel/UC1JTQBa5QxZCpXrFSkMxmPw,Raycevick\n"
            "UCFl7yKfcRcFmIUbKeCA-SJQ,http://www.youtube.com/channel/UCFl7yKfcRcFmIUbKeCA-SJQ,Joji\n";
        const ImportResult r = importSubscriptionsCsv(QByteArray(csvEn));
        expect(r.error.isEmpty(), "takeout csv en parses");
        expect(r.subscriptions.size() == 2, "takeout csv en reads 2");
        expect(r.subscriptions.at(0).url.startsWith(QLatin1String("https://")),
               "takeout csv normalizes http to https");
        expect(r.subscriptions.at(0).name == QStringLiteral("Raycevick"),
               "takeout csv en keeps the title");

        const char *csvEs =
            "Id del canal,URL del canal,Título del canal\n"
            "UC1JTQBa5QxZCpXrFSkMxmPw,https://www.youtube.com/channel/UC1JTQBa5QxZCpXrFSkMxmPw,Canal Uno\n";
        const ImportResult r2 = importSubscriptionsCsv(QByteArray(csvEs));
        expect(r2.subscriptions.size() == 1, "takeout csv es reads 1");
        expect(r2.subscriptions.at(0).name == QStringLiteral("Canal Uno"),
               "takeout csv es keeps the title");
    }

    // 3. A Google Takeout .zip, built on the fly (subscriptions.csv +
    //    playlists/<name>.csv + playlists/Watch later.csv).
    {
        const QByteArray subsCsv =
            "Channel Id,Channel Url,Channel Title\n"
            "UCsXVk37bltHxD1rDPwtNM8Q,https://www.youtube.com/channel/UCsXVk37bltHxD1rDPwtNM8Q,Kurzgesagt\n";
        const QByteArray plCsv =
            "Video ID,Time Added\n"
            "dQw4w9WgXcQ,2020-01-01 00:00:00 UTC\n"
            "abcdefghijk,2020-01-01 00:00:00 UTC\n";
        const QByteArray wlCsv =
            "Video ID,Time Added\n"
            "watchlater1,2020-01-01 00:00:00 UTC\n";
        const QByteArray metaCsv =
            "Playlist ID,Channel ID,Time Created,Time Updated,Title,Description,Visibility\n"
            "PLtnP62,UCUnD-6,1984-01-01 13:37:00 UTC,1984-01-01 13:37:00 UTC,3kl,,Unlisted\n";

        QList<QPair<QString, QByteArray>> files;
        files.append(qMakePair(
            QStringLiteral("Takeout/YouTube and YouTube Music/subscriptions/subscriptions.csv"),
            subsCsv));
        files.append(qMakePair(
            QStringLiteral("Takeout/YouTube and YouTube Music/playlists/playlists.csv"),
            metaCsv));
        files.append(qMakePair(
            QStringLiteral("Takeout/YouTube and YouTube Music/playlists/Mi lista.csv"),
            plCsv));
        files.append(qMakePair(
            QStringLiteral("Takeout/YouTube and YouTube Music/playlists/Watch later.csv"),
            wlCsv));
        const QByteArray zip = buildTestZip(files);
        expect(!zip.isEmpty(), "test zip builds");

        const ImportResult r = importTakeoutZip(zip);
        expect(r.error.isEmpty(), "takeout zip parses");
        expect(r.subscriptions.size() == 1, "takeout zip reads the subscription");
        expect(r.nevertube.playlists.size() == 1, "takeout zip reads the playlist");
        expect(r.nevertube.playlists.at(0).title == QStringLiteral("Mi lista"),
               "takeout zip playlist title = file name");
        expect(r.nevertube.playlists.at(0).videoIds.size() == 2,
               "takeout zip playlist ids");
        expect(r.nevertube.watchLater.size() == 1, "takeout zip reads watch later");
        expect(r.nevertube.favorites.isEmpty(), "takeout zip has no favorites here");
    }

    // 3b. Spanish Google Takeout .zip (suscripciones/suscripciones.csv +
    //     listas de reproducción/*-vídeos.csv + Watch later-vídeos.csv).
    {
        const QByteArray subsCsv =
            "ID del canal,URL del canal,Título del canal\n"
            "UC-3i0A0VCrSFZvEjuM7SYDg,http://www.youtube.com/channel/UC-3i0A0VCrSFZvEjuM7SYDg,Canal Español\n";
        const QByteArray mixCsv =
            "ID de vídeo,Marca de tiempo de creación de la lista de reproducción de vídeos\n"
            "47dtFZ8CFo8,2022-12-28T00:00:46+00:00\n"
            "CfihYWRWRTQ,2022-12-28T00:00:46+00:00\n";
        const QByteArray wlCsv =
            "ID de vídeo,Marca de tiempo de creación de la lista de reproducción de vídeos\n"
            "orDZOE8ARLs,2020-08-10T21:25:35+00:00\n";
        const QByteArray metaCsv =
            "ID de lista de reproducción,Añadir vídeos nuevos al principio\n"
            "PLKxL5pZo,Falso\n";

        QList<QPair<QString, QByteArray>> files;
        files.append(qMakePair(
            QStringLiteral("Takeout/YouTube y YouTube Music/suscripciones/suscripciones.csv"),
            subsCsv));
        files.append(qMakePair(
            QStringLiteral("Takeout/YouTube y YouTube Music/listas de reproducción/Listas de reproducción.csv"),
            metaCsv));
        files.append(qMakePair(
            QStringLiteral("Takeout/YouTube y YouTube Music/listas de reproducción/Mix Música en inglés-vídeos.csv"),
            mixCsv));
        files.append(qMakePair(
            QStringLiteral("Takeout/YouTube y YouTube Music/listas de reproducción/Watch later-vídeos.csv"),
            wlCsv));

        const QByteArray zip = buildTestZip(files);
        expect(!zip.isEmpty(), "spanish test zip builds");

        const ImportResult r = importTakeoutZip(zip);
        expect(r.error.isEmpty(), "spanish takeout zip parses");
        expect(r.subscriptions.size() == 1, "spanish takeout zip reads subscriptions");
        expect(r.subscriptions.at(0).name == QStringLiteral("Canal Español"),
               "spanish takeout zip channel name");
        expect(r.nevertube.playlists.size() == 1, "spanish takeout zip reads playlist");
        expect(r.nevertube.playlists.at(0).title == QStringLiteral("Mix Música en inglés"),
               "spanish takeout zip strips -vídeos suffix");
        expect(r.nevertube.playlists.at(0).videoIds.size() == 2,
               "spanish takeout zip playlist videos");
        expect(r.nevertube.watchLater.size() == 1, "spanish takeout zip reads watch later");
    }

    // 3c. Loose playlist CSV (unzipped file, e.g. "Mix Música en inglés-vídeos.csv")
    {
        const QByteArray mixCsv =
            "ID de vídeo,Marca de tiempo de creación de la lista de reproducción de vídeos\n"
            "47dtFZ8CFo8,2022-12-28T00:00:46+00:00\n"
            "CfihYWRWRTQ,2022-12-28T00:00:46+00:00\n";
        const ImportResult r = importFile(mixCsv, QStringLiteral("Mix Música en inglés-vídeos.csv"));
        expect(r.error.isEmpty(), "loose playlist csv parses");
        expect(r.hasNevertube, "loose playlist csv marked as nevertube data");
        expect(r.nevertube.playlists.size() == 1, "loose playlist csv reads playlist");
        expect(r.nevertube.playlists.at(0).title == QStringLiteral("Mix Música en inglés"),
               "loose playlist csv strips suffix");
        expect(r.nevertube.playlists.at(0).videoIds.size() == 2,
               "loose playlist csv has 2 videos");
    }

    // 4. A file that is neither a zip nor a valid JSON/CSV must not crash.
    {
        const ImportResult r = importFile(QByteArrayLiteral("hello world\n"),
                                          QStringLiteral("file.txt"));
        (void)r;
    }

    // 5. Database round-trip (offline): fills real repositories, exports them,
    //    imports the payload into a fresh database, and asserts that the repos
    //    carry the exact data.
    {
        QTemporaryDir tmp1;
        QTemporaryDir tmp2;
        expect(tmp1.isValid() && tmp2.isValid(), "temp dirs valid");

        const QString dbPath1 = tmp1.path() + QStringLiteral("/db1.sqlite");
        const QString dbPath2 = tmp2.path() + QStringLiteral("/db2.sqlite");

        // DB 1: seed data
        AppDatabase db1(dbPath1);
        expect(db1.open(), "db1 opens");
        SubscriptionsRepo subs1(db1);
        PlaylistsRepo pls1(db1);
        WatchLaterRepo wl1(db1);
        HistoryRepo hist1(db1);
        FavoritesRepo fav1(db1);
        StarredRepo star1(db1);

        subs1.add(QStringLiteral("UCsXVk37bltHxD1rDPwtNM8Q"),
                  QStringLiteral("Kurzgesagt"));
        subs1.add(QStringLiteral("UCEOXxzW2vU0P-0THehuIIeg"),
                  QStringLiteral("CaptainDisillusion"));

        const QString plId = pls1.create(QStringLiteral("Favoritas"));
        expect(!plId.isEmpty(), "playlist created");
        pls1.addVideo(plId, QStringLiteral("videoA"));
        pls1.addVideo(plId, QStringLiteral("videoB"));

        wl1.toggle(QStringLiteral("later1"));
        fav1.toggle(QStringLiteral("fav1"));
        star1.toggle(QStringLiteral("star1"));
        hist1.add(QStringLiteral("hist1"), 45000);

        // Export from DB 1
        QVector<Subscription> exportSubs;
        for (const SubscriptionInfo &s : subs1.all()) {
            Subscription sub;
            sub.serviceId = 0;
            sub.url = QStringLiteral("https://www.youtube.com/channel/") + s.id;
            sub.name = s.title;
            exportSubs.append(sub);
        }
        NevertubeData ntData;
        for (const PlaylistItem &p : pls1.all()) {
            Playlist pl;
            pl.title = p.title;
            pl.videoIds = pls1.videoIds(p.id);
            ntData.playlists.append(pl);
        }
        ntData.watchLater = wl1.ids();
        for (const QString &vid : hist1.recentIds(10)) {
            HistoryEntry h;
            h.videoId = vid;
            h.positionMs = hist1.positionOf(vid);
            ntData.historyBreaks.append(h);
        }
        ntData.favorites = fav1.ids();
        ntData.starred = star1.ids();

        const QByteArray exportedJson =
            exportJson(exportSubs, ntData, QStringLiteral("0.2.0"), 1);
        expect(!exportedJson.isEmpty(), "db export produces bytes");

        // DB 2: fresh, import the payload
        AppDatabase db2(dbPath2);
        expect(db2.open(), "db2 opens");
        SubscriptionsRepo subs2(db2);
        PlaylistsRepo pls2(db2);
        WatchLaterRepo wl2(db2);
        HistoryRepo hist2(db2);
        FavoritesRepo fav2(db2);
        StarredRepo star2(db2);

        const ImportResult res = importNewpipeJson(exportedJson);
        expect(res.error.isEmpty(), "db import has no error");
        expect(res.subscriptions.size() == 2, "db import reads 2 subs");

        for (const Subscription &s : res.subscriptions) {
            const int slash = s.url.lastIndexOf(QLatin1Char('/'));
            const QString channelId = slash >= 0 ? s.url.mid(slash + 1) : s.url;
            subs2.add(channelId, s.name);
        }
        if (res.hasNevertube) {
            for (const Playlist &pl : res.nevertube.playlists) {
                const QString id = pls2.create(pl.title);
                for (const QString &vid : pl.videoIds)
                    pls2.addVideo(id, vid);
            }
            for (const QString &vid : res.nevertube.watchLater)
                wl2.toggle(vid);
            for (const QString &vid : res.nevertube.favorites)
                fav2.toggle(vid);
            for (const QString &vid : res.nevertube.starred)
                star2.toggle(vid);
            for (const HistoryEntry &h : res.nevertube.historyBreaks)
                hist2.add(h.videoId, h.positionMs);
        }

        // Verify DB 2 state
        expect(subs2.all().size() == 2, "db2 has 2 subscriptions");
        expect(subs2.isSubscribed(QStringLiteral("UCsXVk37bltHxD1rDPwtNM8Q")),
               "db2 has Kurzgesagt");
        expect(pls2.all().size() == 1, "db2 has 1 playlist");
        expect(pls2.all().at(0).title == QStringLiteral("Favoritas"),
               "db2 playlist title is Favoritas");
        const QStringList plVideos = pls2.videoIds(pls2.all().at(0).id);
        expect(plVideos.size() == 2, "db2 playlist has 2 videos");
        expect(plVideos.at(0) == QStringLiteral("videoA")
                   && plVideos.at(1) == QStringLiteral("videoB"),
               "db2 playlist keeps video order");
        expect(wl2.isWatchLater(QStringLiteral("later1")), "db2 has watch later");
        expect(fav2.isFavorite(QStringLiteral("fav1")), "db2 has favorite");
        expect(star2.isStarred(QStringLiteral("star1")), "db2 has starred");
        expect(hist2.positionOf(QStringLiteral("hist1")) == 45000,
               "db2 has history with position");
    }

    // 6. Channel verification response parser: recognizes alive vs terminated channels
    {
        const QByteArray aliveJson = QByteArrayLiteral(
            "{\"header\":{\"c4TabbedHeaderRenderer\":{\"title\":\"Canal Vivo\","
            "\"avatar\":{\"thumbnails\":[{\"url\":\"https://example.com/avatar.jpg\"}]}}}}");
        const ChannelCheckResult r1 = parseChannelCheckResponse(aliveJson, QStringLiteral("Fallback"));
        expect(r1.isAlive, "alive channel recognized");
        expect(r1.title == QStringLiteral("Canal Vivo"), "alive channel gets real title");
        expect(r1.avatarUrl == QStringLiteral("https://example.com/avatar.jpg"), "alive channel gets avatar");

        const QByteArray deadJson = QByteArrayLiteral(
            "{\"alerts\":[{\"alertRenderer\":{\"text\":{\"simpleText\":\"Este canal se ha retirado.\"}}}]}");
        const ChannelCheckResult r2 = parseChannelCheckResponse(deadJson, QStringLiteral("Fallback"));
        expect(!r2.isAlive, "terminated channel recognized as not alive");

        const ChannelCheckResult r3 = parseChannelCheckResponse(QByteArrayLiteral("{bad json"), QStringLiteral("Fallback"));
        expect(!r3.isAlive, "bad json treated as not alive");
    }

    if (failures == 0)
        std::fprintf(stderr, "SELFTEST data-transfer: OK\n");
    else
        std::fprintf(stderr, "SELFTEST data-transfer: %d failure(s)\n", failures);
    return failures == 0;
}

}  // namespace data_transfer