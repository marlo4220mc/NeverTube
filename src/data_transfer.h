#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

// Data transfer (export / import) compatible with YouTube (Google Takeout) and
// NewPipe.
//
// The EXPORT produces a SINGLE JSON file with two parts:
//   - `subscriptions` in the EXACT format of NewPipe's import/export
//     (ImportExportJsonHelper): { app_version, app_version_int,
//     subscriptions: [ { service_id, url, name } ] }. NewPipe imports ONLY
//     this array (service_id 0 = YouTube) and IGNORES everything else, so
//     NeverTube's export is a valid NewPipe backup.
//   - a `nevertube` object with the data NewPipe does not cover (playlists,
//     watch later, history, favorites, starred), which NewPipe's parser never
//     reads and which gives NeverTube a full backup for reinstall.
//
// The IMPORT accepts THREE sources:
//   1. A NewPipe / NeverTube JSON (the object above).
//   2. A Google Takeout subscriptions.csv (loose): the header varies by
//      locale but the columns are ALWAYS in the order Channel Id, Channel Url,
//      Channel Title (the same trick YoutubeSubscriptionExtractor uses).
//   3. A Google Takeout .zip: it looks for subscriptions.csv inside, and also
//      reads the playlists folder (one CSV per playlist, whose FILE NAME is
//      the playlist title, with rows "Video ID,Time Added") and
//      "Watch later.csv".
//
// All the parsing/formatting here is PURE (no repo, no DB, no network): the
// caller (MainWindow) feeds in-memory structures and writes the repos. That
// keeps it testable offline with --selftest-data-transfer.

namespace data_transfer
{

// A single subscription row, in the shape NewPipe writes/reads.
struct Subscription
{
    int serviceId = 0;  // 0 = YouTube
    QString url;        // https://www.youtube.com/channel/<id>
    QString name;
    QString avatarUrl;
};

// A playlist: a title and the ordered ids of its videos. (Takeout's per-playlist
// CSV only carries the video ids and the added date; the metadata is resolved
// lazily by the app, see the import section of AGENTS.md.)
struct Playlist
{
    QString title;
    QStringList videoIds;  // in position order
};

// A history entry: the video id and where playback stopped.
struct HistoryEntry
{
    QString videoId;
    qlonglong positionMs = 0;
};

// Everything a NeverTube export/import carries besides subscriptions. NewPipe
// never sees this: it lives under the "nevertube" key.
struct NevertubeData
{
    QVector<Playlist> playlists;
    QStringList watchLater;  // ordered video ids
    QVector<HistoryEntry> historyBreaks;
    QStringList favorites;   // Me gusta
    QStringList starred;
};

// Parsed result of an import. The `source` tells the caller what to apply.
struct ImportResult
{
    QVector<Subscription> subscriptions;
    NevertubeData nevertube;         // empty when the source had no nevertube key
    bool hasNevertube = false;       // the file carried a nevertube block
    QString error;                   // non-empty on a hard failure
    QString warning;                 // non-empty when only PART was read (e.g.
                                     // a zip with no subscriptions.csv)
};

// ---- EXPORT ----
// Serializes a NeverTube export to a byte array (UTF-8 JSON) that is at the
// same time a valid NewPipe subscriptions backup.
QByteArray exportJson(const QVector<Subscription> &subscriptions,
                      const NevertubeData &nevertube,
                      const QString &appVersion, int appVersionInt);

// ---- IMPORT ----
// The caller hands the WHOLE file bytes and a hint of its type. Detects the
// format and returns the parsed result. `fileName` is used to pick CSV vs
// JSON when the content is ambiguous.
ImportResult importFile(const QByteArray &data, const QString &fileName);

// Parses a Google Takeout subscriptions CSV (the loose file).
ImportResult importSubscriptionsCsv(const QByteArray &csv);
// Parses a Google Takeout playlist or watch later CSV (e.g. "My Playlist-videos.csv").
ImportResult importPlaylistCsv(const QByteArray &csv, const QString &fileName);
// Parses a Google Takeout .zip (subscriptions.csv + playlists/ + Watch later.csv).
ImportResult importTakeoutZip(const QByteArray &zipData);
// Parses a NewPipe / NeverTube JSON.
ImportResult importNewpipeJson(const QByteArray &json);

struct ChannelCheckResult
{
    bool isAlive = false;
    QString title;
    QString avatarUrl;
};

// Parses the browse JSON response to check whether a channel is alive
// (active) or terminated/deleted/banned by YouTube.
ChannelCheckResult parseChannelCheckResponse(const QByteArray &json,
                                             const QString &fallbackName);

// Selftest (offline): the pure conversions round-trip and the parsers accept
// the real shapes (NewPipe JSON, Takeout CSV with en/es headers, a small zip
// built on the fly). Returns true when everything passes.
bool dataTransferSelfTest();

}  // namespace data_transfer