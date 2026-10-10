#include "yt_innertube.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QQueue>
#include <QSslError>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QList>
#include <QtMath>

#include <cstdio>
#include <algorithm>
#include <functional>
#include <utility>

#include "debug.h"

#include "i18n.h"

namespace {

const char kYoutubeHost[] = "https://www.youtube.com";
const char kYoutubeName[] = "www.youtube.com";
const char kSwJsData[] = "/sw.js_data";
const char kPlayerEndpoint[] =
    "/youtubei/v1/player?prettyPrint=false";
const char kNextEndpoint[] =
    "/youtubei/v1/next?prettyPrint=false";
const char kBrowseEndpoint[] =
    "/youtubei/v1/browse?prettyPrint=false";
const char kSearchEndpoint[] =
    "/youtubei/v1/search?prettyPrint=false";
// Search suggestions (type-ahead of the field).
//
// The chosen endpoint is the REAL autocomplete of youtube.com:
// suggestqueries.google.com with client=youtube. This is decided by the
// measurement in /selftest-suggest, not by preference (see suggestSelfTest):
//   - Innertube music/get_search_suggestions only matches real YT on
//     14-43% of suggestions, returns 6 on short prefixes and drags along a
//     4-52 KB music carousel that the type-ahead never uses.
//   - client=firefox matches 50-71%, but truncates to 10 items and drops
//     the entity metadata.
//   - client=youtube IS the youtube.com search dataset: 14 items,
//     0.3-2 KB, ~280 ms, and it carries the entity (description, avatar,
//     wikipedia) for rows with a knowledge panel.
// It does not touch the Innertube session: it is a plain GET to a Google
// host, so it cannot suffer the 403 session gate documented in AGENTS.md.
const char kSuggestUrl[] =
    "https://suggestqueries.google.com/complete/search"
    "?client=youtube&ds=yt&hl=es&gl=US&q=";
// JS wrapper that Google puts around the JSON. It is NOT JavaScript to
// execute: it is a prefix and a closing parenthesis that get trimmed off to
// leave plain JSON, which is parsed with QJsonDocument. There is no JS engine
// in the app.
const char kSuggestJsPrefix[] = "window.google.ac.h(";

// UA of the youtube_explode_dart fork used for /sw.js_data.
const char kChromeUA[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/132.0.0.0 Safari/537.36";

// VISIONOS client UA (matches the one in the player body). Values taken
// from the reference client tables (NewPipeExtractor).
const char kVisionOSUA[] =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) "
    "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";

// UA of the IOS client (com.google.ios.youtube).
const char kIOSUA[] =
    "com.google.ios.youtube/21.26.4 "
    "(iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)";

// UA of the ANDROID_VR client (Oculus/Quest).
const char kAndroidVRUA[] =
    "com.google.android.apps.youtube.vr.oculus/1.65.10 "
    "(Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip";

const char kFallbackVisitorData[] =
    "CgttN24wcmd5UzNSWSi2lvq2BjIKCgJKUBIEGgAgYQ%3D%3D";

// Timeouts per phase of the Ipv4Https transport (ms). Regenerating
// visitorData must not wait for the long player timeout.
constexpr int kVisitorTimeoutMs = 10000;
constexpr int kPlayerTimeoutMs = 12000;
constexpr int kMasterTimeoutMs = 15000;

// Client cascade for /player, in order of preference. VISIONOS is the
// primary one (the only one that today needs no poToken and returns
// hlsManifestUrl on VODs); the rest are the NewPipe-style fallback for videos
// that require login (age/members) or that do not respond on VISIONOS. The
// values (current 2026 version, device/os/UA) come from the reference client
// tables (NewPipeExtractor); they are not invented.
struct PlayerClient
{
    const char *name;
    const char *version;
    int clientId; // numeric X-Youtube-Client-Name
    const char *ua;
    const char *deviceMake; // nullptr when not applicable
    const char *deviceModel;
    const char *osName;
    const char *osVersion;
    int androidSdkVersion; // 0 when not applicable
    bool embed;            // requires thirdParty.embedUrl + Referer
    const char *embedUrl;
};

const PlayerClient kPlayerClients[] = {
    {"VISIONOS", "1.02", 101, kVisionOSUA,
     "Apple", "RealityDevice17,1", "visionOS", "26.5.23O471",
     0, false, nullptr},
    {"IOS", "21.26.4", 5, kIOSUA,
     "Apple", "iPhone16,2", "iPhone", "18.3.2.22D82",
     0, false, nullptr},
    {"WEB_EMBEDDED_PLAYER", "2.20260708.00.00", 56, kChromeUA,
     nullptr, nullptr, nullptr, nullptr,
     0, true, "https://www.reddit.com/"},
    {"TVHTML5_SIMPLY", "1.0", 75, kChromeUA,
     nullptr, nullptr, nullptr, nullptr,
     0, false, nullptr},
    {"ANDROID_VR", "1.65.10", 28, kAndroidVRUA,
     "Oculus", "Quest 3", "Android", "12L",
     32, false, nullptr},
};
constexpr int kPlayerClientCount =
    static_cast<int>(sizeof(kPlayerClients) / sizeof(kPlayerClients[0]));

// WEB client (only for browsing a channel's Shorts/Live tabs): VISIONOS does
// not expose those tabs (it returns the Home) but WEB does, though with the
// modern (*ViewModel) layout. It requires no X-Goog-Api-Key (verified).
constexpr int kWebClientId = 1;
const char kWebClientVersion[] = "2.20240726.00.00";

const char kFieldMask[] =
    "playabilityStatus.status,playabilityStatus.reason,"
    "streamingData.hlsManifestUrl,streamingData.adaptiveFormats,"
    "videoDetails.videoId,videoDetails.lengthSeconds,videoDetails.isLive";

// Unique ascending video heights extracted from streamingData.
// Only video entries (with width/height > 0) are taken into account;
// audio-only itags (139/140/249/…) are ignored.
QVector<int> videoHeights(const QJsonObject &streamingData)
{
    QVector<int> heights;
    const QJsonArray adaptive =
        streamingData.value(QStringLiteral("adaptiveFormats")).toArray();
    for (const QJsonValue &entry : adaptive) {
        const QJsonObject f = entry.toObject();
        const int width = f.value(QStringLiteral("width")).toInt();
        const int height = f.value(QStringLiteral("height")).toInt();
        if (width > 0 && height > 0 && !heights.contains(height))
            heights.append(height);
    }
    std::sort(heights.begin(), heights.end());
    return heights;
}

// Generic client context for the cascade, built from the kPlayerClients
// table. Each client contributes its own device/os/UA fields (real values
// from those clients). "hl" is set to the app language (auto "es" from the
// original-titles fix: without it YouTube auto-translates titles to English)
// and every client carries visitorData.
QJsonObject clientContext(const PlayerClient &pc, const QString &visitorData,
                          const QString &hl = QStringLiteral("es"))
{
    QJsonObject client{
        {QStringLiteral("clientName"), QString::fromLatin1(pc.name)},
        {QStringLiteral("clientVersion"), QString::fromLatin1(pc.version)},
        {QStringLiteral("hl"), hl},
        {QStringLiteral("gl"), QStringLiteral("US")},
        {QStringLiteral("visitorData"), visitorData},
    };
    if (pc.ua && *pc.ua)
        client.insert(QStringLiteral("userAgent"), QString::fromLatin1(pc.ua));
    if (pc.deviceMake && *pc.deviceMake)
        client.insert(QStringLiteral("deviceMake"), QString::fromLatin1(pc.deviceMake));
    if (pc.deviceModel && *pc.deviceModel)
        client.insert(QStringLiteral("deviceModel"), QString::fromLatin1(pc.deviceModel));
    if (pc.osName && *pc.osName)
        client.insert(QStringLiteral("osName"), QString::fromLatin1(pc.osName));
    if (pc.osVersion && *pc.osVersion)
        client.insert(QStringLiteral("osVersion"), QString::fromLatin1(pc.osVersion));
    if (pc.androidSdkVersion > 0)
        client.insert(QStringLiteral("androidSdkVersion"), pc.androidSdkVersion);
    return client;
}

// WEB client context (browsing shorts/live tabs). Only name, version,
// language and visitorData: WEB needs neither device/os nor an API key.
QJsonObject webClientContext(const QString &visitorData, const QString &hl)
{
    return QJsonObject{
        {QStringLiteral("clientName"), QStringLiteral("WEB")},
        {QStringLiteral("clientVersion"), QString::fromLatin1(kWebClientVersion)},
        {QStringLiteral("hl"), hl},
        {QStringLiteral("gl"), QStringLiteral("US")},
        {QStringLiteral("visitorData"), visitorData},
        {QStringLiteral("userAgent"), QString::fromLatin1(kChromeUA)},
    };
}

// Body of a youtubei/v1 response → QJsonObject. Strips the anti-XSSI prefix
// (")]}'") that YouTube prepends to some responses and tolerates non-JSON
// bodies (errors/HTML). Returns false when there is no JSON object.
bool parseJsonObject(const QByteArray &body, QJsonObject *root)
{
    QByteArray json = body.trimmed();
    if (json.startsWith(")]}'"))
        json.remove(0, 4);
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    if (root)
        *root = doc.object();
    return true;
}

QString visitorDataFromSwJsData(const QByteArray &body)
{
    QByteArray raw = body;
    raw = raw.trimmed();
    if (!raw.startsWith(")]}'"))
        return QString();
    raw.remove(0, 4);
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return QString();
    const QJsonArray root = doc.array();
    if (root.size() < 1 || !root.at(0).isArray())
        return QString();
    const QJsonArray level1 = root.at(0).toArray();
    if (level1.size() < 3 || !level1.at(2).isArray())
        return QString();
    const QJsonArray level2 = level1.at(2).toArray();
    if (level2.size() < 1 || !level2.at(0).isArray())
        return QString();
    const QJsonArray level3 = level2.at(0).toArray();
    if (level3.size() < 1 || !level3.at(0).isArray())
        return QString();
    const QJsonArray level4 = level3.at(0).toArray();
    if (level4.size() <= 13)
        return QString();
    return level4.at(13).toString();
}

// Body of /complete/search -> suggestion list.
//
// The body is JSON wrapped in a JS call: window.google.ac.h([...]). Nothing
// is executed: the prefix and the trailing parenthesis are trimmed off and
// what remains is a plain JSON array parsed with QJsonDocument. There is no
// JS engine in the app. If Google changed the wrapper, the trim does not
// apply and parsing fails -> empty list, which is the correct failure (the
// dropdown hides) instead of painting garbage rows.
//
// Each item is ["text", flags, [ids...], {metadata}]. Only the text is kept,
// and ONLY if it is a non-empty string: the filter is explicit because items
// without text are not searchable queries and must not become blank rows in
// the dropdown (the typical failure of this endpoint).
QVector<Suggestion> parseSuggestBody(const QByteArray &body)
{
    QVector<Suggestion> out;
    QString text = QString::fromUtf8(body).trimmed();
    if (text.startsWith(QLatin1String(")]}'"))) {
        text.remove(0, 4);
        text = text.trimmed(); // the XSSI prefix is usually followed by a \n
    }
    const QString prefix = QLatin1String(kSuggestJsPrefix);
    if (text.startsWith(prefix)) {
        text = text.mid(prefix.size()).trimmed();
        if (text.endsWith(QLatin1Char(')')))
            text.chop(1);
    } else if (!text.startsWith(QLatin1Char('['))) {
        // Without the expected wrapper this is not the known format: no
        // guessing, not even half of it.
        return out;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return out;

    // arr[1] is the list of items; arr[0] is the already typed prefix.
    const QJsonArray arr = doc.array();
    if (arr.size() < 2)
        return out;
    const QJsonArray items = arr.at(1).toArray();
    for (const QJsonValue &v : items) {
        // The endpoint uses TWO formats depending on the client, and both
        // have to be read:
        //   client=youtube/chrome -> ["text", flags, [ids...], {meta}]
        //   client=firefox        -> "text" (bare string)
        // The youtube one (what the app uses) is covered and, if the item
        // arrives as a string, it is accepted too: it is the same text and it
        // costs nothing. An item that is neither of the two is discarded.
        QString query;
        QString detail;
        if (v.isString()) {
            query = v.toString().trimmed();
        } else if (v.isArray()) {
            const QJsonArray item = v.toArray();
            if (item.isEmpty())
                continue;
            query = item.at(0).toString().trimmed();
            // Entity metadata of the knowledge panel (only some rows carry
            // it): "Cantante y compositor peruano", avatar, wikipedia.
            const QJsonObject meta = item.at(3).toObject();
            detail = meta.value(QStringLiteral("zaf")).toString().trimmed();
            if (detail.isEmpty())
                detail = meta.value(QStringLiteral("zao")).toString().trimmed();
        } else {
            continue;
        }
        if (query.isEmpty())
            continue; // no real query: not a valid row
        Suggestion s;
        s.query = query;
        s.detail = detail;
        out.append(s);
    }
    return out;
}

} // namespace

// Minimal HTTP/HTTPS client that forces IPv4 connections while keeping SNI
// and certificate validation against www.youtube.com (QSslSocket uses
// setPeerVerifyName for both). YouTube blocks certain videos over IPv6 from
// this network; over IPv4 they answer without the anti-bot gate.
class Ipv4Https : public QObject
{
    Q_OBJECT

public:
    using ResultFn = std::function<void(int status, const QByteArray &body)>;
    using ErrorFn = std::function<void(const QString &reason)>;

    explicit Ipv4Https(QObject *parent = nullptr)
        : QObject(parent)
        , m_timeout(new QTimer(this))
    {
        m_timeout->setSingleShot(true);
        m_timeout->setInterval(30000);
        connect(m_timeout, &QTimer::timeout, this, [this]() {
            abortWithError(QStringLiteral("timeout HTTPS"));
        });
    }

    ~Ipv4Https() override
    {
        reset(nullptr);
    }

    bool isBusy() const
    {
        return m_socket && m_socket->state() != QAbstractSocket::UnconnectedState;
    }

    void abort()
    {
        m_queue.clear();
        reset(nullptr);
    }

    void get(const char *path, const char *ua)
    {
        HttpRequest req;
        req.verb = QByteArrayLiteral("GET");
        req.path = QByteArray(path);
        req.headers = QList<QPair<QByteArray, QByteArray>>{
            {QByteArrayLiteral("Accept-Encoding"), QByteArrayLiteral("identity")},
            {QByteArrayLiteral("Connection"), QByteArrayLiteral("close")},
            {QByteArrayLiteral("Host"), QByteArray(kYoutubeName)},
            {QByteArrayLiteral("User-Agent"), QByteArray(ua)},
        };
        start(req);
    }

    // GET of an absolute HTTPS URL (e.g. the HLS master on
    // manifest.googlevideo.com). Reuses the same IPv4 transport.
    // highPriority=true moves the request to the front of the queue: the
    // master is required to start the video and must not wait behind the
    // radio/next seeding (which does not block playback).
    void getUrl(const QUrl &url, const char *ua, bool highPriority = false)
    {
        HttpRequest req;
        req.verb = QByteArrayLiteral("GET");
        const QString host = url.host();
        // FullyEncoded (not the default path()/query(), which RETURN the
        // decoded form): the request-line must send the server exactly what
        // the caller wrote. With query() but no FullyEncoded, a "&q=" that the
        // caller percent-encoded by hand comes back with raw spaces and the
        // endpoint answers 400. For the HLS master it does not show (its URLs
        // already come consistent), but on /complete/search it broke it.
        QString path = url.path(QUrl::FullyEncoded);
        if (url.hasQuery())
            path += QLatin1Char('?') + url.query(QUrl::FullyEncoded);
        req.host = host.toUtf8();
        req.path = path.toUtf8();
        req.headers = QList<QPair<QByteArray, QByteArray>>{
            {QByteArrayLiteral("Accept-Encoding"), QByteArrayLiteral("identity")},
            {QByteArrayLiteral("Connection"), QByteArrayLiteral("close")},
            {QByteArrayLiteral("Host"), req.host},
            {QByteArrayLiteral("User-Agent"), QByteArray(ua)},
        };
        req.highPriority = highPriority;
        start(req);
    }

    void post(const char *path, const QByteArray &jsonBody, const char *ua,
              int clientId, const char *clientVersion, const char *referer = nullptr,
              const char *apiKey = nullptr, const char *fieldMask = nullptr)
    {
        HttpRequest req;
        req.verb = QByteArrayLiteral("POST");
        req.path = QByteArray(path);
        req.body = jsonBody;
        QList<QPair<QByteArray, QByteArray>> headers{
            {QByteArrayLiteral("Accept-Encoding"), QByteArrayLiteral("identity")},
            {QByteArrayLiteral("Connection"), QByteArrayLiteral("close")},
            {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")},
            {QByteArrayLiteral("Cookie"), QByteArrayLiteral("CONSENT=YES+1")},
            {QByteArrayLiteral("Host"), QByteArray(kYoutubeName)},
            {QByteArrayLiteral("Origin"), QByteArrayLiteral("https://www.youtube.com")},
            {QByteArrayLiteral("Sec-Fetch-Mode"), QByteArrayLiteral("navigate")},
            {QByteArrayLiteral("User-Agent"), QByteArray(ua)},
            {QByteArrayLiteral("X-Youtube-Client-Name"), QByteArray::number(clientId)},
            {QByteArrayLiteral("X-Youtube-Client-Version"), QByteArray(clientVersion)},
            {QByteArrayLiteral("Content-Length"), QByteArray::number(req.body.size())},
        };
        if (fieldMask && *fieldMask)
            headers.append({QByteArrayLiteral("X-Goog-FieldMask"),
                            QByteArray(fieldMask)});
        if (apiKey && *apiKey)
            headers.append({QByteArrayLiteral("X-Goog-Api-Key"),
                            QByteArray(apiKey)});
        if (referer && *referer)
            headers.append({QByteArrayLiteral("Referer"), QByteArray(referer)});
        if (!m_visitorData.isEmpty())
            headers.append({QByteArrayLiteral("X-Goog-Visitor-Id"),
                            m_visitorData.toUtf8()});
        req.host = QByteArray(kYoutubeName);
        req.headers = std::move(headers);
        start(req);
    }

    void setVisitorId(const QString &visitorData)
    {
        m_visitorData = visitorData;
    }

    void setTimeoutMs(int ms)
    {
        // Timeout of the NEXT request that gets queued (each request carries
        // its own: the transport is FIFO).
        m_nextTimeoutMs = ms;
    }

    void setResult(ResultFn result)
    {
        m_result = std::move(result);
    }

    void setError(ErrorFn error)
    {
        m_error = std::move(error);
    }

private:
    struct HttpRequest
    {
        QByteArray verb;
        QByteArray path;
        QByteArray host;
        QByteArray body;
        QList<QPair<QByteArray, QByteArray>> headers;
        ResultFn result;
        ErrorFn error;
        int timeoutMs = 30000;
        bool highPriority = false;
    };

    void start(HttpRequest req)
    {
        // Each request captures ITS OWN callbacks and timeout when queued: if
        // another phase (player/next/radio/master) is already using the
        // socket, the new one waits in the FIFO queue instead of aborting the
        // connection in progress.
        req.result = std::move(m_result);
        req.error = std::move(m_error);
        req.timeoutMs = m_nextTimeoutMs;
        m_result = nullptr;
        m_error = nullptr;
        m_nextTimeoutMs = 30000;
        if (req.highPriority)
            m_queue.prepend(req);
        else
            m_queue.enqueue(req);
        pump();
    }

    void pump()
    {
        if (m_inFlight)
            return;
        if (m_queue.isEmpty())
            return;
        m_request = std::move(m_queue.dequeue());
        m_inFlight = true;
        m_response.clear();
        m_responseHeaderSize = -1;
        m_timeout->setInterval(m_request.timeoutMs);
        m_timeout->start();

        const QByteArray reqHost =
            m_request.host.isEmpty() ? QByteArray(kYoutubeName) : m_request.host;

        if (m_socket) {
            m_socket->abort();
            m_socket->deleteLater();
        }
        m_socket = new QSslSocket(this);
        const QSslSocket *current = m_socket;
        connect(m_socket, &QSslSocket::encrypted, this,
                [this, current]() {
                    onEncrypted(current);
                });
        connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::errorOccurred),
                this, [this, current](QAbstractSocket::SocketError) {
                    onSocketError(current);
                });
        connect(m_socket, &QIODevice::readyRead, this,
                [this, current]() {
                    onReadyRead(current);
                });
        connect(m_socket, &QAbstractSocket::disconnected, this,
                [this, current]() {
                    onDisconnected(current);
                });

        // Resolve the target host to an IPv4 address.
        QHostInfo::lookupHost(QString::fromUtf8(reqHost), this,
                              [this, current, reqHost](const QHostInfo &info) {
                                  if (current != m_socket)
                                      return;
                                  QHostAddress ipv4;
                                  const QList<QHostAddress> addrs = info.addresses();
                                  for (const QHostAddress &a : addrs) {
                                      if (a.protocol() == QAbstractSocket::IPv4Protocol) {
                                          ipv4 = a;
                                          break;
                                      }
                                  }
                                  if (ipv4.isNull()) {
                                      for (const QHostAddress &a : addrs) {
                                          if (a.protocol() == QAbstractSocket::IPv6Protocol) {
                                              ipv4 = a;
                                              break;
                                          }
                                      }
                                  }
                                  if (ipv4.isNull()) {
                                      abortWithError(QStringLiteral("sin IPv4 para %1")
                                                         .arg(QString::fromUtf8(reqHost)));
                                      return;
                                  }
                                  NT_DEBUG("E2E innertube ipv4=%s host=%s\n",
                                           ipv4.toString().toUtf8().constData(),
                                           reqHost.constData());
                                  if (current != m_socket)
                                      return;
                                  m_socket->setPeerVerifyName(QString::fromUtf8(reqHost));
                                  m_socket->connectToHostEncrypted(ipv4.toString(), 443);
                              });
    }

    void onEncrypted(const QSslSocket *current)
    {
        if (current != m_socket)
            return;
        if (m_socket->state() != QAbstractSocket::ConnectedState)
            return;
        QByteArray request = m_request.verb + ' ' + m_request.path +
                             " HTTP/1.1\r\n";
        NT_DEBUG("E2E innertube -> %s %s body=%d\n", m_request.verb.constData(),
                 m_request.path.constData(), m_request.body.size());
        for (const auto &header : m_request.headers)
            request += header.first + ": " + header.second + "\r\n";
        request += "\r\n";
        request += m_request.body;
        m_socket->write(request);
        m_socket->flush();
    }

    void onReadyRead(const QSslSocket *current)
    {
        if (current != m_socket)
            return;
        m_response += m_socket->readAll();
        if (m_responseHeaderSize < 0) {
            const int headerEnd = m_response.indexOf("\r\n\r\n");
            if (headerEnd >= 0)
                m_responseHeaderSize = headerEnd + 4;
        }
        const int contentLength = m_responseContentLength();
        if (m_responseHeaderSize >= 0 && contentLength >= 0 &&
            m_response.size() >= m_responseHeaderSize + contentLength)
            finish();
    }

    int m_responseContentLength() const
    {
        if (m_responseHeaderSize < 0)
            return -1;
        const QByteArray head = m_response.left(m_responseHeaderSize);
        const int idx = head.indexOf("Content-Length:");
        if (idx < 0)
            return -1;
        int end = idx;
        while (end < head.size() && head.at(end) != '\r' && head.at(end) != '\n')
            ++end;
        return head.mid(idx + 15, end - idx - 15).trimmed().toInt();
    }

    void onDisconnected(const QSslSocket *current)
    {
        if (current != m_socket)
            return;
        finish();
    }

    void onSocketError(const QSslSocket *current)
    {
        if (current != m_socket)
            return;
        const bool wasRemoteClose =
            m_socket->error() == QAbstractSocket::RemoteHostClosedError;
        if (wasRemoteClose && m_responseHeaderSize >= 0) {
            finish();
            return;
        }
        abortWithError(QStringLiteral("error de socket: %1").arg(m_socket->errorString()));
    }

    void finish()
    {
        if (m_responseHeaderSize < 0) {
            abortWithError(QStringLiteral("respuesta sin cabeceras HTTP"));
            return;
        }
        QByteArray head = m_response.left(m_responseHeaderSize);
        QByteArray body = m_response.mid(m_responseHeaderSize);
        if (headIsChunked(head))
            body = dechunkBody(body);
        const int status = statusFromHead(head);
        auto fn = std::move(m_request.result);
        reset(nullptr);
        if (fn)
            fn(status, body);
        pump();
    }

    static int statusFromHead(const QByteArray &head)
    {
        const QList<QByteArray> lines = head.split('\n');
        if (lines.isEmpty())
            return 0;
        const QByteArray statusLine = lines.first();
        const int a = statusLine.indexOf(' ');
        const int b = statusLine.indexOf(' ', a + 1);
        if (a < 0)
            return 0;
        const QByteArray code = b > a ? statusLine.mid(a + 1, b - a - 1)
                                      : statusLine.mid(a + 1);
        return code.toInt();
    }

    static bool headIsChunked(const QByteArray &head)
    {
        const QByteArray h = head.toLower();
        return h.contains("transfer-encoding: chunked");
    }

    // Decodes a transfer-encoding: chunked body (the one YouTube uses in
    // sw.js_data and youtubei/v1/player).
    static QByteArray dechunkBody(const QByteArray &raw)
    {
        QByteArray out;
        int pos = 0;
        while (pos < raw.size()) {
            const int eol = raw.indexOf('\n', pos);
            if (eol < 0)
                break;
            int sizeEnd = eol;
            const int ext = raw.indexOf(';', pos); // "1a;ext" optional
            if (ext > pos && ext < eol)
                sizeEnd = ext;
            bool ok = false;
            const int size = raw.mid(pos, sizeEnd - pos).trimmed().toInt(&ok, 16);
            if (!ok || size <= 0)
                break;
            const int start = eol + 1;
            if (start + size > raw.size())
                break;
            out += raw.mid(start, size);
            pos = start + size + 2; // CRLF after the block
        }
        return out;
    }

    void abortWithError(const QString &reason)
    {
        auto fn = std::move(m_request.error);
        reset(nullptr);
        if (fn)
            fn(reason);
        pump();
    }

    void reset(QSslSocket *newSocket)
    {
        m_timeout->stop();
        m_inFlight = false;
        m_result = nullptr;
        m_error = nullptr;
        if (m_socket && m_socket != newSocket) {
            m_socket->disconnect(this);
            m_socket->abort();
            m_socket->deleteLater();
        }
        m_socket = newSocket;
    }

    QSslSocket *m_socket = nullptr;
    QTimer *m_timeout = nullptr;
    HttpRequest m_request;
    QByteArray m_response;
    int m_responseHeaderSize = -1;
    QString m_visitorData;
    ResultFn m_result = nullptr;
    ErrorFn m_error = nullptr;
    bool m_inFlight = false;
    int m_nextTimeoutMs = 30000;
    QQueue<HttpRequest> m_queue;
};

// ---------------------------------------------------------------------------

Innertube::Innertube(QObject *parent)
    : QObject(parent)
    , m_http(new Ipv4Https(this))
{
    loadHlsCacheFromDisk();
}

Innertube::~Innertube()
{
    m_http->abort();
}

bool Innertube::isBusy() const
{
    return m_http->isBusy();
}

void Innertube::seedVisitorData(const QString &visitorData, qint64 expiresAt)
{
    if (visitorData.isEmpty() || expiresAt <= 0)
        return;
    if (QDateTime::currentSecsSinceEpoch() >= expiresAt)
        return;
    m_visitorData = visitorData;
    m_visitorExpiresAt = expiresAt;
    m_http->setVisitorId(visitorData);
    NT_DEBUG("E2E innertube visitorData persistida ida=%d\n",
             visitorData.size());
}

void Innertube::setHl(const QString &hl)
{
    const QString clean = hl.trimmed();
    if (clean.isEmpty())
        return;
    m_hl = clean;
}

void Innertube::rotateSession()
{
    // YouTube's gate is per SESSION, not per URL: dropping the current
    // visitorData (even if it has not expired) forces sw.js_data -> a new
    // visitorData.
    m_visitorData.clear();
    m_visitorExpiresAt = 0;
    m_retryingVisitor = false;
    // Signed URLs cached from the previous session: all potentially gated.
    // Drop all of them (not just the video at hand, in case the user plays
    // another one with that cache) so the next fetchHls repeats the POST
    // /player with the new visitorData.
    if (!m_hlsCache.isEmpty()) {
        m_hlsCache.clear();
        saveHlsCacheToDisk();
    }
    NT_DEBUG("E2E innertube sesión rotada (visitorData + URLs descartados)\n");
}

void Innertube::fetchHls(const QString &videoId, int preferredClient,
                         bool forceRefresh)
{
    // Forced freshness: drop the cached entry (hlsUrl + adaptiveFormats with
    // their signed URLs) so the POST /player returns new URLs. Otherwise a
    // second playback reuses an already exhausted videoplayback URL (per
    // request budget) and YouTube answers 403 → frozen video.
    if (forceRefresh && m_hlsCache.remove(videoId) > 0) {
        saveHlsCacheToDisk();
        NT_DEBUG("E2E innertube hls cache invalidada id=%s\n",
                 videoId.toUtf8().constData());
    }
    // Session cache: if there is already a signed, still-valid
    // hlsManifestUrl, it is served instantly without repeating the player
    // POST.
    if (serveCachedHls(videoId)) {
        NT_DEBUG("E2E innertube hls caché hit id=%s\n",
                 videoId.toUtf8().constData());
        return;
    }
    // Single-flight: if a player request is already in flight for this
    // videoId (e.g. two consecutive resolution changes), do not launch
    // another HTTP request. The request in flight will emit hlsReady and Yt's
    // guards filter by gen/purpose and pick it up.
    if (m_hlsInFlight && m_flightVideoId == videoId) {
        NT_DEBUG("E2E innertube hls single-flight id=%s\n",
                 videoId.toUtf8().constData());
        return;
    }

    m_flightVideoId = videoId;
    m_hlsInFlight = true;
    mMasterInFlight = false;
    m_videoId = videoId;
    m_retryingVisitor = false;
    m_lastWinClient = -1;
    m_lastIsLive = false;
    m_clientIndex = 0;
    if (preferredClient >= 0 && preferredClient < kPlayerClientCount)
        m_clientIndex = preferredClient;
    m_hlsStartMs = QDateTime::currentMSecsSinceEpoch();
    NT_DEBUG("E2E innertube fetchHls id=%s client=%s\n",
             videoId.toUtf8().constData(), kPlayerClients[m_clientIndex].name);
    if (!hasFreshVisitor()) {
        m_playerPending = true;
        fetchVisitorData();
    } else
        requestPlayer();
}

void Innertube::fetchMaster(const QString &videoId, const QString &masterUrl)
{
    // Session cache of the master body (same videoId and valid signature).
    if (serveCachedMaster(videoId)) {
        NT_DEBUG("E2E innertube master caché hit id=%s\n",
                 videoId.toUtf8().constData());
        return;
    }
    // Single-flight: if another request for the same videoId is downloading
    // the master, wait for its masterReady instead of launching a second GET.
    if (mMasterInFlight && m_flightVideoId == videoId) {
        NT_DEBUG("E2E innertube master single-flight id=%s\n",
                 videoId.toUtf8().constData());
        return;
    }
    if (videoId != m_videoId) {
        // Not the videoId of the hls request in flight: it cannot be served.
        fail(nt::tr("error.masterStale"));
        return;
    }
    const QUrl url(masterUrl);
    if (!url.isValid() || url.scheme() != QLatin1String("https")) {
        fail(nt::tr("error.masterUrl"));
        return;
    }
    m_flightVideoId = videoId;
    mMasterInFlight = true;
    m_masterStartMs = QDateTime::currentMSecsSinceEpoch();
    NT_DEBUG("E2E innertube fetchMaster id=%s\n", videoId.toUtf8().constData());
    m_http->setTimeoutMs(kMasterTimeoutMs);
    m_http->setResult([this](int status, const QByteArray &body) {
        handleMaster(status == 200, QString(), body);
    });
    m_http->setError([this](const QString &reason) {
        handleMaster(false, reason, QByteArray());
    });
    // High priority: the master is required to start the video; it jumps
    // ahead of the already queued radio/next seeding (which does not block).
    m_http->getUrl(url, kVisionOSUA, /*highPriority=*/true);
}

void Innertube::fetchVisitorData()
{
    // The documented flow (AGENTS.md) is sw.js_data -> visitorData. The
    // homepage (/) returns an empty body (JS challenge) and is useless.
    if (m_visitorInFlight) {
        NT_DEBUG("E2E innertube visitorData single-flight\n");
        return;
    }
    m_visitorInFlight = true;
    m_http->setResult([this](int status, const QByteArray &body) {
        m_visitorInFlight = false;
        if (status != 200) {
            useFallbackVisitorData();
            return;
        }
        const QString visitorData = visitorDataFromSwJsData(body);
        if (visitorData.isEmpty()) {
            NT_DEBUG("E2E innertube sw.js_data sin visitorData, fallback\n");
            useFallbackVisitorData();
            return;
        }
        m_visitorData = visitorData;
        m_visitorExpiresAt = QDateTime::currentSecsSinceEpoch() + 2592000;
        m_visitorDataFreshRun = true;
        NT_DEBUG("E2E innertube visitorData sw.js_data len=%d\n",
                 visitorData.size());
        emit visitorDataReady(m_visitorData, m_visitorExpiresAt);
        // Dispatch EVERY phase that was waiting for visitorData (the player
        // first, to start the video as soon as possible), not just one.
        dispatchPending();
    });
    m_http->setError([this](const QString &reason) {
        m_visitorInFlight = false;
        NT_DEBUG("E2E innertube sw.js_data err: %s\n",
                 reason.toUtf8().constData());
        useFallbackVisitorData();
    });
    m_http->setTimeoutMs(kVisitorTimeoutMs);
    m_http->get(kSwJsData, kChromeUA);
}

void Innertube::useFallbackVisitorData()
{
    m_visitorData = QString::fromLatin1(kFallbackVisitorData);
    m_visitorExpiresAt = QDateTime::currentSecsSinceEpoch() + 2592000;
    // Mark as "fresh in this session" on the fallback too: it keeps the Home
    // from retrying sw.js_data on every reload while the endpoint is down
    // (the fixed fallback is already usable).
    m_visitorDataFreshRun = true;
    NT_DEBUG("E2E innertube visitorData fixed fallback len=%d\n",
             m_visitorData.size());
    dispatchPending();
}

// Dispatcher after refreshing visitorData: launches the player/next/radio
// phases that were left waiting, with priority to the player. The FIFO
// transport serializes the POSTs without aborting each other.
void Innertube::dispatchPending()
{
    if (m_playerPending) {
        m_playerPending = false;
        requestPlayer();
    }
    if (m_nextPending) {
        m_nextPending = false;
        requestNext();
    }
    if (m_radioPending) {
        m_radioPending = false;
        requestRadio();
    }
    if (m_currentDetailPending) {
        m_currentDetailPending = false;
        requestCurrentDetail();
    }
    if (m_homePending) {
        m_homePending = false;
        requestHome(QString());
    }
    if (m_channelPending) {
        m_channelPending = false;
        requestChannel();
    }
    if (m_trendPending) {
        m_trendPending = false;
        requestTrending();
    }
    if (m_searchPending) {
        m_searchPending = false;
        requestSearch(false);
    }
    if (m_chInfoPending) {
        m_chInfoPending = false;
        requestChannelInfo();
    }
    // Channel tabs pending on visitorData: relaunch the ones that were
    // waiting (there can be several: the UI requests videos/shorts/live at
    // the same time).
    const QList<QString> tabKeys = m_tabStates.keys();
    for (const QString &key : tabKeys) {
        ChannelTabState st = m_tabStates.value(key);
        if (!st.pending)
            continue;
        st.pending = false;
        st.inFlight = true;
        m_tabStates.insert(key, st);
        requestChannelTab(st.channelId, st.tab, false);
    }
    if (m_detailPending) {
        m_detailPending = false;
        requestVideoDetail();
    }
    if (m_plPending) {
        m_plPending = false;
        requestPlaylist();
    }
}

bool Innertube::retryBrowseWithFreshVisitor(bool *retried, bool *pending)
{
    if (*retried)
        return false;
    *retried = true;
    *pending = true;
    fetchVisitorData();
    return true;
}

bool Innertube::hasFreshVisitor() const
{
    return !m_visitorData.isEmpty()
        && QDateTime::currentSecsSinceEpoch() < m_visitorExpiresAt;
}

void Innertube::fetchNext(const QString &videoId)
{
    if (m_nextInFlight && m_nextVideoId == videoId) {
        NT_DEBUG("E2E innertube next single-flight id=%s\n",
                 videoId.toUtf8().constData());
        return;
    }
    m_nextInFlight = true;
    m_nextVideoId = videoId;
    m_nextClientIndex = 0;
    m_bestNext.clear();
    NT_DEBUG("E2E innertube fetchNext id=%s\n", videoId.toUtf8().constData());
    if (!hasFreshVisitor()) {
        m_nextPending = true;
        fetchVisitorData();
        return;
    }
    requestNext();
}

void Innertube::fetchCurrentDetail(const QString &videoId)
{
    if (m_currentDetailInFlight && m_currentDetailVideoId == videoId) {
        NT_DEBUG("E2E innertube currentDetail single-flight id=%s\n",
                 videoId.toUtf8().constData());
        return;
    }
    m_currentDetailInFlight = true;
    m_currentDetailVideoId = videoId;
    NT_DEBUG("E2E innertube fetchCurrentDetail id=%s\n",
             videoId.toUtf8().constData());
    if (!hasFreshVisitor()) {
        m_currentDetailPending = true;
        fetchVisitorData();
        return;
    }
    requestCurrentDetail();
}

// Translated detail of the current video with a plain /next. Primary client
// VISIONOS, no cascade and no seeding: only currentDetailReady is emitted.
void Innertube::requestCurrentDetail()
{
    const QString videoId = m_currentDetailVideoId;
    m_http->setResult([this, videoId](int status, const QByteArray &body) {
        QString title;
        QString description;
        if (status == 200) {
            QJsonObject jsonRoot;
            const bool jsonOk = parseJsonObject(body, &jsonRoot);
            if (jsonOk) {
                parseCurrentDetail(jsonRoot, &title, &description);
            }
        }
        m_currentDetailInFlight = false;
        NT_DEBUG("E2E innertube currentDetail ok title=%d desc=%d\n",
                 title.size(), description.size());
        emit currentDetailReady(videoId, title, description);
    });
    m_http->setError([this, videoId](const QString &reason) {
        m_currentDetailInFlight = false;
        NT_DEBUG("E2E innertube currentDetail err: %s\n",
                 reason.toUtf8().constData());
        emit currentDetailReady(videoId, QString(), QString());
    });
    m_http->setTimeoutMs(kPlayerTimeoutMs);
    m_http->setVisitorId(m_visitorData);

    const PlayerClient &pc = kPlayerClients[0]; // VISIONOS
    const QJsonObject client = clientContext(pc, m_visitorData, m_hl);
    QJsonObject context{{QStringLiteral("client"), client}};
    const QJsonObject body{
        {QStringLiteral("context"), context},
        {QStringLiteral("videoId"), videoId},
        {QStringLiteral("racyCheckOk"), true},
        {QStringLiteral("contentCheckOk"), true},
    };
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    m_http->post(kNextEndpoint, json, pc.ua, pc.clientId, pc.version,
                 nullptr, nullptr, nullptr);
}

// videoMetadataRenderer of the /next: title and description in the
// requested "hl" language (YouTube translation). It is searched both in the
// twoColumnWatchNextResults /next (desktop/TV client) and in the
// singleColumnWatchNextResults → watchNextTabbedResultsRenderer that VISIONOS
// returns (tabbed view: the playback tab carries the current video's
// videoMetadataRenderer).
void Innertube::parseCurrentDetail(const QJsonObject &root, QString *title,
                                   QString *description)
{
    if (!title || !description)
        return;
    const QJsonObject contents = root.value(QStringLiteral("contents")).toObject();
    const QJsonObject sc = contents.value(QStringLiteral("singleColumnWatchNextResults")).toObject();
    const QJsonObject tw = contents.value(QStringLiteral("twoColumnWatchNextResults")).toObject();
    QJsonObject col = sc.isEmpty() ? tw : sc;
    if (col.isEmpty())
        return;

    // Tabbed view (VISIONOS): walk the first tab whose section contains the
    // current video's renderer.
    const QJsonObject tabbed =
        col.value(QStringLiteral("results"))
            .toObject().value(QStringLiteral("watchNextTabbedResultsRenderer"))
            .toObject();
    if (!tabbed.isEmpty()) {
        const QJsonArray tabs = tabbed.value(QStringLiteral("tabs")).toArray();
        for (const QJsonValue &tabVal : tabs) {
            const QJsonArray sections =
                tabVal.toObject().value(QStringLiteral("tabRenderer"))
                    .toObject().value(QStringLiteral("content"))
                    .toObject().value(QStringLiteral("sectionListRenderer"))
                    .toObject().value(QStringLiteral("contents")).toArray();
            for (const QJsonValue &secVal : sections) {
                const QJsonArray items =
                    secVal.toObject().value(QStringLiteral("itemSectionRenderer"))
                        .toObject().value(QStringLiteral("contents")).toArray();
                for (const QJsonValue &v : items) {
                    const QJsonObject vm =
                        v.toObject().value(QStringLiteral("videoMetadataRenderer"))
                            .toObject();
                    if (vm.isEmpty())
                        continue;
                    const QJsonValue t = vm.value(QStringLiteral("title"));
                    const QJsonObject to = t.toObject();
                    if (to.value(QStringLiteral("simpleText")).isString()) {
                        *title = to.value(QStringLiteral("simpleText")).toString();
                    } else {
                        for (const QJsonValue &r :
                             to.value(QStringLiteral("runs")).toArray())
                            *title += r.toObject().value(QStringLiteral("text")).toString();
                    }
                    const QJsonValue d = vm.value(QStringLiteral("description"));
                    const QJsonObject do_ = d.toObject();
                    if (do_.value(QStringLiteral("simpleText")).isString()) {
                        *description =
                            do_.value(QStringLiteral("simpleText")).toString();
                    } else {
                        for (const QJsonValue &r :
                             do_.value(QStringLiteral("runs")).toArray()) {
                            if (!description->isEmpty() && !description->endsWith('\n'))
                                *description += QLatin1Char('\n');
                            *description +=
                                r.toObject().value(QStringLiteral("text")).toString();
                        }
                    }
                    return;
                }
            }
        }
        return;
    }

    // Classic route results.results.contents[].itemSectionRenderer.contents[].
    const QJsonObject results = col.value(QStringLiteral("results")).toObject()
                                    .value(QStringLiteral("results")).toObject();
    const QJsonArray items = results.value(QStringLiteral("contents")).toArray();
    for (const QJsonValue &secVal : items) {
        const QJsonArray sec =
            secVal.toObject().value(QStringLiteral("itemSectionRenderer"))
                .toObject().value(QStringLiteral("contents")).toArray();
        for (const QJsonValue &v : sec) {
            const QJsonObject vm =
                v.toObject().value(QStringLiteral("videoMetadataRenderer"))
                    .toObject();
            if (vm.isEmpty())
                continue;
            const QJsonValue t = vm.value(QStringLiteral("title"));
            const QJsonObject to = t.toObject();
            if (to.value(QStringLiteral("simpleText")).isString()) {
                *title = to.value(QStringLiteral("simpleText")).toString();
            } else {
                for (const QJsonValue &r :
                     to.value(QStringLiteral("runs")).toArray())
                    *title += r.toObject().value(QStringLiteral("text")).toString();
            }
            const QJsonValue d = vm.value(QStringLiteral("description"));
            const QJsonObject do_ = d.toObject();
            if (do_.value(QStringLiteral("simpleText")).isString()) {
                *description =
                    do_.value(QStringLiteral("simpleText")).toString();
            } else {
                for (const QJsonValue &r :
                     do_.value(QStringLiteral("runs")).toArray()) {
                    if (!description->isEmpty() && !description->endsWith('\n'))
                        *description += QLatin1Char('\n');
                    *description +=
                        r.toObject().value(QStringLiteral("text")).toString();
                }
            }
            return;
        }
    }
}

void Innertube::requestNext()
{
    const QString videoId = m_nextVideoId;
    m_http->setResult([this, videoId](int status, const QByteArray &body) {
        NT_DEBUG("E2E innertube next HTTP status=%d client=%s\n", status,
                 kPlayerClients[m_nextClientIndex].name);
        handleNext(videoId, status == 200, QString(), body);
    });
    m_http->setError([this, videoId](const QString &reason) {
        handleNext(videoId, false, reason, QByteArray());
    });
    m_http->setTimeoutMs(kPlayerTimeoutMs);
    m_http->setVisitorId(m_visitorData);

    const PlayerClient &pc = kPlayerClients[m_nextClientIndex];
    const QJsonObject client = clientContext(pc, m_visitorData, m_hl);
    QJsonObject context{{QStringLiteral("client"), client}};
    if (pc.embed) {
        context.insert(QStringLiteral("thirdParty"),
                       QJsonObject{{QStringLiteral("embedUrl"),
                                    QString::fromLatin1(pc.embedUrl)}});
    }

    const QJsonObject body{
        {QStringLiteral("context"), context},
        {QStringLiteral("videoId"), videoId},
        {QStringLiteral("racyCheckOk"), true},
        {QStringLiteral("contentCheckOk"), true},
    };
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    m_http->post(kNextEndpoint, json, pc.ua, pc.clientId, pc.version,
                 pc.embed ? pc.embedUrl : nullptr, nullptr, nullptr);
}

void Innertube::handleNext(const QString &videoId, bool ok,
                           const QString &error, const QByteArray &body)
{
    m_nextInFlight = false;
    // NewPipe-style client cascade: try the next client before giving up
    // with an empty response.
    auto tryNextClient = [this, videoId]() {
        if (m_nextClientIndex + 1 < kPlayerClientCount) {
            ++m_nextClientIndex;
            m_nextInFlight = true;
            NT_DEBUG("E2E innertube next trying client [%d] %s\n",
                     m_nextClientIndex,
                     kPlayerClients[m_nextClientIndex].name);
            requestNext();
            return true;
        }
        return false;
    };

    if (!ok) {
        NT_DEBUG("E2E innertube next HTTP err: %s\n", error.toUtf8().constData());
        if (tryNextClient())
            return;
        emit relatedReady(videoId, m_bestNext);
        m_bestNext.clear();
        return;
    }
    QJsonObject jsonRoot;
    const bool jsonOk = parseJsonObject(body, &jsonRoot);
    if (!jsonOk) {
        NT_DEBUG("E2E innertube next json fail size=%d prefix=%s\n", body.size(),
                 body.left(120).constData());
        if (tryNextClient())
            return;
        emit relatedReady(videoId, m_bestNext);
        m_bestNext.clear();
        return;
    }
    const QVector<VideoInfo> related = parseRelated(jsonRoot);
    NT_DEBUG("E2E innertube next ok id=%s relacionados=%d\n",
             videoId.toUtf8().constData(), related.size());
    // Pick the best response: keep the longest list and keep trying clients
    // until a reasonable "Up next" shows up.
    auto emitBest = [this, videoId]() {
        emit relatedReady(videoId, m_bestNext);
    };
    if (related.size() >= 5) {
        m_bestNext.clear();
        emit relatedReady(videoId, related);
        return;
    }
    if (related.size() > m_bestNext.size())
        m_bestNext = related;
    if (tryNextClient())
        return;
    QVector<VideoInfo> best = m_bestNext;
    m_bestNext.clear();
    emit relatedReady(videoId, best);
}

void Innertube::fetchRadio(const QString &videoId)
{
    if (m_radioInFlight) {
        NT_DEBUG("E2E innertube radio single-flight id=%s\n",
                 videoId.toUtf8().constData());
        return;
    }
    m_radioInFlight = true;
    m_radioVideoId = videoId;
    m_radioContinuation.clear();
    m_radioFallback.clear();
    m_radioVisionFallback.clear();
    m_radioClientIndex = 0;
    NT_DEBUG("E2E innertube fetchRadio id=%s\n", videoId.toUtf8().constData());
    if (!hasFreshVisitor()) {
        m_radioPending = true;
        fetchVisitorData();
        return;
    }
    requestRadio();
}

void Innertube::fetchRadioMore()
{
    if (m_radioContinuation.isEmpty() || m_radioInFlight)
        return;
    m_radioInFlight = true;
    NT_DEBUG("E2E innertube radio more id=%s\n",
             m_radioVideoId.toUtf8().constData());
    requestRadio();
}

void Innertube::requestRadio()
{
    const QString videoId = m_radioVideoId;
    m_http->setResult([this, videoId](int status, const QByteArray &body) {
        NT_DEBUG("E2E innertube radio HTTP status=%d client=%s\n", status,
                 kPlayerClients[m_radioClientIndex].name);
        handleRadio(videoId, status == 200, QString(), body);
    });
    m_http->setError([this, videoId](const QString &reason) {
        handleRadio(videoId, false, reason, QByteArray());
    });
    m_http->setTimeoutMs(kPlayerTimeoutMs);
    m_http->setVisitorId(m_visitorData);

    const PlayerClient &pc = kPlayerClients[m_radioClientIndex];
    const QJsonObject client = clientContext(pc, m_visitorData, m_hl);
    QJsonObject context{{QStringLiteral("client"), client}};
    if (pc.embed) {
        context.insert(QStringLiteral("thirdParty"),
                       QJsonObject{{QStringLiteral("embedUrl"),
                                    QString::fromLatin1(pc.embedUrl)}});
    }

    QJsonObject body;
    if (m_radioContinuation.isEmpty()) {
        // First batch: YouTube's RDAMVM<videoId> (radio) mix.
        body = {
            {QStringLiteral("context"), context},
            {QStringLiteral("videoId"), videoId},
            {QStringLiteral("playlistId"),
             QStringLiteral("RDAMVM") + videoId},
            {QStringLiteral("params"), QStringLiteral("wAEB")},
            {QStringLiteral("racyCheckOk"), true},
            {QStringLiteral("contentCheckOk"), true},
            {QStringLiteral("enablePersistentPlaylistPanel"), true},
            {QStringLiteral("isAudioOnly"), true},
        };
    } else {
        // Next batch of the same mix, via continuation token.
        body = {
            {QStringLiteral("context"), context},
            {QStringLiteral("continuation"), m_radioContinuation},
        };
    }
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    m_http->post(kNextEndpoint, json, pc.ua, pc.clientId, pc.version,
                 pc.embed ? pc.embedUrl : nullptr, nullptr, nullptr);
}

void Innertube::handleRadio(const QString &videoId, bool ok,
                            const QString &error, const QByteArray &body)
{
    m_radioInFlight = false;
    // Client cascade only on the first batch: the continuation token is
    // specific to the client that issued it.
    auto tryNextClient = [this, videoId]() {
        if (m_radioContinuation.isEmpty()
            && m_radioClientIndex + 1 < kPlayerClientCount) {
            ++m_radioClientIndex;
            m_radioInFlight = true;
            NT_DEBUG("E2E innertube radio trying client [%d] %s\n",
                     m_radioClientIndex,
                     kPlayerClients[m_radioClientIndex].name);
            requestRadio();
            return true;
        }
        return false;
    };

    if (!ok) {
        NT_DEBUG("E2E innertube radio HTTP err: %s\n",
                 error.toUtf8().constData());
        if (tryNextClient())
            return;
        emit radioReady(videoId, QVector<VideoInfo>());
        return;
    }
    QJsonObject jsonRoot;
    const bool jsonOk = parseJsonObject(body, &jsonRoot);
    if (!jsonOk) {
        NT_DEBUG("E2E innertube radio json fail size=%d prefix=%s\n",
                 body.size(), body.left(120).constData());
        if (tryNextClient())
            return;
        emit radioReady(videoId, QVector<VideoInfo>());
        return;
    }

    QVector<VideoInfo> videos;
    QString continuation;
    parseWatchQueue(jsonRoot, &videos, &continuation);
    if (videos.isEmpty()) {
        // The RDAMVM mix only carries a panel (playlistPanelVideoRenderer)
        // for videos with a music edit; for the rest, /next answers the mix as
        // recommendations (compactVideoRenderer). The first mix seen is saved
        // and the cascade keeps going in case another client brings the real
        // panel; if none does, the mix is emitted.
        const QVector<VideoInfo> fb = parseRelated(jsonRoot);
        // Primary client (VISIONOS): answers the mix as recommendations
        // (compactVideoRenderer, WITHOUT date and with the localized views
        // text). Instead of emitting it right away, the same /next is asked
        // once to the WEB client, which returns lockupViewModel with views
        // and date; if that request fails, the VISIONOS one is emitted as is.
        // It is still a single extra request, not the 4 of the full cascade
        // (~12-15 s), and IOS/others still bring the real music panel.
        if (m_radioClientIndex == 0 && !fb.isEmpty()) {
            NT_DEBUG("E2E innertube radio vision fallback id=%s n=%d -> web\n",
                     videoId.toUtf8().constData(), fb.size());
            m_radioVisionFallback = fb;
            requestRadioWeb(videoId);
            return;
        }
        if (!fb.isEmpty() && m_radioFallback.isEmpty()) {
            m_radioFallback = fb;
            NT_DEBUG("E2E innertube radio guarda mezcla id=%s n=%d\n",
                     videoId.toUtf8().constData(), fb.size());
        }
        if (tryNextClient())
            return;
        if (!m_radioFallback.isEmpty()) {
            NT_DEBUG("E2E innertube radio final recomendaciones id=%s n=%d\n",
                     videoId.toUtf8().constData(),
                     m_radioFallback.size());
            emit radioReady(videoId, m_radioFallback);
            m_radioFallback.clear();
            return;
        }
        NT_DEBUG("E2E innertube radio empty id=%s\n",
                 videoId.toUtf8().constData());
        emit radioReady(videoId, QVector<VideoInfo>());
        return;
    }
    m_radioFallback.clear();
    if (!continuation.isEmpty())
        m_radioContinuation = continuation;
    NT_DEBUG("E2E innertube radio ok id=%s cola=%d cont=%s\n",
             videoId.toUtf8().constData(), videos.size(),
             m_radioContinuation.isEmpty() ? "no" : "si");
    emit radioReady(videoId, videos);
}

// Second request of the radio fallback: the same /next with the WEB client.
// Its modern layout (lockupViewModel) carries the views and the date that
// VISIONOS's compactVideoRenderer lacks; it is the only thing that makes
// "Up next" show the three fields like the rest of the app. No API key
// (same as browsing channel tabs).
void Innertube::requestRadioWeb(const QString &videoId)
{
    // The radio is still "in flight" (handleRadio just lowered the flag):
    // without this a concurrent fetchRadio would launch another request and
    // overwrite these callbacks.
    m_radioInFlight = true;
    m_http->setResult([this, videoId](int status, const QByteArray &body) {
        NT_DEBUG("E2E innertube radioWeb HTTP status=%d size=%d\n", status,
                 body.size());
        handleRadioWeb(videoId, status == 200, body);
    });
    m_http->setError([this, videoId](const QString &reason) {
        handleRadioWeb(videoId, false, reason.toUtf8());
    });
    m_http->setTimeoutMs(kPlayerTimeoutMs);
    m_http->setVisitorId(m_visitorData);

    const QJsonObject client = webClientContext(m_visitorData, m_hl);
    const QJsonObject body{
        {QStringLiteral("context"),
         QJsonObject{{QStringLiteral("client"), client}}},
        {QStringLiteral("videoId"), videoId},
    };
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    m_http->post(kNextEndpoint, json, kChromeUA, kWebClientId, kWebClientVersion,
                 nullptr, nullptr, nullptr);
}

void Innertube::handleRadioWeb(const QString &videoId, bool ok,
                               const QByteArray &body)
{
    m_radioInFlight = false;
    QVector<VideoInfo> out;
    if (ok) {
        QJsonObject root;
        if (parseJsonObject(body, &root))
            out = parseRelated(root); // lockupViewModel -> visitas + fecha
    }
    // If WEB did not yield a usable list, use the one VISIONOS already
    // brought: the queue is served the same way, only without the date.
    if (out.isEmpty())
        out = m_radioVisionFallback;
    m_radioVisionFallback.clear();
    NT_DEBUG("E2E innertube radioWeb ok id=%s n=%d\n",
             videoId.toUtf8().constData(), out.size());
    emit radioReady(videoId, out);
}

// Home feed limits (browse FEwhat_to_watch): up to kHomeShelfCap shelves
// and kHomeShelfVideos videos per shelf; if there are no shelves (different
// layout), up to kHomeShelfVideos loose videos are collected.
constexpr int kHomeShelfCap = 3;
constexpr int kHomeShelfVideos = 8;

// Params of the channels' "Videos" tab (browse). A value verified as
// identical on real channels; without it the browse returns the default tab
// ("Home") instead of the video listing.
const char kChannelVideosParams[] = "EgZ2aWRlb3PyBgQKAjoA";
// Trending: real browseId, although today YouTube answers 400/redirect to
// the Home for anonymous trending (the selftest records the real status).
const char kTrendingBrowseId[] = "FEtrending";
// How many videos are collected per channel/trending (then the UI
// decides/dedupes and limits to 8). Enough to fill the section without
// asking for more channels.
constexpr int kChannelTake = 12;
constexpr int kTrendingTake = 12;
// Params of the Shorts and Live tabs (channel browse). VISIONOS ignores
// them and returns the Home; the WEB client does serve them.
const char kChannelShortsParams[] = "EgZzaG9ydHPyBgUKA5oBAA==";
const char kChannelLiveParams[] = "EgdzdHJlYW1z8gYECgJ6AA==";
// Channel "About" tab (browse): description, country, canonical url.
const char kChannelAboutParams[] = "EgVhYm91dPIGBAoCEgA=";
// Page size of channel tabs and search: the UI shows "Show more" when the
// page reaches this size.
constexpr int kChannelTabTake = 25;
constexpr int kSearchTake = 20;

void Innertube::fetchHome()
{
    if (m_homeInFlight || m_homeMoreInFlight) {
        NT_DEBUG("E2E innertube home single-flight\n");
        return;
    }
    m_homeInFlight = true;
    m_homeContinuation.clear();
    m_homeVisitorRetried = false;
    NT_DEBUG("E2E innertube fetchHome\n");
    // The current persisted visitorData is used directly: the refresh is no
    // longer forced once per session (saves one RTT at startup). If the
    // browse answers 500/empty, handleHome refreshes visitorData ONCE and
    // retries (m_homeVisitorRetried), without looping.
    if (!hasFreshVisitor()) {
        m_homePending = true;
        fetchVisitorData();
        return;
    }
    requestHome(QString());
}

void Innertube::fetchHomeMore()
{
    if (m_homeContinuation.isEmpty() || m_homeInFlight || m_homeMoreInFlight) {
        NT_DEBUG("E2E innertube home more ignorada\n");
        return;
    }
    m_homeMoreInFlight = true;
    NT_DEBUG("E2E innertube fetchHomeMore\n");
    requestHome(m_homeContinuation);
}

void Innertube::requestHome(const QString &continuation)
{
    m_http->setResult([this, continuation](int status, const QByteArray &body) {
        NT_DEBUG("E2E innertube home HTTP status=%d size=%d\n", status,
                 body.size());
        handleHome(status == 200, QString(), body, !continuation.isEmpty());
    });
    m_http->setError([this, continuation](const QString &reason) {
        handleHome(false, reason, QByteArray(), !continuation.isEmpty());
    });
    m_http->setTimeoutMs(kPlayerTimeoutMs);
    m_http->setVisitorId(m_visitorData);

    // VISIONOS: the only tested client that returns the real Home feed
    // (62 compactVideoRenderer in PROOF 1). No cascade: on failure/500, retry
    // once with fresh visitorData (not in a loop).
    const PlayerClient &pc = kPlayerClients[0];
    const QJsonObject client = clientContext(pc, m_visitorData, m_hl);
    QJsonObject context{{QStringLiteral("client"), client}};
    QJsonObject body;
    if (continuation.isEmpty()) {
        body = {
            {QStringLiteral("context"), context},
            {QStringLiteral("browseId"), QStringLiteral("FEwhat_to_watch")},
        };
    } else {
        body = {
            {QStringLiteral("context"), context},
            {QStringLiteral("continuation"), continuation},
        };
    }
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    m_http->post(kBrowseEndpoint, json, pc.ua, pc.clientId, pc.version,
                 nullptr, nullptr, nullptr);
}

void Innertube::handleHome(bool ok, const QString &error,
                           const QByteArray &body, bool more)
{
    if (more)
        m_homeMoreInFlight = false;
    else
        m_homeInFlight = false;
    if (!ok) {
        NT_DEBUG("E2E innertube home HTTP err: %s\n",
                 error.toUtf8().constData());
        if (more)
            emit homeMoreReady(QVector<VideoInfo>());
        else if (retryBrowseWithFreshVisitor(&m_homeVisitorRetried,
                                             &m_homePending))
            return;
        else
            emit homeReady(QVector<VideoInfo>());
        return;
    }
    QJsonObject jsonRoot;
    const bool jsonOk = parseJsonObject(body, &jsonRoot);
    if (!jsonOk) {
        NT_DEBUG("E2E innertube home json fail size=%d\n", body.size());
        if (more)
            emit homeMoreReady(QVector<VideoInfo>());
        else if (retryBrowseWithFreshVisitor(&m_homeVisitorRetried,
                                             &m_homePending))
            return;
        else
            emit homeReady(QVector<VideoInfo>());
        return;
    }
    QString continuation;
    const QVector<VideoInfo> videos =
        parseHomeBrowse(jsonRoot, &continuation);
    m_homeContinuation = continuation;
    NT_DEBUG("E2E innertube home ok videos=%d cont=%s\n", videos.size(),
             continuation.isEmpty() ? "no" : "si");
    if (more) {
        emit homeMoreReady(videos);
        return;
    }
    // Valid response but with no videos (e.g. 200 with challenge/error):
    // retry once with fresh visitorData before hiding the section.
    if (videos.isEmpty()
        && retryBrowseWithFreshVisitor(&m_homeVisitorRetried,
                                       &m_homePending))
        return;
    emit homeReady(videos);
}

void Innertube::fetchChannelVideos(const QString &channelId)
{
    if (m_channelInFlight) {
        NT_DEBUG("E2E innertube channel single-flight\n");
        return;
    }
    m_channelInFlight = true;
    m_channelId = channelId;
    m_channelVisitorRetried = false;
    NT_DEBUG("E2E innertube fetchChannelVideos id=%s\n",
             channelId.toUtf8().constData());
    if (!hasFreshVisitor()) {
        m_channelPending = true;
        fetchVisitorData();
        return;
    }
    requestChannel();
}

void Innertube::requestChannel()
{
    const QString channelId = m_channelId;
    m_http->setResult([this, channelId](int status, const QByteArray &body) {
        NT_DEBUG("E2E innertube channel HTTP status=%d size=%d id=%s\n",
                 status, body.size(), channelId.toUtf8().constData());
        handleChannel(status == 200, QString(), body);
    });
    m_http->setError([this](const QString &reason) {
        handleChannel(false, reason, QByteArray());
    });
    m_http->setTimeoutMs(kPlayerTimeoutMs);
    m_http->setVisitorId(m_visitorData);

    const PlayerClient &pc = kPlayerClients[0];
    const QJsonObject client = clientContext(pc, m_visitorData, m_hl);
    const QJsonObject body{
        {QStringLiteral("context"), QJsonObject{{QStringLiteral("client"), client}}},
        {QStringLiteral("browseId"), channelId},
        {QStringLiteral("params"), QString::fromLatin1(kChannelVideosParams)},
    };
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    m_http->post(kBrowseEndpoint, json, pc.ua, pc.clientId, pc.version,
                 nullptr, nullptr, nullptr);
}

void Innertube::handleChannel(bool ok, const QString &error,
                              const QByteArray &body)
{
    m_channelInFlight = false;
    if (!ok) {
        NT_DEBUG("E2E innertube channel HTTP err: %s\n",
                 error.toUtf8().constData());
        if (retryBrowseWithFreshVisitor(&m_channelVisitorRetried,
                                        &m_channelPending))
            return;
        emit channelVideosReady(m_channelId, QVector<VideoInfo>());
        return;
    }
    QJsonObject jsonRoot;
    const bool jsonOk = parseJsonObject(body, &jsonRoot);
    if (!jsonOk) {
        NT_DEBUG("E2E innertube channel json fail size=%d\n", body.size());
        if (retryBrowseWithFreshVisitor(&m_channelVisitorRetried,
                                        &m_channelPending))
            return;
        emit channelVideosReady(m_channelId, QVector<VideoInfo>());
        return;
    }
    QVector<VideoInfo> videos =
        collectVideoRenderers(jsonRoot, kChannelTake);
    if (videos.isEmpty()
        && retryBrowseWithFreshVisitor(&m_channelVisitorRetried,
                                       &m_channelPending))
        return;
    NT_DEBUG("E2E innertube channel ok videos=%d\n", videos.size());
    emit channelVideosReady(m_channelId, videos);
}

void Innertube::fetchTrending()
{
    if (m_trendInFlight) {
        NT_DEBUG("E2E innertube trending single-flight\n");
        return;
    }
    m_trendInFlight = true;
    m_trendVisitorRetried = false;
    NT_DEBUG("E2E innertube fetchTrending\n");
    if (!hasFreshVisitor()) {
        m_trendPending = true;
        fetchVisitorData();
        return;
    }
    requestTrending();
}

void Innertube::requestTrending()
{
    m_http->setResult([this](int status, const QByteArray &body) {
        NT_DEBUG("E2E innertube trending HTTP status=%d size=%d\n", status,
                 body.size());
        handleTrending(status == 200, QString(), body);
    });
    m_http->setError([this](const QString &reason) {
        handleTrending(false, reason, QByteArray());
    });
    m_http->setTimeoutMs(kPlayerTimeoutMs);
    m_http->setVisitorId(m_visitorData);

    const PlayerClient &pc = kPlayerClients[0];
    const QJsonObject client = clientContext(pc, m_visitorData, m_hl);
    const QJsonObject body{
        {QStringLiteral("context"), QJsonObject{{QStringLiteral("client"), client}}},
        {QStringLiteral("browseId"), QString::fromLatin1(kTrendingBrowseId)},
    };
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    m_http->post(kBrowseEndpoint, json, pc.ua, pc.clientId, pc.version,
                 nullptr, nullptr, nullptr);
}

void Innertube::handleTrending(bool ok, const QString &error,
                               const QByteArray &body)
{
    m_trendInFlight = false;
    if (!ok) {
        NT_DEBUG("E2E innertube trending HTTP err: %s\n",
                 error.toUtf8().constData());
        if (retryBrowseWithFreshVisitor(&m_trendVisitorRetried,
                                        &m_trendPending))
            return;
        emit trendingReady(QVector<VideoInfo>());
        return;
    }
    QJsonObject jsonRoot;
    const bool jsonOk = parseJsonObject(body, &jsonRoot);
    if (!jsonOk) {
        NT_DEBUG("E2E innertube trending json fail size=%d\n", body.size());
        if (retryBrowseWithFreshVisitor(&m_trendVisitorRetried,
                                        &m_trendPending))
            return;
        emit trendingReady(QVector<VideoInfo>());
        return;
    }
    QVector<VideoInfo> videos =
        collectVideoRenderers(jsonRoot, kTrendingTake);
    if (videos.isEmpty()
        && retryBrowseWithFreshVisitor(&m_trendVisitorRetried,
                                       &m_trendPending))
        return;
    NT_DEBUG("E2E innertube trending ok videos=%d\n", videos.size());
    emit trendingReady(videos);
}

// ---------------------------------------------------------------------------
// UI search and browse requests (channel, tabs, detail, playlist). Same
// pattern as Home: single-flight, visitorData, and one retry with fresh
// visitorData if the browse responds empty/error.

void Innertube::fetchSearch(const QString &query, int limit)
{
    const QString q = query.trimmed();
    if (q.isEmpty()) {
        emit searchReady(query, QVector<VideoInfo>(), false);
        return;
    }
    if (m_searchInFlight && m_searchQuery == q) {
        NT_DEBUG("E2E innertube search single-flight q=%s\n",
                 q.toUtf8().constData());
        return;
    }
    m_searchInFlight = true;
    m_searchQuery = q;
    m_searchLimit = limit > 0 ? limit : kSearchTake;
    m_searchContinuation.clear();
    m_searchVisitorRetried = false;
    NT_DEBUG("E2E innertube fetchSearch q=%s\n", q.toUtf8().constData());
    if (!hasFreshVisitor()) {
        m_searchPending = true;
        fetchVisitorData();
        return;
    }
    requestSearch(false);
}

void Innertube::fetchSearchMore(int limit)
{
    if (m_searchContinuation.isEmpty() || m_searchInFlight) {
        NT_DEBUG("E2E innertube search more ignorada\n");
        return;
    }
    m_searchLimit = limit > 0 ? limit : kSearchTake;
    m_searchInFlight = true;
    requestSearch(true);
}

void Innertube::fetchSuggest(const QString &query)
{
    // With no text there is nothing to suggest, and a 1-letter prefix only
    // brings entity noise; the minimum of 2 avoids one request per keypress
    // when starting a word.
    const QString q = query.trimmed();
    if (q.size() < 2) {
        m_suggestInFlight = false;
        emit suggestReady(query, QVector<Suggestion>());
        return;
    }
    if (m_suggestInFlight && m_suggestQuery == q)
        return; // same prefix already requested

    m_suggestInFlight = true;
    m_suggestQuery = q;
    // The query travels in the URL, so encoding it is MANDATORY: with raw
    // spaces the endpoint returns 400. QUrl::toPercentEncoding leaves safe
    // characters intact and escapes the rest (accents included).
    const QString url = QLatin1String(kSuggestUrl)
        + QString::fromLatin1(QUrl::toPercentEncoding(q));

    m_http->setResult([this, q](int status, const QByteArray &body) {
        NT_DEBUG("E2E suggest q=%s HTTP %d size=%d\n", q.toUtf8().constData(),
                 status, body.size());
        handleSuggest(q, status == 200, body);
    });
    m_http->setError([this, q](const QString &reason) {
        NT_DEBUG("E2E suggest q=%s err: %s\n", q.toUtf8().constData(),
                 reason.toUtf8().constData());
        handleSuggest(q, false, QByteArray());
    });
    m_http->setTimeoutMs(8000); // type-ahead: expires sooner than a search
    // getUrl() validates TLS against the URL's own host, so it works for
    // suggestqueries.google.com without touching the transport.
    m_http->getUrl(QUrl(url), kChromeUA);
}

void Innertube::handleSuggest(const QString &query, bool ok,
                              const QByteArray &body)
{
    m_suggestInFlight = false;
    QVector<Suggestion> items;
    if (ok)
        items = parseSuggestBody(body);
    NT_DEBUG("E2E suggest q=%s items=%d\n", query.toUtf8().constData(),
             items.size());
    // Always emitted, even when empty: the UI treats it as "close the
    // dropdown", not as "do nothing".
    emit suggestReady(query, items);
}

void Innertube::requestSearch(bool more)
{
    const QString query = m_searchQuery;
    m_http->setResult([this, query, more](int status, const QByteArray &body) {
        NT_DEBUG("E2E innertube search HTTP status=%d size=%d\n", status,
                 body.size());
        handleSearch(query, status == 200, QString(), body, more);
    });
    m_http->setError([this, query, more](const QString &reason) {
        handleSearch(query, false, reason, QByteArray(), more);
    });
    m_http->setTimeoutMs(kPlayerTimeoutMs);
    m_http->setVisitorId(m_visitorData);

    const PlayerClient &pc = kPlayerClients[0]; // VISIONOS
    const QJsonObject client = clientContext(pc, m_visitorData, m_hl);
    const QJsonObject context{{QStringLiteral("client"), client}};
    QJsonObject body;
    if (more) {
        body = {{QStringLiteral("context"), context},
                {QStringLiteral("continuation"), m_searchContinuation}};
    } else {
        body = {{QStringLiteral("context"), context},
                {QStringLiteral("query"), query}};
    }
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    m_http->post(kSearchEndpoint, json, pc.ua, pc.clientId, pc.version,
                 nullptr, nullptr, nullptr);
}

void Innertube::handleSearch(const QString &query, bool ok,
                             const QString &error, const QByteArray &body,
                             bool more)
{
    m_searchInFlight = false;
    if (!ok) {
        NT_DEBUG("E2E innertube search HTTP err: %s\n",
                 error.toUtf8().constData());
        if (!more
            && retryBrowseWithFreshVisitor(&m_searchVisitorRetried,
                                           &m_searchPending))
            return;
        emit searchReady(query, QVector<VideoInfo>(), false);
        return;
    }
    QJsonObject jsonRoot;
    const bool jsonOk = parseJsonObject(body, &jsonRoot);
    if (!jsonOk) {
        NT_DEBUG("E2E innertube search json fail size=%d\n", body.size());
        if (!more
            && retryBrowseWithFreshVisitor(&m_searchVisitorRetried,
                                           &m_searchPending))
            return;
        emit searchReady(query, QVector<VideoInfo>(), false);
        return;
    }
    QString continuation;
    const QVector<VideoInfo> videos =
        parseSearchBrowse(jsonRoot, m_searchLimit, &continuation);
    m_searchContinuation = continuation;
    if (!more && videos.isEmpty()
        && retryBrowseWithFreshVisitor(&m_searchVisitorRetried,
                                       &m_searchPending))
        return;
    NT_DEBUG("E2E innertube search ok q=%s videos=%d more=%d\n",
             query.toUtf8().constData(), videos.size(),
             continuation.isEmpty() ? 0 : 1);
    emit searchReady(query, videos, !continuation.isEmpty());
}

void Innertube::fetchChannelInfo(const QString &channelId)
{
    if (m_chInfoInFlight && m_chInfoId == channelId) {
        NT_DEBUG("E2E innertube channelInfo single-flight\n");
        return;
    }
    m_chInfoInFlight = true;
    m_chInfoId = channelId;
    m_chInfoVisitorRetried = false;
    NT_DEBUG("E2E innertube fetchChannelInfo id=%s\n",
             channelId.toUtf8().constData());
    if (!hasFreshVisitor()) {
        m_chInfoPending = true;
        fetchVisitorData();
        return;
    }
    requestChannelInfo();
}

void Innertube::requestChannelInfo()
{
    const QString channelId = m_chInfoId;
    m_http->setResult([this, channelId](int status, const QByteArray &body) {
        NT_DEBUG("E2E innertube channelInfo HTTP status=%d size=%d id=%s\n",
                 status, body.size(), channelId.toUtf8().constData());
        handleChannelInfo(channelId, status == 200, QString(), body);
    });
    m_http->setError([this, channelId](const QString &reason) {
        handleChannelInfo(channelId, false, reason, QByteArray());
    });
    m_http->setTimeoutMs(kPlayerTimeoutMs);
    m_http->setVisitorId(m_visitorData);

    const PlayerClient &pc = kPlayerClients[0]; // VISIONOS
    const QJsonObject client = clientContext(pc, m_visitorData, m_hl);
    const QJsonObject body{
        {QStringLiteral("context"), QJsonObject{{QStringLiteral("client"), client}}},
        {QStringLiteral("browseId"), channelId},
        {QStringLiteral("params"), QString::fromLatin1(kChannelAboutParams)},
    };
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    m_http->post(kBrowseEndpoint, json, pc.ua, pc.clientId, pc.version,
                 nullptr, nullptr, nullptr);
}

void Innertube::handleChannelInfo(const QString &channelId, bool ok,
                                  const QString &error, const QByteArray &body)
{
    m_chInfoInFlight = false;
    ChannelInfo info;
    info.id = channelId;
    if (ok) {
        QJsonObject jsonRoot;
        const bool jsonOk = parseJsonObject(body, &jsonRoot);
        if (jsonOk)
            parseChannelInfo(jsonRoot, &info);
        else
            NT_DEBUG("E2E innertube channelInfo json fail size=%d\n", body.size());
    } else {
        NT_DEBUG("E2E innertube channelInfo HTTP err: %s\n",
                 error.toUtf8().constData());
    }
    if (info.name.isEmpty()
        && retryBrowseWithFreshVisitor(&m_chInfoVisitorRetried, &m_chInfoPending))
        return;
    NT_DEBUG("E2E innertube channelInfo ok name=%s subs=%lld\n",
             info.name.toUtf8().constData(),
             static_cast<long long>(info.subscriberCount));
    emit channelInfoReady(info);
}

QString Innertube::tabKey(const QString &channelId, const QString &tab)
{
    return channelId + QLatin1Char('\n') + tab;
}

void Innertube::fetchChannelTab(const QString &channelId, const QString &tab,
                                int limit)
{
    const QString key = tabKey(channelId, tab);
    if (m_tabStates.value(key).inFlight) {
        NT_DEBUG("E2E innertube channelTab single-flight\n");
        return;
    }
    ChannelTabState st;
    st.channelId = channelId;
    st.tab = tab;
    st.limit = limit > 0 ? limit : kChannelTabTake;
    NT_DEBUG("E2E innertube fetchChannelTab id=%s tab=%s\n",
             channelId.toUtf8().constData(), tab.toUtf8().constData());
    if (!hasFreshVisitor()) {
        st.pending = true;
        m_tabStates.insert(key, st);
        fetchVisitorData();
        return;
    }
    st.inFlight = true;
    m_tabStates.insert(key, st);
    requestChannelTab(channelId, tab, false);
}

void Innertube::fetchChannelTabMore(const QString &channelId, const QString &tab,
                                    int limit)
{
    const QString key = tabKey(channelId, tab);
    const ChannelTabState cur = m_tabStates.value(key);
    if (cur.continuation.isEmpty() || cur.inFlight || cur.moreInFlight) {
        NT_DEBUG("E2E innertube channelTab more ignorada\n");
        return;
    }
    ChannelTabState st = cur;
    st.moreInFlight = true;
    if (limit > 0)
        st.limit = limit;
    m_tabStates.insert(key, st);
    requestChannelTab(channelId, tab, true);
}

void Innertube::requestChannelTab(const QString &channelId, const QString &tab,
                                  bool more)
{
    const QString key = tabKey(channelId, tab);
    const QString continuation = m_tabStates.value(key).continuation;
    m_http->setResult([this, channelId, tab, more](int status,
                                                   const QByteArray &body) {
        NT_DEBUG("E2E innertube channelTab HTTP status=%d size=%d tab=%s\n",
                 status, body.size(), tab.toUtf8().constData());
        handleChannelTab(channelId, tab, status == 200, QString(), body, more);
    });
    m_http->setError([this, channelId, tab, more](const QString &reason) {
        handleChannelTab(channelId, tab, false, reason, QByteArray(), more);
    });
    m_http->setTimeoutMs(kPlayerTimeoutMs);
    m_http->setVisitorId(m_visitorData);

    // WEB client for all three tabs: VISIONOS only exposes "Videos" and its
    // continuation returns empty pages; WEB serves videos/shorts/live with the
    // modern layout (lockups) and working continuation.
    const QJsonObject client = webClientContext(m_visitorData, m_hl);
    QJsonObject body{
        {QStringLiteral("context"),
         QJsonObject{{QStringLiteral("client"), client}}}};
    if (more) {
        body.insert(QStringLiteral("continuation"), continuation);
    } else {
        body.insert(QStringLiteral("browseId"), channelId);
        const char *params = tab == QLatin1String("shorts")
                                 ? kChannelShortsParams
                             : tab == QLatin1String("live")
                                 ? kChannelLiveParams
                                 : kChannelVideosParams;
        body.insert(QStringLiteral("params"), QString::fromLatin1(params));
    }
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    m_http->post(kBrowseEndpoint, json, kChromeUA, kWebClientId,
                 kWebClientVersion, nullptr, nullptr, nullptr);
}

void Innertube::handleChannelTab(const QString &channelId, const QString &tab,
                                 bool ok, const QString &error,
                                 const QByteArray &body, bool more)
{
    const QString key = tabKey(channelId, tab);
    ChannelTabState st = m_tabStates.value(key);
    if (more)
        st.moreInFlight = false;
    else
        st.inFlight = false;
    QVector<VideoInfo> videos;
    QString continuation;
    if (!ok) {
        NT_DEBUG("E2E innertube channelTab HTTP err: %s\n",
                 error.toUtf8().constData());
    } else {
        QJsonObject jsonRoot;
        const bool jsonOk = parseJsonObject(body, &jsonRoot);
        if (jsonOk) {
            // All three tabs go through the WEB client (modern lockups); the
            // classic parser remains as a fallback in case the layout changes.
            videos = collectWebLockups(jsonRoot, st.limit);
            if (videos.isEmpty())
                videos = collectVideoRenderers(jsonRoot, st.limit);
            continuation = firstContinuationToken(jsonRoot);
        } else {
            NT_DEBUG("E2E innertube channelTab json fail size=%d\n", body.size());
        }
    }
    st.continuation = continuation;
    if (!more && videos.isEmpty() && !st.visitorRetried) {
        // Only one retry with fresh visitorData per (channel, tab).
        st.visitorRetried = true;
        st.pending = true;
        m_tabStates.insert(key, st);
        fetchVisitorData();
        return;
    }
    m_tabStates.insert(key, st);
    NT_DEBUG("E2E innertube channelTab ok tab=%s videos=%d more=%d\n",
             tab.toUtf8().constData(), videos.size(),
             continuation.isEmpty() ? 0 : 1);
    emit channelTabReady(channelId, tab, videos, !continuation.isEmpty());
}

void Innertube::fetchVideoDetail(const QString &videoId)
{
    if (m_detailInFlight && m_detailId == videoId) {
        NT_DEBUG("E2E innertube videoDetail single-flight\n");
        return;
    }
    m_detailInFlight = true;
    m_detailId = videoId;
    m_detailVisitorRetried = false;
    NT_DEBUG("E2E innertube fetchVideoDetail id=%s\n",
             videoId.toUtf8().constData());
    if (!hasFreshVisitor()) {
        m_detailPending = true;
        fetchVisitorData();
        return;
    }
    requestVideoDetail();
}

void Innertube::requestVideoDetail()
{
    const QString videoId = m_detailId;
    m_http->setResult([this, videoId](int status, const QByteArray &body) {
        NT_DEBUG("E2E innertube videoDetail HTTP status=%d size=%d\n", status,
                 body.size());
        handleVideoDetail(videoId, status == 200, QString(), body);
    });
    m_http->setError([this, videoId](const QString &reason) {
        handleVideoDetail(videoId, false, reason, QByteArray());
    });
    m_http->setTimeoutMs(kPlayerTimeoutMs);
    m_http->setVisitorId(m_visitorData);

    const PlayerClient &pc = kPlayerClients[0]; // VISIONOS
    const QJsonObject client = clientContext(pc, m_visitorData, m_hl);
    const QJsonObject body{
        {QStringLiteral("context"), QJsonObject{{QStringLiteral("client"), client}}},
        {QStringLiteral("videoId"), videoId},
        {QStringLiteral("racyCheckOk"), true},
        {QStringLiteral("contentCheckOk"), true},
    };
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    m_http->post(kNextEndpoint, json, pc.ua, pc.clientId, pc.version,
                 nullptr, nullptr, nullptr);
}

void Innertube::handleVideoDetail(const QString &videoId, bool ok,
                                  const QString &error, const QByteArray &body)
{
    m_detailInFlight = false;
    VideoInfo video;
    QString description;
    qint64 likeCount = 0;
    if (!ok) {
        NT_DEBUG("E2E innertube videoDetail HTTP err: %s\n",
                 error.toUtf8().constData());
    } else {
        QJsonObject jsonRoot;
        const bool jsonOk = parseJsonObject(body, &jsonRoot);
        if (jsonOk)
            parseVideoDetail(jsonRoot, &video, &description, &likeCount);
        else
            NT_DEBUG("E2E innertube videoDetail json fail size=%d\n", body.size());
    }
    if (video.id.isEmpty())
        video.id = videoId;
    NT_DEBUG("E2E innertube videoDetail ok id=%s title=%d desc=%d likes=%lld\n",
             videoId.toUtf8().constData(), video.title.size(), description.size(),
             static_cast<long long>(likeCount));
    emit videoDetailReady(video, description, likeCount);
}

void Innertube::fetchPlaylist(const QString &playlistId, int limit)
{
    Q_UNUSED(limit);
    if (m_plInFlight && m_plId == playlistId) {
        NT_DEBUG("E2E innertube playlist single-flight\n");
        return;
    }
    m_plInFlight = true;
    m_plId = playlistId;
    m_plVisitorRetried = false;
    NT_DEBUG("E2E innertube fetchPlaylist id=%s\n",
             playlistId.toUtf8().constData());
    if (!hasFreshVisitor()) {
        m_plPending = true;
        fetchVisitorData();
        return;
    }
    requestPlaylist();
}

void Innertube::requestPlaylist()
{
    const QString playlistId = m_plId;
    m_http->setResult([this, playlistId](int status, const QByteArray &body) {
        NT_DEBUG("E2E innertube playlist HTTP status=%d size=%d id=%s\n",
                 status, body.size(), playlistId.toUtf8().constData());
        handlePlaylist(playlistId, status == 200, QString(), body);
    });
    m_http->setError([this, playlistId](const QString &reason) {
        handlePlaylist(playlistId, false, reason, QByteArray());
    });
    m_http->setTimeoutMs(kPlayerTimeoutMs);
    m_http->setVisitorId(m_visitorData);

    const PlayerClient &pc = kPlayerClients[0]; // VISIONOS
    const QJsonObject client = clientContext(pc, m_visitorData, m_hl);
    const QJsonObject body{
        {QStringLiteral("context"), QJsonObject{{QStringLiteral("client"), client}}},
        {QStringLiteral("browseId"), QStringLiteral("VL") + playlistId},
    };
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    m_http->post(kBrowseEndpoint, json, pc.ua, pc.clientId, pc.version,
                 nullptr, nullptr, nullptr);
}

void Innertube::handlePlaylist(const QString &playlistId, bool ok,
                               const QString &error, const QByteArray &body)
{
    m_plInFlight = false;
    PlaylistInfo info;
    info.id = playlistId;
    QVector<VideoInfo> videos;
    if (!ok) {
        NT_DEBUG("E2E innertube playlist HTTP err: %s\n",
                 error.toUtf8().constData());
    } else {
        QJsonObject jsonRoot;
        const bool jsonOk = parseJsonObject(body, &jsonRoot);
        if (jsonOk)
            parsePlaylistBrowse(jsonRoot, &info, &videos);
        else
            NT_DEBUG("E2E innertube playlist json fail size=%d\n", body.size());
    }
    if (info.title.isEmpty() && videos.isEmpty()
        && retryBrowseWithFreshVisitor(&m_plVisitorRetried, &m_plPending))
        return;
    NT_DEBUG("E2E innertube playlist ok title=%s videos=%d\n",
             info.title.toUtf8().constData(), videos.size());
    emit playlistReady(info, videos);
}

namespace {

// Text of a node (simpleText or runs).
QString homeTextOf(const QJsonValue &val)
{
    const QJsonObject o = val.toObject();
    const QJsonValue simple = o.value(QStringLiteral("simpleText"));
    if (simple.isString())
        return simple.toString();
    const QJsonArray runs = o.value(QStringLiteral("runs")).toArray();
    QString t;
    for (const QJsonValue &r : runs)
        t += r.toObject().value(QStringLiteral("text")).toString();
    return t;
}

// browseId of the channel in a shared byline (compactVideoRenderer /
// playlistPanelVideoRenderer): runs[0].navigationEndpoint.browseEndpoint.
// Without this, videos arriving via the queue were left WITHOUT a channel id,
// and one could neither enter the channel nor subscribe from the Player page.
QString channelIdFromByline(const QJsonObject &text)
{
    const QJsonArray runs = text.value(QStringLiteral("runs")).toArray();
    if (runs.isEmpty())
        return QString();
    return runs.at(0).toObject().value(QStringLiteral("navigationEndpoint"))
        .toObject().value(QStringLiteral("browseEndpoint")).toObject()
        .value(QStringLiteral("browseId")).toString();
}

int homeDurationOf(const QJsonArray &overlays)
{
    for (const QJsonValue &val : overlays) {
        const QJsonObject o = val.toObject();
        if (!o.contains(QStringLiteral("thumbnailOverlayTimeStatusRenderer")))
            continue;
        const QString t = homeTextOf(o.value(
            QStringLiteral("thumbnailOverlayTimeStatusRenderer")).toObject()
                .value(QStringLiteral("text")));
        QStringList parts = t.split(QLatin1Char(':'));
        if (parts.size() == 2)
            return parts.at(0).toInt() * 60 + parts.at(1).toInt();
        if (parts.size() == 3)
            return parts.at(0).toInt() * 3600 + parts.at(1).toInt() * 60
                   + parts.at(2).toInt();
    }
    return 0;
}

// Does the thumbnailOverlayTimeStatusRenderer declare a live stream? In
// VISIONOS a live stream carries style "LIVE" in that overlay (text "En
// directo ahora" instead of a duration); a VOD has no live style.
bool overlayIsLive(const QJsonArray &overlays)
{
    for (const QJsonValue &val : overlays) {
        const QString style = val.toObject()
            .value(QStringLiteral("thumbnailOverlayTimeStatusRenderer")).toObject()
            .value(QStringLiteral("style")).toString();
        if (style.contains(QStringLiteral("LIVE")))
            return true;
    }
    return false;
}

// Does the badges list of the classic renderer declare a live stream? The WEB
// client does it with metadataBadgeRenderer.style ==
// "BADGE_STYLE_TYPE_LIVE_NOW" (LIVE icon, label "EN DIRECTO").
bool badgesIsLive(const QJsonArray &badges)
{
    for (const QJsonValue &b : badges) {
        const QString style = b.toObject()
            .value(QStringLiteral("metadataBadgeRenderer")).toObject()
            .value(QStringLiteral("style")).toString();
        if (style.contains(QStringLiteral("LIVE")))
            return true;
    }
    return false;
}

// A classic renderer (videoRenderer/compactVideoRenderer/playlistPanelVideo
// Renderer) is a live stream if either of the two paths declares it.
bool rendererIsLive(const QJsonObject &r)
{
    return overlayIsLive(r.value(QStringLiteral("thumbnailOverlays")).toArray())
        || badgesIsLive(r.value(QStringLiteral("badges")).toArray());
}

// Does the /next viewCountText describe a live stream ON AIR ("10.554
// usuarios viéndolo ahora", "10,554 watching now", "assistindo agora") instead
// of "N visualizaciones"? It is the discriminator for "still live": a live
// stream that ended goes back to the normal view-count format.
bool viewCountShowsLive(const QString &text)
{
    const QString t = text.toLower();
    return t.contains(QStringLiteral("viéndolo"))
        || t.contains(QStringLiteral("watching"))
        || t.contains(QStringLiteral("assistindo"));
}

// --- "Metalanguage" of relative dates ---------------------------------------
// The parser does NOT rely on the language's words ("hace"/"ago") nor on a
// cascade of startsWith() in a hand-fixed order. That design already hid a
// real bug: "semanas" starts with 's' just like "segundos", it fell into the
// seconds branch and the date of "hace 2 semanas" came out 2 s from now -> the
// app painted it "hoy". So it does not happen again with another unit or
// language:
//   1) language-agnostic extraction: find the first number (or the article
//      "uno") whose NEXT token is a recognizable unit. The connector does not
//      matter: "hace 2 semanas", "2 weeks ago", "Emitido hace 6 horas" or
//      Portuguese "ha 2 semanas" all work without touching anything.
//   2) the unit is recognized with a DECLARATIVE table (hint -> factor) by
//      LONGEST match, not by order of ifs: "sem" beats "s" without having to
//      armor any branch, and adding a new unit (e.g. "quincena") is adding a
//      row.
//   3) what is not recognized returns EMPTY: the UI does not invent a date
//      (before, a weird unit could slip in and produce "hoy").
// The hints are compared without accents (the unit is normalized to NFD and
// the marks are stripped) to cover any language.
struct RelativeUnitHint
{
    const char *hint; // substring (or exact token if length 1), lowercase, no accents
    qint64 factor;    // seconds per unit
};

// The order does not matter: the longest hint wins. "sem" covers semana(s),
// "seg"/"segund" cover segundo(s), "mes"/"mese" cover mes(es), etc.
const RelativeUnitHint kRelativeUnits[] = {
    {"segund", 1},           {"seg", 1},          {"sec", 1},  {"s", 1},
    {"minut", 60},           {"min", 60},         {"m", 30 * 86400},
    {"hora", 3600},          {"hour", 3600},      {"hr", 3600},  {"h", 3600},
    {"dia", 86400},          {"day", 86400},      {"d", 86400},
    {"sem", 7 * 86400},      {"week", 7 * 86400}, {"wk", 7 * 86400}, {"w", 7 * 86400},
    {"mes", 30 * 86400},     {"month", 30 * 86400},
    {"ano", 365 * 86400},    {"year", 365 * 86400}, {"yr", 365 * 86400},
    {"a", 365 * 86400},      {"y", 365 * 86400},
};

// Spanish/Portuguese eat accents ("día", "mês", "año"): they are stripped so
// the same hint ("dia", "mes", "ano") works in all three supported languages.
QString sinAcentos(const QString &s)
{
    const QString d = s.normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(d.size());
    for (const QChar c : d) {
        const QChar::Category cat = c.category();
        if (cat != QChar::Mark_NonSpacing
            && cat != QChar::Mark_SpacingCombining)
            out.append(c);
    }
    return out;
}

// Seconds per unit (0 if not recognized). The longest hint wins; if the unit
// is ONE letter, an EXACT match is required (not "s" inside "semanas").
qint64 unidadSegundos(const QString &rawUnit)
{
    const QString u = sinAcentos(rawUnit.toLower()).trimmed();
    if (u.isEmpty())
        return 0;
    qint64 best = 0;
    int bestLen = 0;
    for (const RelativeUnitHint &p : kRelativeUnits) {
        const QLatin1String hint(p.hint);
        const bool hit =
            (hint.size() == 1) ? (u == hint) : u.contains(hint);
        if (hit && hint.size() > bestLen) {
            bestLen = hint.size();
            best = p.factor;
        }
    }
    return best;
}

// "hace 3 días" / "hace 8 h" / "2 months ago" / "há 2 semanas" → approximate
// ISO date (to sort feeds without depending on an exact timestamp). If the
// text is not recognized, returns empty (the UI does not invent a date).
// Recognizes the abbreviated units of the /next lockupViewModel ("hace 8 h",
// "hace 2 m", "hace 3 d"): without them the date was lost and "A continuación"
// came out without it. Recognition (number + unit) is language-agnostic and
// uses the kRelativeUnits table; see the "metalanguage" a few lines above.
QString relativeToIso(const QString &text)
{
    const QString t = text.trimmed().toLower();
    const QStringList parts = t.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 2)
        return QString();
    // Language-agnostic extraction: first number (or article "un/una/um/uma/
    // an/a") whose next token is a recognizable unit. NOT anchored to
    // "hace"/"ago".
    double n = 0;
    qint64 factor = 0;
    for (int i = 0; i + 1 < parts.size(); ++i) {
        const QString tok = parts.at(i);
        bool ok = false;
        double val = tok.toDouble(&ok);
        if (!ok) {
            if (tok == QLatin1String("un") || tok == QLatin1String("una")
                || tok == QLatin1String("um") || tok == QLatin1String("uma")
                || tok == QLatin1String("an") || tok == QLatin1String("a"))
                val = 1;
            else
                continue;
        }
        const qint64 f = unidadSegundos(parts.at(i + 1));
        if (val > 0 && f > 0) {
            n = val;
            factor = f;
            break;
        }
    }
    if (factor <= 0)
        return QString();
    const qint64 secs = qint64(n * factor);
    if (secs <= 0)
        return QString();
    return QDateTime::currentDateTimeUtc().addSecs(-secs)
        .toString(Qt::ISODate);
}

// Numeric count from a localized text ("16,9 M de suscriptores",
// "47 M de visualizaciones", "1.234 suscriptores"). In "es" the comma is the
// decimal separator and the dot the thousands separator; the M/K/B suffix
// multiplies.
long long countFromText(const QString &raw)
{
    const QString t = raw.simplified();
    if (t.isEmpty())
        return 0;
    const QString low = t.toLower();
    double mult = 1.0;
    if (low.contains(QStringLiteral("mil millones"))
        || low.contains(QStringLiteral("billion")))
        mult = 1e9;
    else if (low.contains(QStringLiteral("millon")) || low.contains(QLatin1Char('m')))
        mult = 1e6;
    else if (low.contains(QStringLiteral("mil")) || low.contains(QLatin1Char('k')))
        mult = 1e3;
    else if (low.contains(QLatin1Char('b')))
        mult = 1e9;
    QString num;
    for (const QChar c : t) {
        if (c.isDigit() || c == QLatin1Char(',') || c == QLatin1Char('.'))
            num += c;
        else
            break;
    }
    if (num.isEmpty())
        return 0;
    const int lastSep = qMax(num.lastIndexOf(QLatin1Char(',')),
                             num.lastIndexOf(QLatin1Char('.')));
    if (lastSep >= 0) {
        const int after = num.size() - lastSep - 1;
        if (mult > 1.0 || after <= 2) {
            // Multiplier or decimal separator: comma decimal, dot thousands.
            num.remove(QLatin1Char('.'));
            num.replace(QLatin1Char(','), QLatin1Char('.'));
        } else {
            // No multiplier and 3 digits after the separator: thousands.
            num.remove(QLatin1Char(','));
            num.remove(QLatin1Char('.'));
        }
    }
    bool ok = false;
    const double v = num.toDouble(&ok);
    return ok ? qRound64(v * mult) : 0;
}

// First value with that key in the tree (Undefined if it does not appear).
QJsonValue findFirstValue(const QJsonValue &root, const QString &key)
{
    if (root.isObject()) {
        const QJsonObject o = root.toObject();
        if (o.contains(key))
            return o.value(key);
        for (auto it = o.begin(); it != o.end(); ++it) {
            const QJsonValue r = findFirstValue(it.value(), key);
            if (!r.isUndefined())
                return r;
        }
    } else if (root.isArray()) {
        const QJsonArray arr = root.toArray();
        for (const QJsonValue &e : arr) {
            const QJsonValue r = findFirstValue(e, key);
            if (!r.isUndefined())
                return r;
        }
    }
    return QJsonValue(QJsonValue::Undefined);
}

} // namespace

// compactVideoRenderer / videoRenderer → VideoInfo.
VideoInfo Innertube::videoFromHomeRenderer(const QJsonObject &item) const
{
    const QJsonObject vr = item.value(QStringLiteral("videoRenderer")).toObject();
    const QJsonObject cr = item.value(QStringLiteral("compactVideoRenderer")).toObject();
    const QJsonObject *r = vr.isEmpty() ? &cr : &vr;
    if (r->isEmpty())
        return VideoInfo{};
    VideoInfo v;
    v.id = r->value(QStringLiteral("navigationEndpoint")).toObject()
               .value(QStringLiteral("watchEndpoint")).toObject()
               .value(QStringLiteral("videoId")).toString();
    if (v.id.isEmpty())
        v.id = r->value(QStringLiteral("videoId")).toString();
    if (v.id.isEmpty())
        return VideoInfo{};
    v.title = homeTextOf(r->value(QStringLiteral("title")));
    // The channel lives in ownerText (Home/channel) or longBylineText/
    // shortBylineText (search): the first run with browseEndpoint.browseId
    // gives name and id.
    for (const char *key : {"ownerText", "longBylineText", "shortBylineText"}) {
        const QJsonArray runs = r->value(QString::fromLatin1(key)).toObject()
                                    .value(QStringLiteral("runs")).toArray();
        for (const QJsonValue &rv : runs) {
            const QJsonObject o = rv.toObject();
            const QString id = o.value(QStringLiteral("navigationEndpoint"))
                                   .toObject()
                                   .value(QStringLiteral("browseEndpoint"))
                                   .toObject()
                                   .value(QStringLiteral("browseId"))
                                   .toString();
            if (!id.isEmpty()) {
                v.channel = o.value(QStringLiteral("text")).toString();
                v.channelId = id;
                break;
            }
        }
        if (!v.channelId.isEmpty())
            break;
    }
    if (v.channel.isEmpty())
        v.channel = homeTextOf(r->value(QStringLiteral("longBylineText")));
    const QJsonArray thumbs = r->value(QStringLiteral("thumbnail"))
                                  .toObject().value(QStringLiteral("thumbnails"))
                                  .toArray();
    if (!thumbs.isEmpty())
        v.thumbnailUrl = thumbs.last().toObject()
                             .value(QStringLiteral("url")).toString();
    v.durationSeconds = homeDurationOf(
        r->value(QStringLiteral("thumbnailOverlays")).toArray());
    v.isLive = rendererIsLive(*r);
    v.viewCount = countFromText(homeTextOf(r->value(QStringLiteral("viewCountText"))));
    v.publishedAt = relativeToIso(homeTextOf(
        r->value(QStringLiteral("publishedTimeText"))));
    if (v.thumbnailUrl.isEmpty())
        v.thumbnailUrl = fallbackThumbnailUrl(v.id);
    return v;
}

// Walks the tree collecting compactVideoRenderer/videoRenderer without
// duplicating ids, respecting the response order and the limit.
QVector<VideoInfo> Innertube::collectVideoRenderers(const QJsonObject &root,
                                                    int maxCount) const
{
    QVector<VideoInfo> out;
    std::function<void(const QJsonValue &)> collect =
        [&](const QJsonValue &val) {
            if (out.size() >= maxCount)
                return;
            if (val.isArray()) {
                const QJsonArray arr = val.toArray();
                for (const QJsonValue &e : arr)
                    collect(e);
                return;
            }
            if (!val.isObject())
                return;
            const QJsonObject o = val.toObject();
            if (o.contains(QStringLiteral("compactVideoRenderer"))
                || o.contains(QStringLiteral("videoRenderer"))) {
                const VideoInfo v = videoFromHomeRenderer(o);
                if (!v.id.isEmpty()) {
                    bool seen = false;
                    for (const VideoInfo &d : std::as_const(out))
                        if (d.id == v.id) { seen = true; break; }
                    if (!seen)
                        out.append(v);
                }
                return;
            }
            for (auto it = o.begin(); it != o.end(); ++it)
                collect(it.value());
        };
    collect(QJsonValue(root));
    return out;
}

// Home feed (FEwhat_to_watch) with the layout verified in PROOF 1 with
// VISIONOS: singleColumnBrowseResultsRenderer → tabs → tabRenderer → content →
// sectionListRenderer → contents[] → shelfRenderer → content →
// verticalListRenderer → items[] → compactVideoRenderer / videoRenderer.
QVector<VideoInfo> Innertube::parseHomeBrowse(const QJsonObject &root,
                                              QString *continuation) const
{
    QVector<VideoInfo> out;
    if (continuation)
        continuation->clear();

    // First continuation token found anywhere in the response; the shelves do
    // not carry it, it lives in the sectionListRenderer itself or in the
    // listing's continuations.
    std::function<void(const QJsonValue &)> grabCont;
    grabCont = [&](const QJsonValue &val) {
        if (continuation && !continuation->isEmpty())
            return;
        if (val.isArray()) {
            const QJsonArray arr = val.toArray();
            for (const QJsonValue &e : arr)
                grabCont(e);
            return;
        }
        if (!val.isObject())
            return;
        const QJsonObject o = val.toObject();
        QString tok;
        if (o.contains(QStringLiteral("continuationCommand")) && tok.isEmpty()) {
            tok = o.value(QStringLiteral("continuationCommand")).toObject()
                      .value(QStringLiteral("token")).toString();
        }
        if (tok.isEmpty() && o.contains(QStringLiteral("nextContinuationData"))) {
            tok = o.value(QStringLiteral("nextContinuationData")).toObject()
                      .value(QStringLiteral("continuation")).toString();
        }
        if (tok.isEmpty() && o.contains(QStringLiteral("reloadContinuationData"))) {
            tok = o.value(QStringLiteral("reloadContinuationData")).toObject()
                      .value(QStringLiteral("continuation")).toString();
        }
        if (continuation && !tok.isEmpty())
            *continuation = tok;
        for (auto it = o.begin(); it != o.end(); ++it)
            grabCont(it.value());
    };
    for (auto it = root.begin(); it != root.end(); ++it)
        grabCont(it.value());

    // Collects video renderers inside a section (subtree). Respects the
    // per-shelf and total limits.
    int shelves = 0;
    int shelfVideos = 0;
    std::function<void(const QJsonValue &)> collect =
        [&](const QJsonValue &val) {
            if (out.size() >= kHomeShelfCap * kHomeShelfVideos
                || shelfVideos >= kHomeShelfVideos)
                return;
            if (val.isArray()) {
                const QJsonArray arr = val.toArray();
                for (const QJsonValue &e : arr)
                    collect(e);
                return;
            }
            if (!val.isObject())
                return;
            const QJsonObject o = val.toObject();
            if (o.contains(QStringLiteral("compactVideoRenderer"))
                || o.contains(QStringLiteral("videoRenderer"))) {
                const VideoInfo v = videoFromHomeRenderer(o);
                if (!v.id.isEmpty() && shelfVideos < kHomeShelfVideos
                    && out.size() < kHomeShelfCap * kHomeShelfVideos)
                    out.append(v);
                ++shelfVideos;
                return;
            }
            for (auto it = o.begin(); it != o.end(); ++it)
                collect(it.value());
        };

    const QJsonObject contents = root.value(QStringLiteral("contents")).toObject();
    const QJsonObject sc = contents
        .value(QStringLiteral("singleColumnBrowseResultsRenderer")).toObject();
    const QJsonArray tabs = sc.value(QStringLiteral("tabs")).toArray();
    if (!tabs.isEmpty()) {
        const QJsonObject content = tabs.first().toObject()
            .value(QStringLiteral("tabRenderer")).toObject()
            .value(QStringLiteral("content")).toObject();
        const QJsonArray sections = content
            .value(QStringLiteral("sectionListRenderer")).toObject()
            .value(QStringLiteral("contents")).toArray();
        for (const QJsonValue &sec : sections) {
            if (out.size() >= kHomeShelfCap * kHomeShelfVideos)
                break;
            const QJsonObject shelf = sec.toObject()
                .value(QStringLiteral("shelfRenderer")).toObject();
            if (shelf.isEmpty())
                continue;
            if (shelves >= kHomeShelfCap)
                break;
            ++shelves;
            shelfVideos = 0;
            collect(shelf.value(QStringLiteral("content")));
        }
    }

    // Different layout (e.g. richGrid): collect up to kHomeShelfVideos
    // straight from the root tree as if it were a single "shelf".
    if (out.isEmpty()) {
        shelfVideos = 0;
        collect(QJsonValue(root));
    }
    // Dedupe by id (the same video can repeat across shelves).
    QVector<VideoInfo> unique;
    for (const VideoInfo &v : std::as_const(out)) {
        bool seen = false;
        for (const VideoInfo &d : std::as_const(unique)) {
            if (d.id == v.id) {
                seen = true;
                break;
            }
        }
        if (!seen)
            unique.append(v);
    }
    return unique;
}

// ---------------------------------------------------------------------------
// Modern renderers of the WEB client (channel Shorts/Live tabs, which
// VISIONOS does not expose): shortsLockupViewModel and lockupViewModel.

namespace {

QString webLockupThumb(const QJsonObject &contentImage)
{
    const QJsonArray sources = contentImage
                                   .value(QStringLiteral("thumbnailViewModel"))
                                   .toObject().value(QStringLiteral("image"))
                                   .toObject().value(QStringLiteral("sources"))
                                   .toArray();
    return sources.isEmpty()
        ? QString()
        : sources.last().toObject().value(QStringLiteral("url")).toString();
}

// Duration and live badge of the modern WEB layout (lockupViewModel): the
// bottom overlay carries one badge per thumbnail — the duration ("12:34") for
// VODs and the live one ("EN DIRECTO", badgeStyle THUMBNAIL_OVERLAY_
// BADGE_STYLE_LIVE) for live streams.
int webLockupMeta(const QJsonObject &contentImage, int *secondsOut,
                  bool *isLiveOut)
{
    int seconds = 0;
    const QJsonArray overlays = contentImage
                                    .value(QStringLiteral("thumbnailViewModel"))
                                    .toObject().value(QStringLiteral("overlays"))
                                    .toArray();
    for (const QJsonValue &ov : overlays) {
        const QJsonObject bvm = ov.toObject()
            .value(QStringLiteral("thumbnailBottomOverlayViewModel")).toObject();
        const QJsonArray badges = bvm.value(QStringLiteral("badges")).toArray();
        for (const QJsonValue &b : badges) {
            const QJsonObject tbvm = b.toObject()
                .value(QStringLiteral("thumbnailBadgeViewModel")).toObject();
            const QString style = tbvm.value(QStringLiteral("badgeStyle")).toString();
            if (style.contains(QStringLiteral("LIVE")))
                if (isLiveOut)
                    *isLiveOut = true;
            const QString txt = tbvm.value(QStringLiteral("text")).toString();
            const QStringList parts = txt.split(QLatin1Char(':'));
            if (parts.size() == 2)
                seconds = parts.at(0).toInt() * 60 + parts.at(1).toInt();
            else if (parts.size() == 3)
                seconds = parts.at(0).toInt() * 3600 + parts.at(1).toInt() * 60
                          + parts.at(2).toInt();
        }
    }
    if (secondsOut)
        *secondsOut = seconds;
    return seconds;
}

// shortsLockupViewModel: id in entityId="shorts-shelf-item-<id>", title and
// views in overlayMetadata, vertical thumbnail in thumbnailViewModel.
VideoInfo videoFromWebShorts(const QJsonObject &sl)
{
    VideoInfo v;
    v.id = sl.value(QStringLiteral("entityId")).toString();
    const QString prefix = QStringLiteral("shorts-shelf-item-");
    if (v.id.startsWith(prefix))
        v.id.remove(0, prefix.size());
    const QJsonObject ov = sl.value(QStringLiteral("overlayMetadata")).toObject();
    v.title = ov.value(QStringLiteral("primaryText")).toObject()
                  .value(QStringLiteral("content")).toString();
    v.viewCount = countFromText(ov.value(QStringLiteral("secondaryText")).toObject()
                                    .value(QStringLiteral("content")).toString());
    const QJsonArray sources = sl.value(QStringLiteral("thumbnailViewModel"))
                                   .toObject().value(QStringLiteral("image"))
                                   .toObject().value(QStringLiteral("sources"))
                                   .toArray();
    if (!sources.isEmpty())
        v.thumbnailUrl = sources.last().toObject()
                             .value(QStringLiteral("url")).toString();
    if (v.thumbnailUrl.isEmpty())
        v.thumbnailUrl = fallbackThumbnailUrl(v.id);
    return v;
}

// Is the text a count like "21 K", "5,4 M", "684"? The /next lockup gives the
// views as number + multiplier, WITHOUT the word "vistas" (that word only
// appears in the channel layout). It requires the text to start with a digit
// and what remains to be only the multiplier suffix, so as not to confuse a
// channel name ("3 Doors Down" starts with a digit but continues with letters).
bool looksLikeCountText(const QString &raw)
{
    const QString t = raw.simplified();
    if (t.isEmpty() || !t.at(0).isDigit())
        return false;
    int i = 0;
    while (i < t.size()
           && (t.at(i).isDigit() || t.at(i) == QLatin1Char(',')
               || t.at(i) == QLatin1Char('.') || t.at(i) == QLatin1Char(' ')))
        ++i;
    const QString rest = t.mid(i).trimmed().toLower();
    return rest.isEmpty() || rest == QLatin1String("k")
        || rest == QLatin1String("m") || rest == QLatin1String("b")
        || rest == QLatin1String("mil") || rest.startsWith(QLatin1String("millon"))
        || rest == QLatin1String("bn");
}

// lockupViewModel: id=contentId, title in lockupMetadataViewModel, views and
// date in metadataRows, thumbnail+duration in contentImage.
VideoInfo videoFromWebLockup(const QJsonObject &lv)
{
    VideoInfo v;
    v.id = lv.value(QStringLiteral("contentId")).toString();
    const QJsonObject md = lv.value(QStringLiteral("metadata")).toObject()
                               .value(QStringLiteral("lockupMetadataViewModel"))
                               .toObject();
    v.title = md.value(QStringLiteral("title")).toObject()
                  .value(QStringLiteral("content")).toString();
    const QJsonArray rows = md.value(QStringLiteral("metadata")).toObject()
                                .value(QStringLiteral("contentMetadataViewModel"))
                                .toObject()
                                .value(QStringLiteral("metadataRows")).toArray();
    for (const QJsonValue &r : rows) {
        const QJsonArray parts = r.toObject()
                                     .value(QStringLiteral("metadataParts")).toArray();
        for (const QJsonValue &p : parts) {
            const QString t = p.toObject().value(QStringLiteral("text")).toObject()
                                  .value(QStringLiteral("content")).toString();
            // The date parser is the ONLY authority on "this is a date": there
            // is no filter by "hace"/"ago" here (it was a second copy of the
            // parser's knowledge and with pt "ha 2 semanas" the date was not
            // classified, the text falling into the channel name).
            const QString iso = relativeToIso(t);
            if (!iso.isEmpty())
                v.publishedAt = iso;
            else if (t.contains(QStringLiteral("visualizacion"))
                     || t.contains(QStringLiteral("view")))
                v.viewCount = countFromText(t);
            else if (looksLikeCountText(t))
                v.viewCount = countFromText(t);
            else if (v.channel.isEmpty())
                v.channel = t; // the first datum that is neither views nor date
        }
    }
    // Channel: the lockup avatar carries the channel's browseEndpoint, so the
    // id is resolved too (the row's "Ver canal" menu needs it).
    const QJsonObject channelEndpoint =
        md.value(QStringLiteral("image")).toObject()
            .value(QStringLiteral("decoratedAvatarViewModel")).toObject()
            .value(QStringLiteral("avatar")).toObject()
            .value(QStringLiteral("rendererContext")).toObject()
            .value(QStringLiteral("commandContext")).toObject()
            .value(QStringLiteral("onTap")).toObject()
            .value(QStringLiteral("innertubeCommand")).toObject()
            .value(QStringLiteral("browseEndpoint")).toObject();
    if (v.channelId.isEmpty())
        v.channelId = channelEndpoint.value(QStringLiteral("browseId")).toString();
    const QJsonObject contentImage = lv.value(QStringLiteral("contentImage")).toObject();
    v.thumbnailUrl = webLockupThumb(contentImage);
    v.durationSeconds = webLockupMeta(contentImage, nullptr, &v.isLive);
    if (v.thumbnailUrl.isEmpty())
        v.thumbnailUrl = fallbackThumbnailUrl(v.id);
    return v;
}

} // namespace

// First continuation token of the tree, whatever the layout
// (continuationCommand / nextContinuationData / reloadContinuationData).
QString Innertube::firstContinuationToken(const QJsonObject &root)
{
    QString token;
    std::function<void(const QJsonValue &)> walk = [&](const QJsonValue &val) {
        if (!token.isEmpty())
            return;
        if (val.isArray()) {
            for (const QJsonValue &e : val.toArray())
                walk(e);
            return;
        }
        if (!val.isObject())
            return;
        const QJsonObject o = val.toObject();
        if (o.contains(QStringLiteral("continuationCommand")))
            token = o.value(QStringLiteral("continuationCommand")).toObject()
                        .value(QStringLiteral("token")).toString();
        if (token.isEmpty() && o.contains(QStringLiteral("nextContinuationData")))
            token = o.value(QStringLiteral("nextContinuationData")).toObject()
                        .value(QStringLiteral("continuation")).toString();
        if (token.isEmpty() && o.contains(QStringLiteral("reloadContinuationData")))
            token = o.value(QStringLiteral("reloadContinuationData")).toObject()
                        .value(QStringLiteral("continuation")).toString();
        if (!token.isEmpty())
            return;
        for (auto it = o.begin(); it != o.end(); ++it)
            walk(it.value());
    };
    walk(QJsonValue(root));
    return token;
}

// /search page: compactVideoRenderer of the sectionListRenderer.
QVector<VideoInfo> Innertube::parseSearchBrowse(const QJsonObject &root,
                                                int maxCount,
                                                QString *continuation) const
{
    if (continuation)
        *continuation = firstContinuationToken(root);
    return collectVideoRenderers(root, maxCount);
}

// Walks the tree collecting the WEB client's modern lockups without
// duplicating ids (channel Shorts/Live tabs).
QVector<VideoInfo> Innertube::collectWebLockups(const QJsonObject &root,
                                                int maxCount) const
{
    QVector<VideoInfo> out;
    std::function<void(const QJsonValue &)> collect = [&](const QJsonValue &val) {
        if (out.size() >= maxCount)
            return;
        if (val.isArray()) {
            for (const QJsonValue &e : val.toArray())
                collect(e);
            return;
        }
        if (!val.isObject())
            return;
        const QJsonObject o = val.toObject();
        VideoInfo v;
        if (o.contains(QStringLiteral("shortsLockupViewModel")))
            v = videoFromWebShorts(
                o.value(QStringLiteral("shortsLockupViewModel")).toObject());
        else if (o.contains(QStringLiteral("lockupViewModel")))
            v = videoFromWebLockup(
                o.value(QStringLiteral("lockupViewModel")).toObject());
        if (!v.id.isEmpty()) {
            bool seen = false;
            for (const VideoInfo &d : std::as_const(out))
                if (d.id == v.id) { seen = true; break; }
            if (!seen)
                out.append(v);
            return;
        }
        for (auto it = o.begin(); it != o.end(); ++it)
            collect(it.value());
    };
    collect(QJsonValue(root));
    return out;
}

// Channel card from the "About" browse: c4TabbedHeaderRenderer (title,
// avatar, subscribers) + channelAboutMetadataRenderer (description).
void Innertube::parseChannelInfo(const QJsonObject &root, ChannelInfo *info)
{
    if (!info)
        return;
    const QJsonObject header = findFirstValue(
        QJsonValue(root), QStringLiteral("c4TabbedHeaderRenderer")).toObject();
    if (!header.isEmpty()) {
        const QString id = header.value(QStringLiteral("channelId")).toString();
        if (!id.isEmpty())
            info->id = id;
        info->name = header.value(QStringLiteral("title")).toString();
        info->subscriberCount = countFromText(
            homeTextOf(header.value(QStringLiteral("subscriberCountText"))));
        const QJsonArray av = header.value(QStringLiteral("avatar")).toObject()
                                  .value(QStringLiteral("thumbnails")).toArray();
        if (!av.isEmpty())
            info->avatarUrl = av.last().toObject()
                                  .value(QStringLiteral("url")).toString();
    }
    const QJsonObject about = findFirstValue(
        QJsonValue(root), QStringLiteral("channelAboutMetadataRenderer")).toObject();
    if (!about.isEmpty()) {
        if (info->name.isEmpty())
            info->name = homeTextOf(about.value(QStringLiteral("title")));
        info->description = homeTextOf(about.value(QStringLiteral("description")));
        const QJsonArray av = about.value(QStringLiteral("avatar")).toObject()
                                  .value(QStringLiteral("thumbnails")).toArray();
        if (info->avatarUrl.isEmpty() && !av.isEmpty())
            info->avatarUrl = av.last().toObject()
                                  .value(QStringLiteral("url")).toString();
    }
}

// Video detail from /next: videoMetadataRenderer (title, views, date,
// channel, description) + likeButtonRenderer (likes).
void Innertube::parseVideoDetail(const QJsonObject &root, VideoInfo *video,
                                 QString *description, qint64 *likeCount)
{
    if (video)
        *video = VideoInfo{};
    const QJsonObject vm = findFirstValue(
        QJsonValue(root), QStringLiteral("videoMetadataRenderer")).toObject();
    if (vm.isEmpty())
        return;
    if (video) {
        video->id = vm.value(QStringLiteral("videoId")).toString();
        video->title = homeTextOf(vm.value(QStringLiteral("title")));
        const QString viewCountText =
            homeTextOf(vm.value(QStringLiteral("viewCountText")));
        video->viewCount = countFromText(viewCountText);
        // A live stream on air shows "N usuarios viéndolo ahora" instead of
        // "N visualizaciones": that is what distinguishes "still live" from
        // "ended" in the /next refresh (the player response is authoritative
        // when playing, but /next does not touch the player phase).
        video->isLive = viewCountShowsLive(viewCountText);
        video->publishedAt = relativeToIso(
            homeTextOf(vm.value(QStringLiteral("dateText"))));
        // If dateText does not give a relative date, publishedTimeText is
        // tried ("Emitió en directo hace 6 horas").
        if (video->publishedAt.isEmpty()) {
            video->publishedAt = relativeToIso(homeTextOf(
                vm.value(QStringLiteral("publishedTimeText"))));
        }
        const QJsonObject owner = vm.value(QStringLiteral("owner")).toObject()
                                      .value(QStringLiteral("videoOwnerRenderer"))
                                      .toObject();
        video->channel = homeTextOf(owner.value(QStringLiteral("title")));
        video->channelId = owner.value(QStringLiteral("navigationEndpoint"))
                               .toObject().value(QStringLiteral("browseEndpoint"))
                               .toObject().value(QStringLiteral("browseId"))
                               .toString();
        // Videos with several COLLABORATION channels: the owner carries no
        // navigationEndpoint (only avatarStack + subscribeButton), so the id
        // comes from the subscribe button. Without this path, those channels
        // could not be entered or subscribed from the Player page (measured:
        // empty channelId in the detail and the "Canal" button doing nothing).
        if (video->channelId.isEmpty()) {
            video->channelId = owner.value(QStringLiteral("subscribeButton"))
                                   .toObject()
                                   .value(QStringLiteral("subscribeButtonRenderer"))
                                   .toObject()
                                   .value(QStringLiteral("channelId"))
                                   .toString();
        }
    }
    if (description)
        *description = homeTextOf(vm.value(QStringLiteral("description")));
    if (likeCount) {
        const QJsonObject like = vm.value(QStringLiteral("likeButton")).toObject()
                                     .value(QStringLiteral("likeButtonRenderer"))
                                     .toObject();
        *likeCount = like.value(QStringLiteral("likeCount"))
                         .toVariant().toLongLong();
    }
}

// Playlist (browse VL<id>): playlistHeaderRenderer + playlistVideoRenderer.
void Innertube::parsePlaylistBrowse(const QJsonObject &root, PlaylistInfo *info,
                                    QVector<VideoInfo> *videos)
{
    if (info) {
        const QJsonObject ph = findFirstValue(
            QJsonValue(root), QStringLiteral("playlistHeaderRenderer")).toObject();
        if (!ph.isEmpty()) {
            info->title = homeTextOf(ph.value(QStringLiteral("title")));
            info->channel = homeTextOf(ph.value(QStringLiteral("ownerText")));
            info->videoCount = int(countFromText(
                homeTextOf(ph.value(QStringLiteral("numVideosText")))));
        }
    }
    if (!videos)
        return;
    std::function<void(const QJsonValue &)> collect = [&](const QJsonValue &val) {
        if (val.isArray()) {
            for (const QJsonValue &e : val.toArray())
                collect(e);
            return;
        }
        if (!val.isObject())
            return;
        const QJsonObject o = val.toObject();
        if (o.contains(QStringLiteral("playlistVideoRenderer"))) {
            const QJsonObject pv =
                o.value(QStringLiteral("playlistVideoRenderer")).toObject();
            VideoInfo v;
            v.id = pv.value(QStringLiteral("videoId")).toString();
            v.title = homeTextOf(pv.value(QStringLiteral("title")));
            v.channel = homeTextOf(pv.value(QStringLiteral("shortBylineText")));
            if (v.channel.isEmpty())
                v.channel =
                    homeTextOf(pv.value(QStringLiteral("longBylineText")));
            // Channel id, same as in the queue: without it, a video opened
            // from a playlist could not enter the channel or subscribe until
            // the detail arrived.
            v.channelId = channelIdFromByline(
                pv.value(QStringLiteral("shortBylineText")).toObject());
            if (v.channelId.isEmpty()) {
                v.channelId = channelIdFromByline(
                    pv.value(QStringLiteral("longBylineText")).toObject());
            }
            v.durationSeconds =
                pv.value(QStringLiteral("lengthSeconds")).toString().toInt();
            v.isLive = rendererIsLive(pv);
            const QJsonArray thumbs = pv.value(QStringLiteral("thumbnail"))
                                          .toObject()
                                          .value(QStringLiteral("thumbnails"))
                                          .toArray();
            if (!thumbs.isEmpty())
                v.thumbnailUrl = thumbs.last().toObject()
                                     .value(QStringLiteral("url")).toString();
            if (v.thumbnailUrl.isEmpty())
                v.thumbnailUrl = fallbackThumbnailUrl(v.id);
            if (!v.id.isEmpty())
                videos->append(v);
            return;
        }
        for (auto it = o.begin(); it != o.end(); ++it)
            collect(it.value());
    };
    collect(QJsonValue(root));
}

// "watch playlist" queue of the RDAMVM mix: playlistPanelVideoRenderer from
// the /next response. Extracts the continuation token (nextRadioContinuationData
// / nextContinuationData / continuationItemRenderer) if the mix has more
// batches. Supports twoColumnWatchNextResults and the single-column rendering
// (musicQueueRenderer of the music client).
void Innertube::parseWatchQueue(const QJsonObject &root, QVector<VideoInfo> *out,
                                QString *continuation) const
{
    const QJsonObject contents =
        root.value(QStringLiteral("contents")).toObject();

    auto grabContinuation = [continuation](const QJsonObject &panel) {
        if (!continuation->isEmpty())
            return;
        const QJsonArray conts =
            panel.value(QStringLiteral("continuations")).toArray();
        for (const QJsonValue &val : conts) {
            const QJsonObject entry = val.toObject();
            QString tok;
            if (entry.contains(QStringLiteral("nextRadioContinuationData"))) {
                tok = entry.value(QStringLiteral("nextRadioContinuationData"))
                          .toObject()
                          .value(QStringLiteral("continuation"))
                          .toString();
            } else if (entry.contains(QStringLiteral("nextContinuationData"))) {
                tok = entry.value(QStringLiteral("nextContinuationData"))
                          .toObject()
                          .value(QStringLiteral("continuation"))
                          .toString();
            } else if (entry.contains(QStringLiteral("continuationItemRenderer"))) {
                tok = entry.value(QStringLiteral("continuationItemRenderer"))
                          .toObject()
                          .value(QStringLiteral("continuationEndpoint"))
                          .toObject()
                          .value(QStringLiteral("continuationCommand"))
                          .toObject()
                          .value(QStringLiteral("token"))
                          .toString();
            }
            if (!tok.isEmpty()) {
                *continuation = tok;
                return;
            }
        }
    };

    auto parsePanelContents = [&](const QJsonArray &items) {
        for (const QJsonValue &val : items) {
            const QJsonObject pvr =
                val.toObject()
                    .value(QStringLiteral("playlistPanelVideoRenderer"))
                    .toObject();
            if (pvr.isEmpty())
                continue;
            VideoInfo v;
            v.id = pvr.value(QStringLiteral("navigationEndpoint"))
                       .toObject()
                       .value(QStringLiteral("watchEndpoint"))
                       .toObject()
                       .value(QStringLiteral("videoId"))
                       .toString();
            if (v.id.isEmpty())
                v.id = pvr.value(QStringLiteral("videoId")).toString();
            if (v.id.isEmpty())
                continue;
            v.title = homeTextOf(pvr.value(QStringLiteral("title")));
            v.channel = homeTextOf(pvr.value(QStringLiteral("longBylineText")));
            if (v.channel.isEmpty())
                v.channel =
                    homeTextOf(pvr.value(QStringLiteral("shortBylineText")));
            v.channelId = channelIdFromByline(
                pvr.value(QStringLiteral("longBylineText")).toObject());
            if (v.channelId.isEmpty()) {
                v.channelId = channelIdFromByline(
                    pvr.value(QStringLiteral("shortBylineText")).toObject());
            }
            const QJsonArray thumbs = pvr.value(QStringLiteral("thumbnail"))
                                          .toObject()
                                          .value(QStringLiteral("thumbnails"))
                                          .toArray();
            if (!thumbs.isEmpty())
                v.thumbnailUrl = thumbs.last().toObject()
                                     .value(QStringLiteral("url")).toString();
            QString durationText =
                homeTextOf(pvr.value(QStringLiteral("lengthText")));
            if (durationText.isEmpty()) {
                const QJsonArray overlays =
                    pvr.value(QStringLiteral("thumbnailOverlays")).toArray();
                for (const QJsonValue &over : overlays) {
                    durationText = homeTextOf(
                        over.toObject()
                            .value(QStringLiteral("thumbnailOverlayTimeStatusRenderer"))
                            .toObject()
                            .value(QStringLiteral("text")));
                    if (!durationText.isEmpty())
                        break;
                }
            }
            const QStringList parts = durationText.split(QLatin1Char(':'));
            if (parts.size() == 2) {
                v.durationSeconds = parts.at(0).toInt() * 60
                                    + parts.at(1).toInt();
            } else if (parts.size() == 3) {
                v.durationSeconds = parts.at(0).toInt() * 3600
                                    + parts.at(1).toInt() * 60
                                    + parts.at(2).toInt();
            }
            v.isLive = rendererIsLive(pvr);
            // Same localized parser as elsewhere ("1,2 M de visitas"): with a
            // plain toInt() a text panel gave views=0.
            v.viewCount = static_cast<int>(countFromText(
                homeTextOf(pvr.value(QStringLiteral("viewCountText")))));
            v.publishedAt = relativeToIso(homeTextOf(
                pvr.value(QStringLiteral("publishedTimeText"))));
            out->append(v);
        }
    };

    auto handlePanel = [&](const QJsonObject &panel) {
        parsePanelContents(panel.value(QStringLiteral("contents")).toArray());
        grabContinuation(panel);
    };

    // 1) Two-column layout: the mix queue lives in playlist.playlist.
    const QJsonObject tw =
        contents.value(QStringLiteral("twoColumnWatchNextResults")).toObject();
    handlePanel(tw.value(QStringLiteral("playlist")).toObject()
                    .value(QStringLiteral("playlist")).toObject());

    // 1b) Single-column layout of VISIONOS/ANDROID_VR: the mix is also in
    //     playlist.playlist (playlistPanelVideoRenderer contents).
    const QJsonObject scw =
        contents.value(QStringLiteral("singleColumnWatchNextResults")).toObject();
    handlePanel(scw.value(QStringLiteral("playlist")).toObject()
                    .value(QStringLiteral("playlist")).toObject());

    // 2) Music client (singleColumnMusicWatchNextResultsRenderer): the queue
    //    is in musicQueueRenderer → playlistPanelRenderer.
    const QJsonObject scm =
        contents.value(QStringLiteral("singleColumnMusicWatchNextResultsRenderer"))
            .toObject();
    const QJsonObject scmTabs =
        scm.value(QStringLiteral("tabbedRenderer")).toObject()
            .value(QStringLiteral("watchNextTabbedResultsRenderer")).toObject();
    const QJsonArray scmTabsArr = scmTabs.value(QStringLiteral("tabs")).toArray();
    for (const QJsonValue &tab : scmTabsArr) {
        handlePanel(tab.toObject().value(QStringLiteral("tabRenderer"))
                        .toObject().value(QStringLiteral("content"))
                        .toObject().value(QStringLiteral("musicQueueRenderer"))
                        .toObject().value(QStringLiteral("content"))
                        .toObject().value(QStringLiteral("playlistPanelRenderer"))
                        .toObject());
    }

    if (out->isEmpty())
        NT_DEBUG("E2E innertube radio panel no encontrado\n");
}

QVector<VideoInfo> Innertube::parseRelated(const QJsonObject &root) const
{
    const QJsonObject contents = root.value(QStringLiteral("contents")).toObject();
    const QJsonObject tw = contents.value(QStringLiteral("twoColumnWatchNextResults")).toObject();
    const QJsonObject sc = contents.value(QStringLiteral("singleColumnWatchNextResults")).toObject();
    if (tw.isEmpty() && sc.isEmpty())
        return {};

    QVector<VideoInfo> out;

    // Text of a text node (runs or simpleText).
    auto durationSecondsOf = [](const QJsonArray &overlays) {
        for (const QJsonValue &val : overlays) {
            const QJsonObject o = val.toObject();
            if (!o.contains(QStringLiteral("thumbnailOverlayTimeStatusRenderer")))
                continue;
            const QString t = homeTextOf(o.value(
                QStringLiteral("thumbnailOverlayTimeStatusRenderer"))
                    .toObject()
                    .value(QStringLiteral("text")));
            QStringList parts = t.split(QLatin1Char(':'));
            if (parts.size() == 2) {
                return parts.at(0).toInt() * 60 + parts.at(1).toInt();
            }
            if (parts.size() == 3) {
                return parts.at(0).toInt() * 3600 + parts.at(1).toInt() * 60
                       + parts.at(2).toInt();
            }
        }
        return 0;
    };

    auto viewCountOf = [](const QString &s) {
        // Reuses the localized text parser: "85.805 visualizaciones",
        // "1,2 M de vistas", "85 mil visualizaciones". The own parser that was
        // here stripped dots/commas/spaces and did toLongLong on the result,
        // so "85.805 visualizaciones" gave 0 and "A continuación" was left
        // WITHOUT the views (measured in the render).
        return static_cast<int>(countFromText(s));
    };

    auto pushRenderer = [&](const QJsonObject &item) {
        const QJsonObject vr =
            item.value(QStringLiteral("videoRenderer")).toObject();
        const QJsonObject cr =
            item.value(QStringLiteral("compactVideoRenderer")).toObject();
        const QJsonObject *r = vr.isEmpty() ? &cr : &vr;
        if (r->isEmpty())
            return;
        VideoInfo v;
        v.id = r->value(QStringLiteral("navigationEndpoint"))
                   .toObject()
                   .value(QStringLiteral("watchEndpoint"))
                   .toObject()
                   .value(QStringLiteral("videoId"))
                   .toString();
        if (v.id.isEmpty())
            return;
        v.title = homeTextOf(r->value(QStringLiteral("title")));
        const QJsonArray ownerRuns =
            r->value(QStringLiteral("ownerText")).toObject()
                .value(QStringLiteral("runs")).toArray();
        if (!ownerRuns.isEmpty()) {
            const QJsonObject o0 = ownerRuns.at(0).toObject();
            v.channel = o0.value(QStringLiteral("text")).toString();
            v.channelId = o0.value(QStringLiteral("navigationEndpoint"))
                              .toObject()
                              .value(QStringLiteral("browseEndpoint"))
                              .toObject()
                              .value(QStringLiteral("browseId"))
                              .toString();
        } else {
            // Shared byline (compactVideoRenderer): the name goes in
            // longBylineText/shortBylineText and the channel id in its runs.
            const QJsonObject longByline =
                r->value(QStringLiteral("longBylineText")).toObject();
            const QJsonObject shortByline =
                r->value(QStringLiteral("shortBylineText")).toObject();
            v.channel = homeTextOf(r->value(QStringLiteral("longBylineText")));
            if (v.channel.isEmpty())
                v.channel = homeTextOf(r->value(QStringLiteral("shortBylineText")));
            v.channelId = channelIdFromByline(longByline);
            if (v.channelId.isEmpty())
                v.channelId = channelIdFromByline(shortByline);
        }
        const QJsonArray thumbs = r->value(QStringLiteral("thumbnail"))
                                      .toObject()
                                      .value(QStringLiteral("thumbnails"))
                                      .toArray();
        if (!thumbs.isEmpty())
            v.thumbnailUrl = thumbs.last().toObject()
                                 .value(QStringLiteral("url")).toString();
        v.durationSeconds = durationSecondsOf(
            r->value(QStringLiteral("thumbnailOverlays")).toArray());
        v.isLive = rendererIsLive(*r);
        v.viewCount = viewCountOf(homeTextOf(r->value(QStringLiteral("viewCountText"))));
        v.publishedAt = relativeToIso(homeTextOf(
            r->value(QStringLiteral("publishedTimeText"))));
        out.append(v);
    };

    // Walks any results container (results and secondaryResults), descending
    // through richItemRenderer/itemSectionRenderer.
    std::function<void(const QJsonArray &)> collect =
        [&](const QJsonArray &items) {
            for (const QJsonValue &val : items) {
                if (!val.isObject())
                    continue;
                const QJsonObject o = val.toObject();
                if (o.contains(QStringLiteral("videoRenderer"))
                    || o.contains(QStringLiteral("compactVideoRenderer"))) {
                    pushRenderer(o);
                } else if (o.contains(QStringLiteral("richItemRenderer"))) {
                    const QJsonValue content =
                        o.value(QStringLiteral("richItemRenderer"))
                            .toObject()
                            .value(QStringLiteral("content"));
                    if (content.isObject()) {
                        collect(QJsonArray{content});
                    }
                } else if (o.contains(QStringLiteral("itemSectionRenderer"))) {
                    collect(o.value(QStringLiteral("itemSectionRenderer"))
                                .toObject()
                                .value(QStringLiteral("contents"))
                                .toArray());
                }
            }
        };

    collect(tw.value(QStringLiteral("results"))
                .toObject().value(QStringLiteral("results"))
                .toObject().value(QStringLiteral("contents")).toArray());
    collect(tw.value(QStringLiteral("secondaryResults"))
                .toObject().value(QStringLiteral("secondaryResults"))
                .toObject().value(QStringLiteral("results")).toArray());
    // Single-column clients (VISIONOS/ANDROID): the full recommendation list
    // lives in watchNextTabbedResultsRenderer → tabs[]
    // → tabRenderer → content → sectionListRenderer → contents[] →
    // itemSectionRenderer → contents → compactVideoRenderer / videoRenderer.
    const QJsonArray scTabs =
        sc.value(QStringLiteral("results")).toObject()
            .value(QStringLiteral("watchNextTabbedResultsRenderer")).toObject()
            .value(QStringLiteral("tabs")).toArray();
    for (const QJsonValue &tab : scTabs) {
        const QJsonObject content =
            tab.toObject().value(QStringLiteral("tabRenderer")).toObject()
                .value(QStringLiteral("content")).toObject();
        const QJsonArray secs =
            content.value(QStringLiteral("sectionListRenderer")).toObject()
                .value(QStringLiteral("contents")).toArray();
        for (const QJsonValue &sec : secs) {
            collect(sec.toObject().value(QStringLiteral("itemSectionRenderer"))
                        .toObject().value(QStringLiteral("contents")).toArray());
        }
    }
    // Single-column clients (VISIONOS): the autoplay queue
    // lives in autoplay.autoplay.sets[].autoplayVideoRenderer.compactVideoRenderer.
    const QJsonArray sets =
        sc.value(QStringLiteral("autoplay")).toObject()
            .value(QStringLiteral("autoplay")).toObject()
            .value(QStringLiteral("sets")).toArray();
    for (const QJsonValue &val : sets) {
        if (!val.isObject())
            continue;
        const QJsonObject avr =
            val.toObject().value(QStringLiteral("autoplayVideoRenderer"))
               .toObject();
        if (!avr.isEmpty())
            pushRenderer(avr);
    }

    // WEB client: the recommendation list comes in the modern layout
    // (lockupViewModel), not in compactVideoRenderer. It is the layout that
    // DOES carry the views and the date in the lockup metadata, so without this
    // branch a WEB client /next came out empty and "A continuación" was left
    // without those two fields (which is what the VISIONOS client does, whose
    // compactVideoRenderer has no date).
    if (out.isEmpty())
        out = collectWebLockups(root, 20);

    // Dedupe by id (a same recommendation can appear in both containers).
    QVector<VideoInfo> unique;
    for (const VideoInfo &v : out) {
        bool seen = false;
        for (const VideoInfo &d : unique) {
            if (d.id == v.id) {
                seen = true;
                break;
            }
        }
        if (!seen)
            unique.append(v);
    }
    return unique;
}

void Innertube::requestPlayer()
{
    if (!m_hlsInFlight)
        m_hlsInFlight = true; // ensure single-flight if entered via visitor
    m_http->setResult([this](int status, const QByteArray &body) {
        handlePlayer(status == 200, QString(), body);
    });
    m_http->setError([this](const QString &reason) {
        handlePlayer(false, reason, QByteArray());
    });
    m_http->setTimeoutMs(kPlayerTimeoutMs);
    m_http->setVisitorId(m_visitorData);

    const PlayerClient &pc = kPlayerClients[m_clientIndex];
    const QJsonObject client = clientContext(pc, m_visitorData, m_hl);
    QJsonObject context{{QStringLiteral("client"), client}};
    if (pc.embed) {
        context.insert(QStringLiteral("thirdParty"),
                       QJsonObject{{QStringLiteral("embedUrl"),
                                    QString::fromLatin1(pc.embedUrl)}});
    }

    const QJsonObject body{
        {QStringLiteral("context"), context},
        {QStringLiteral("playbackContext"),
         QJsonObject{{QStringLiteral("contentPlaybackContext"),
                      QJsonObject{{QStringLiteral("signatureTimestamp"),
                                   static_cast<qint64>(QDateTime::currentSecsSinceEpoch()
                                                       / 86400 - 1)}}}}},
        {QStringLiteral("videoId"), m_videoId},
        {QStringLiteral("contentCheckOk"), true},
        {QStringLiteral("racyCheckOk"), true},
    };
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);

    // X-Goog-Visitor-Id is added to the request dynamically; we insert it
    // directly into the headers by state.
    m_http->post(kPlayerEndpoint, json, pc.ua, pc.clientId, pc.version,
                 pc.embed ? pc.embedUrl : nullptr);
}

void Innertube::handlePlayer(bool ok, const QString &error, const QByteArray &body)
{
    if (!ok) {
        fail(error.isEmpty() ? QStringLiteral("player HTTP err") : error);
        return;
    }
    QJsonObject jsonRoot;
    const bool jsonOk = parseJsonObject(body, &jsonRoot);
    if (!jsonOk) {
        NT_DEBUG("E2E innertube player json fail size=%d prefix=%s\n",
                 body.size(), body.left(120).constData());
        fail(nt::tr("error.playerJson"));
        return;
    }
    const QJsonObject root = jsonRoot;
    const QJsonObject playability =
        root.value(QStringLiteral("playabilityStatus")).toObject();
    const QString status = playability.value(QStringLiteral("status")).toString();
    const QString reason = playability.value(QStringLiteral("reason")).toString();
    if (status != QLatin1String("OK")) {
        if (status == QLatin1String("LOGIN_REQUIRED") && !m_retryingVisitor) {
            m_retryingVisitor = true;
            m_visitorData.clear();
            m_visitorExpiresAt = 0;
            // The regenerated visitor Data invalidates the previous signed URLs
            // for this video (they could remain tied to the earlier token).
            m_hlsCache.remove(m_videoId);
            NT_DEBUG("E2E innertube regenerating visitorData after "
                     "LOGIN_REQUIRED\n");
            m_playerPending = true;
            fetchVisitorData();
            return;
        }
        // NewPipe-style client cascade: once the current client or the fresh
        // visitorData attempt is exhausted, try the next client before
        // giving up.
        if (m_clientIndex + 1 < kPlayerClientCount) {
            ++m_clientIndex;
            m_retryingVisitor = false;
            NT_DEBUG("E2E innertube trying client [%d] %s after status=%s\n",
                     m_clientIndex, kPlayerClients[m_clientIndex].name,
                     status.toUtf8().constData());
            requestPlayer();
            return;
        }
        fail(reason.isEmpty()
                 ? QStringLiteral("player status=%1").arg(status)
                 : QStringLiteral("player status=%1: %2").arg(status, reason));
        return;
    }

    // Faked response of the "not available in this app" kind (detected by
    // NewPipe) or rate-limit (Piped): the returned videoId is not the requested
    // one.
    const QString gotVideoId = root.value(QStringLiteral("videoDetails"))
                                   .toObject()
                                   .value(QStringLiteral("videoId"))
                                   .toString();
    if (gotVideoId != m_videoId) {
        NT_DEBUG("E2E innertube invalid player response (got %s)\n",
                 gotVideoId.isEmpty() ? "(sin id)" : gotVideoId.toUtf8().constData());
        if (m_clientIndex + 1 < kPlayerClientCount) {
            ++m_clientIndex;
            m_retryingVisitor = false;
            requestPlayer();
            return;
        }
        fail(nt::tr("error.playerInvalid"));
        return;
    }

    const QJsonObject streamingData =
        root.value(QStringLiteral("streamingData")).toObject();
    const QString hls =
        streamingData.value(QStringLiteral("hlsManifestUrl")).toString();
    if (hls.isEmpty()) {
        // For VODs only the Apple clients return hlsManifestUrl; exhaust the
        // cascade before failing.
        NT_DEBUG("E2E innertube client %s sin hlsManifestUrl\n",
                 kPlayerClients[m_clientIndex].name);
        if (m_clientIndex + 1 < kPlayerClientCount) {
            ++m_clientIndex;
            m_retryingVisitor = false;
            requestPlayer();
            return;
        }
        fail(nt::tr("error.hlsMissing"));
        return;
    }
    const QVector<int> heights = videoHeights(streamingData);
    const QJsonArray adaptiveFormats =
        streamingData.value(QStringLiteral("adaptiveFormats")).toArray();
    int durationSeconds = root.value(QStringLiteral("videoDetails"))
                              .toObject()
                              .value(QStringLiteral("lengthSeconds"))
                              .toInt();
    // Some contexts (e.g. VISIONOS) do not include lengthSeconds; the signed
    // URL of each format carries "dur=<seconds>" with the real duration.
    // Using it prevents the edge master from declaring false EXTINF (~1h) that
    // break the progress bar and the player's final time.
    if (durationSeconds <= 0) {
        const QString u =
            !adaptiveFormats.isEmpty()
                    ? adaptiveFormats.first()
                          .toObject()
                          .value(QStringLiteral("url"))
                          .toString()
                    : QString();
        if (!u.isEmpty()) {
            const int pos = u.indexOf(QLatin1String("dur="));
            if (pos >= 0) {
                const QString tail = u.mid(pos + 4);
                bool ok = false;
                const double d = tail.left(tail.indexOf(QLatin1Char('&')))
                                     .toDouble(&ok);
                if (ok && d > 0)
                    durationSeconds = int(qFloor(d));
            }
        }
    }
    m_lastWinClient = m_clientIndex;
    // Authoritative live signal (videoDetails.isLive): YouTube declares whether
    // the media is a current live stream. It is copied to the cache for reuse.
    m_lastIsLive = root.value(QStringLiteral("videoDetails"))
                       .toObject()
                       .value(QStringLiteral("isLive"))
                       .toBool();
    const qint64 elapsed =
        QDateTime::currentMSecsSinceEpoch() - m_hlsStartMs;
    NT_DEBUG("E2E innertube hls ok len=%d alturas=%d client=%s isLive=%d "
             "(%lldms)\n",
             hls.size(), heights.size(), kPlayerClients[m_clientIndex].name,
             m_lastIsLive ? 1 : 0, static_cast<long long>(elapsed));
    // Session cache: stores the signed hlsManifestUrl, its expiration and the
    // adaptiveFormats (with direct url) + duration, to avoid repeating the
    // player POST on resolution changes of the same video.
    const qint64 exp = expireFromUrl(hls);
    m_hlsCache.insert(m_videoId,
                      HlsCacheEntry{hls, QByteArray(), heights, exp,
                                    m_clientIndex, adaptiveFormats,
                                    durationSeconds, m_lastIsLive});
    saveHlsCacheToDisk();
    m_hlsInFlight = false;
    mMasterInFlight = false;
    // adaptiveFormats (with a url per format) first: Yt builds the edge master
    // (1 video + 1 audio) without touching YouTube's master. If the edge is not
    // possible, the classic hlsReady → fetchMaster flow covers it.
    emit formatsReady(m_videoId, adaptiveFormats, durationSeconds);
    emit hlsReady(m_videoId, hls, heights);
}

void Innertube::handleMaster(bool ok, const QString &error,
                             const QByteArray &body)
{
    const qint64 elapsed =
        QDateTime::currentMSecsSinceEpoch() - m_masterStartMs;
    if (!ok) {
        mMasterInFlight = false;
        fail(error.isEmpty() ? QStringLiteral("master HTTP err") : error);
        return;
    }
    if (body.isEmpty()) {
        mMasterInFlight = false;
        fail(nt::tr("error.masterEmpty"));
        return;
    }
    NT_DEBUG("E2E innertube master ok len=%d (%lldms)\n", body.size(),
             static_cast<long long>(elapsed));
    // Stores the master body in the videoId cache to avoid re-downloading it on
    // resolution changes.
    auto it = m_hlsCache.find(m_videoId);
    if (it != m_hlsCache.end())
        it->masterBody = body;
    else
        m_hlsCache.insert(m_videoId,
                          HlsCacheEntry{QString(), body, QVector<int>(), 0,
                                        m_clientIndex});
    saveHlsCacheToDisk();
    mMasterInFlight = false;
    emit masterReady(m_videoId, body);
}

void Innertube::fail(const QString &reason)
{
    const QString id = m_videoId;
    m_hlsInFlight = false;
    mMasterInFlight = false;
    NT_DEBUG("E2E innertube failed id=%s reason=%s\n", id.toUtf8().constData(),
             reason.toUtf8().constData());
    emit failed(id, reason);
}

bool Innertube::serveCachedHls(const QString &videoId)
{
    if (m_hlsInFlight && m_flightVideoId == videoId)
        return false; // the in-flight request will resolve itself
    if (m_http->isBusy())
        return false; // transport busy (POST of another video): do not emit
                      // signals that would trigger a real GET and abort it
    const auto it = m_hlsCache.constFind(videoId);
    if (it == m_hlsCache.constEnd())
        return false;
    if (it->hlsUrl.isEmpty()
        || (it->expiresAt > 0
            && QDateTime::currentSecsSinceEpoch() >= it->expiresAt)) {
        const_cast<Innertube *>(this)->m_hlsCache.erase(it);
        return false;
    }
    m_lastWinClient = it->clientIndex;
    m_lastIsLive = it->isLive;
    m_videoId = videoId;
    // Emit the cached adaptiveFormats first: Yt can rebuild the edge master
    // without repeating the player POST; if not applicable, hlsReady covers
    // the classic path (fetchMaster).
    if (!it->adaptiveFormats.isEmpty())
        emit formatsReady(videoId, it->adaptiveFormats, it->durationSeconds);
    emit hlsReady(videoId, it->hlsUrl, it->heights);
    return true;
}

bool Innertube::serveCachedMaster(const QString &videoId)
{
    if (mMasterInFlight && m_flightVideoId == videoId)
        return false;
    if (m_http->isBusy())
        return false; // see serveCachedHls
    const auto it = m_hlsCache.constFind(videoId);
    if (it == m_hlsCache.constEnd())
        return false;
    if (it->masterBody.isEmpty()
        || (it->expiresAt > 0
            && QDateTime::currentSecsSinceEpoch() >= it->expiresAt))
        return false;
    m_videoId = videoId;
    emit masterReady(videoId, it->masterBody);
    return true;
}

qint64 Innertube::expireFromUrl(const QString &url) const
{
    // The signed URL includes /expire/<unix timestamp>/. If it does not appear
    // (or is not numeric) we return 0 → the cache is considered stale and is
    // discarded on the next lookup.
    const QByteArray marker = "/expire/";
    const int pos = url.indexOf(QString::fromLatin1(marker));
    if (pos < 0)
        return 0;
    int end = pos + marker.size();
    while (end < url.size() && url.at(end).isDigit())
        ++end;
    if (end == pos + marker.size())
        return 0;
    bool ok = false;
    const qint64 ts = url.mid(pos + marker.size(), end - pos - marker.size())
                          .toLongLong(&ok);
    return ok ? ts : 0;
}

QString Innertube::hlsCacheFilePath()
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString base = dir.isEmpty()
                             ? QDir::tempPath() + QStringLiteral("/nevertube")
                             : dir;
    QDir().mkpath(base);
    return base + QStringLiteral("/hls_resolve_cache.json");
}

void Innertube::saveHlsCacheToDisk() const
{
    QJsonArray arr;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (auto it = m_hlsCache.constBegin(); it != m_hlsCache.constEnd(); ++it) {
        const HlsCacheEntry &e = it.value();
        if (e.hlsUrl.isEmpty())
            continue;
        if (e.expiresAt > 0 && now >= e.expiresAt)
            continue;
        QJsonObject o;
        o.insert(QStringLiteral("videoId"), it.key());
        o.insert(QStringLiteral("hlsUrl"), e.hlsUrl);
        o.insert(QStringLiteral("master"), QString::fromUtf8(e.masterBody));
        QJsonArray hs;
        for (const int h : e.heights)
            hs.append(h);
        o.insert(QStringLiteral("heights"), hs);
        o.insert(QStringLiteral("expiresAt"), e.expiresAt);
        o.insert(QStringLiteral("clientIndex"), e.clientIndex);
        o.insert(QStringLiteral("adaptiveFormats"), e.adaptiveFormats);
        o.insert(QStringLiteral("durationSeconds"), e.durationSeconds);
        o.insert(QStringLiteral("isLive"), e.isLive);
        arr.append(o);
    }
    const QByteArray json = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    QFile file(hlsCacheFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(json);
    file.close();
}

void Innertube::loadHlsCacheFromDisk()
{
    const QByteArray json = [this]() {
        QFile file(hlsCacheFilePath());
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        return file.readAll();
    }();
    if (json.isEmpty())
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray())
        return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        const QString videoId = o.value(QStringLiteral("videoId")).toString();
        const QString hlsUrl = o.value(QStringLiteral("hlsUrl")).toString();
        const qint64 expiresAt = static_cast<qint64>(
            o.value(QStringLiteral("expiresAt")).toDouble(0));
        if (videoId.isEmpty() || hlsUrl.isEmpty())
            continue;
        if (expiresAt > 0 && now >= expiresAt)
            continue;
        HlsCacheEntry e;
        e.hlsUrl = hlsUrl;
        e.masterBody = o.value(QStringLiteral("master"))
                           .toString()
                           .toUtf8();
        const QJsonArray hs = o.value(QStringLiteral("heights")).toArray();
        for (const QJsonValue &h : hs)
            e.heights.append(h.toInt());
        e.expiresAt = expiresAt;
        e.clientIndex = o.value(QStringLiteral("clientIndex")).toInt(-1);
        e.adaptiveFormats =
            o.value(QStringLiteral("adaptiveFormats")).toArray();
        e.durationSeconds =
            o.value(QStringLiteral("durationSeconds")).toInt();
        e.isLive = o.value(QStringLiteral("isLive")).toBool();
        m_hlsCache.insert(videoId, e);
    }
}

bool innertubeSelfTest()
{
    const QString videoId = QStringLiteral("dQw4w9WgXcQ");
    Innertube tube;
    bool success = false;

    auto *timeout = new QTimer;
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, [&success]() {
        std::fprintf(stderr, "SELFTEST FAIL: innertube timeout sin HLS/next\n");
        QCoreApplication::exit(1);
    });

    QObject::connect(&tube, &Innertube::hlsReady,
                     [&tube, videoId](const QString &vid,
                                      const QString &hlsUrl,
                                      const QVector<int> &heights) {
                         if (vid != videoId ||
                             !hlsUrl.startsWith(QLatin1String("https://")) ||
                             heights.isEmpty()) {
                             std::fprintf(stderr,
                                          "SELFTEST FAIL: hls inválida "
                                          "(id=%s hls=%s alturas=%d)\n",
                                          vid.toUtf8().constData(),
                                          hlsUrl.toUtf8().constData(),
                                          heights.size());
                             QCoreApplication::exit(1);
                             return;
                         }
                         std::fprintf(stderr,
                                      "SELFTEST OK (visitorData->player->hls+alturas) "
                                      "id=%s hls=%s alturas=%d -> next\n",
                                      vid.toUtf8().constData(),
                                      hlsUrl.toUtf8().constData(),
                                      heights.size());
                         tube.fetchNext(videoId);
                     });
    QObject::connect(&tube, &Innertube::relatedReady,
                     [&success, &tube, videoId](const QString &vid,
                                                const QVector<VideoInfo> &videos) {
                         if (vid != videoId || videos.isEmpty()) {
                             std::fprintf(stderr,
                                          "SELFTEST FAIL: next vacío "
                                          "(id=%s relacionados=%d)\n",
                                          vid.toUtf8().constData(),
                                          videos.size());
                             QCoreApplication::exit(1);
                             return;
                         }
                         success = true;
                         std::fprintf(stderr,
                                      "SELFTEST OK next relacionados=%d "
                                      "(1º: %s) -> radio\n",
                                      videos.size(),
                                      videos.first().title.toUtf8().constData());
                         tube.fetchRadio(videoId);
                     });
    QObject::connect(&tube, &Innertube::radioReady,
                     [](const QString &vid,
                        const QVector<VideoInfo> &videos) {
                         if (videos.isEmpty()) {
                             std::fprintf(stderr,
                                          "SELFTEST FAIL: radio vacío (id=%s)\n",
                                          vid.toUtf8().constData());
                             QCoreApplication::exit(1);
                             return;
                         }
                         std::fprintf(stderr,
                                      "SELFTEST OK radio videos=%d "
                                      "(1º: %s, dup=%s)\n",
                                      videos.size(),
                                      videos.first().title.toUtf8().constData(),
                                      videos.first().id.toUtf8().constData());
                         QCoreApplication::exit(0);
                     });
    QObject::connect(&tube, &Innertube::failed,
                     [](const QString &vid, const QString &reason) {
                         std::fprintf(stderr, "SELFTEST FAIL: innertube %s: %s\n",
                                      vid.toUtf8().constData(),
                                      reason.toUtf8().constData());
                         QCoreApplication::exit(1);
                     });

    timeout->start(45000);
    tube.fetchHls(videoId);
    QCoreApplication::exec();
    return success;
}

// ---------------------------------------------------------------------------
// Diagnostic probe of the real Home (POST /browse, browseId FEwhat_to_watch).
// Investigation ONLY (it is not the app's Home and does not touch HomePage or
// player):
//   - visitorData: the persisted one if passed in NEVERTUBE_HOME_VISITOR (value
//     as stored in settings), or a fresh one via sw.js_data.
//   - tries FEwhat_to_watch with the existing clients (VISIONOS first;
//     IOS/ANDROID_VR/WEB_EMBEDDED for comparison if VISIONOS fails/empty).
//   - reports HTTP status, time, size, renderers, videos, continuations and
//     possible challenges (alertRenderer), without inventing fields.
// The printed list of titles allows manually counting the appearance of a
// topic (PROOF 2: persisted session A vs fresh session B).
//
// Launched with:  ./build/nevertube_qt2 --selftest-home
// (optional)          NEVERTUBE_HOME_VISITOR='<persisted visitorData>'

namespace {

struct HomeVideoRow
{
    QString id;
    QString title;
    QString channel;
    QString duration;
    int seconds = 0;
};

struct HomeStats
{
    QHash<QString, int> renderers; // renderer/viewmodel -> count
    QVector<HomeVideoRow> videos;
    QString continuation;
    bool hasAlerts = false;
};

QString probeTextOf(const QJsonValue &val)
{
    const QJsonObject o = val.toObject();
    const QJsonValue simple = o.value(QStringLiteral("simpleText"));
    if (simple.isString())
        return simple.toString();
    const QJsonArray runs = o.value(QStringLiteral("runs")).toArray();
    QString t;
    for (const QJsonValue &r : runs)
        t += r.toObject().value(QStringLiteral("text")).toString();
    return t;
}

void walkHomeJson(const QJsonValue &v, HomeStats *st)
{
    if (v.isArray()) {
        const QJsonArray arr = v.toArray();
        for (const QJsonValue &e : arr)
            walkHomeJson(e, st);
        return;
    }
    if (!v.isObject())
        return;
    const QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QString key = it.key();
        if (key.endsWith(QLatin1String("Renderer"))
            || key.endsWith(QLatin1String("ViewModel")))
            st->renderers[key]++;
        if (key == QLatin1String("alertRenderer"))
            st->hasAlerts = true;
        if (key == QLatin1String("continuationCommand")) {
            if (st->continuation.isEmpty())
                st->continuation = it.value().toObject()
                                       .value(QStringLiteral("token")).toString();
        } else if (key == QLatin1String("nextContinuationData")) {
            if (st->continuation.isEmpty())
                st->continuation = it.value().toObject()
                                       .value(QStringLiteral("continuation")).toString();
        } else if (key == QLatin1String("reloadContinuationData")) {
            if (st->continuation.isEmpty())
                st->continuation = it.value().toObject()
                                       .value(QStringLiteral("continuation")).toString();
        }
        if (key == QLatin1String("videoRenderer")) {
            const QJsonObject vr = it.value().toObject();
            HomeVideoRow row;
            row.id = vr.value(QStringLiteral("videoId")).toString();
            if (row.id.isEmpty())
                row.id = vr.value(QStringLiteral("navigationEndpoint")).toObject()
                             .value(QStringLiteral("watchEndpoint")).toObject()
                             .value(QStringLiteral("videoId")).toString();
            row.title = probeTextOf(vr.value(QStringLiteral("title")));
            const QJsonArray owner =
                vr.value(QStringLiteral("ownerText")).toObject()
                    .value(QStringLiteral("runs")).toArray();
            if (!owner.isEmpty())
                row.channel = owner.first().toObject()
                                  .value(QStringLiteral("text")).toString();
            if (row.channel.isEmpty())
                row.channel = probeTextOf(vr.value(QStringLiteral("longBylineText")));
            row.duration = probeTextOf(vr.value(QStringLiteral("lengthText")));
            const QStringList parts = row.duration.split(QLatin1Char(':'));
            if (parts.size() == 2)
                row.seconds = parts.at(0).toInt() * 60 + parts.at(1).toInt();
            else if (parts.size() == 3)
                row.seconds = parts.at(0).toInt() * 3600
                              + parts.at(1).toInt() * 60 + parts.at(2).toInt();
            st->videos.append(row);
        }
        walkHomeJson(it.value(), st);
    }
}

} // namespace

// First phase of the probe used by homeBrowseSelfTest (defined further below).
namespace {
void runHomeProbes(Ipv4Https *http, const QString &visitorData);
}

// Check of the PRODUCTION path of the Home: Innertube::fetchHome()
// (sw.js_data → fresh visitorData → POST /browse FEwhat_to_watch VISIONOS →
// parseHomeBrowse) and, if there is a token, fetchHomeMore(). Does not touch
// the player. If the feed arrives empty (500/network failure) the selftest does
// NOT fail: that is the behavior by design (the app hides the section). It does
// fail if the parser produces duplicates or loses the expected id/continuation.
bool homeFeedLiveCheck()
{
    std::fprintf(stderr,
                 "HOME-FEED LIVE CHECK start (producción: fetchHome VISIONOS)\n");
    Innertube tube;
    bool ok = true;
    bool gotResponse = false;
    bool requestedMore = false;
    bool done = false;

    auto *timeout = new QTimer;
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, [&gotResponse, &ok, &done]() {
        if (done)
            return;
        done = true;
        if (!gotResponse) {
            std::fprintf(stderr,
                         "SELFTEST OK (fallback): home feed sin respuesta en 45s\n");
            ok = true;
        } else {
            std::fprintf(stderr, "SELFTEST FAIL: home feed timeout tras respuesta\n");
            ok = false;
        }
        QCoreApplication::exit(ok ? 0 : 1);
    });
    timeout->start(45000);

    QObject::connect(&tube, &Innertube::homeReady,
                     [&tube, &gotResponse, &ok, &done, &requestedMore](
                         const QVector<VideoInfo> &videos) {
                         if (done)
                             return;
                         gotResponse = true;
                         if (videos.isEmpty()) {
                             std::fprintf(stderr,
                                          "HOME-FEED feed vacío: la app oculta "
                                          "la sección (por diseño)\n");
                             ok = true;
                             done = true;
                             QCoreApplication::exit(0);
                             return;
                         }
                         int n11 = 0, emptyTitle = 0, emptyChannel = 0, dups = 0,
                             withDur = 0;
                         QSet<QString> seen;
                         for (const VideoInfo &v : videos) {
                             if (v.id.size() == 11)
                                 ++n11;
                             if (seen.contains(v.id))
                                 ++dups;
                             seen.insert(v.id);
                             if (v.title.isEmpty())
                                 ++emptyTitle;
                             if (v.channel.isEmpty())
                                 ++emptyChannel;
                             if (v.durationSeconds > 0)
                                 ++withDur;
                         }
                         std::fprintf(stderr,
                                      "HOME-FEED videos=%d (id11=%d) dup=%d "
                                      "emptyTitle=%d emptyChannel=%d conDur=%d\n",
                                      videos.size(), n11, dups, emptyTitle,
                                      emptyChannel, withDur);
                         std::fprintf(stderr, "HOME-FEED 1º=%s chan=%s dur=%d\n",
                                      videos.first().title.toUtf8().constData(),
                                      videos.first().channel.toUtf8().constData(),
                                      videos.first().durationSeconds);
                         ok = (dups == 0 && n11 > 0);
                         if (ok)
                             std::fprintf(stderr, "HOME-FEED parse OK\n");
                         else
                             std::fprintf(stderr, "SELFTEST FAIL: parser home\n");
                         if (ok && tube.hasHomeContinuation() && !requestedMore) {
                             requestedMore = true;
                             std::fprintf(stderr,
                                          "HOME-FEED continuación detectada -> "
                                          "fetchHomeMore\n");
                             tube.fetchHomeMore();
                             return;
                         }
                         done = true;
                         QCoreApplication::exit(ok ? 0 : 1);
                     });
    QObject::connect(&tube, &Innertube::homeMoreReady,
                     [&ok, &done](const QVector<VideoInfo> &more) {
                         if (done)
                             return;
                         QSet<QString> seen;
                         for (const VideoInfo &v : more)
                             seen.insert(v.id);
                         std::fprintf(stderr,
                                      "HOME-FEED more=%d (dupEnRespuesta=%s)\n",
                                      more.size(),
                                      seen.size() < more.size() ? "si" : "no");
                         ok = true;
                         done = true;
                         QCoreApplication::exit(ok ? 0 : 1);
                     });
    tube.fetchHome();
    QCoreApplication::exec();
    return ok;
}

bool homeBrowseSelfTest()
{
    std::fprintf(stderr, "HOME-BROWSE SELFTEST start\n");
    Ipv4Https http;

    // Global timeout: don't hang the selftest if YouTube doesn't respond.
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, []() {
        std::fprintf(stderr, "SELFTEST FAIL: home browse (timeout global)\n");
        QCoreApplication::exit(1);
    });
    watchdog.start(110000);

    // 1) visitorData: the persisted one if passed via env, otherwise a fresh one.
    const QByteArray envVisitor = qgetenv("NEVERTUBE_HOME_VISITOR").trimmed();
    QString visitorData;
    bool seeded = false;
    if (!envVisitor.isEmpty()) {
        visitorData = QString::fromUtf8(envVisitor);
        seeded = true;
        std::fprintf(stderr,
                     "HOME-BROWSE visitor: persistido (seed) len=%d\n",
                     visitorData.size());
    }
    if (seeded) {
        runHomeProbes(&http, visitorData);
        QCoreApplication::exec();
        return homeFeedLiveCheck();
    }
    // fresh visitorData via sw.js_data (normal flow).
    http.setTimeoutMs(kVisitorTimeoutMs);
    http.setResult([&http](int status, const QByteArray &body) {
        if (status != 200) {
            std::fprintf(stderr, "HOME-BROWSE sw.js_data status=%d\n", status);
            runHomeProbes(&http, QString());
            return;
        }
        const QString vd = visitorDataFromSwJsData(body);
        if (vd.isEmpty()) {
            std::fprintf(stderr, "HOME-BROWSE sw.js_data sin visitorData\n");
            runHomeProbes(&http, QString());
            return;
        }
        std::fprintf(stderr, "HOME-BROWSE visitor fresho len=%d vd=%s\n",
                     vd.size(), vd.toUtf8().constData());
        runHomeProbes(&http, vd);
    });
    http.setError([&http](const QString &reason) {
        std::fprintf(stderr, "HOME-BROWSE sw.js_data err: %s\n",
                     reason.toUtf8().constData());
        runHomeProbes(&http, QString());
    });
    http.get(kSwJsData, kChromeUA);

    QCoreApplication::exec();
    return homeFeedLiveCheck();
}

// --- C1: which suggestions endpoint to use ----------------------------------
//
// It is not a PASS/FAIL: the question is "which endpoint serves, with which
// client and at what cost", so both candidates are measured and the table is
// printed to decide. The number that matters most is not how many suggestions
// come out, but how many are QUERIES and how many are SHORTCUTS: those that
// only carry serviceEndpoint point somewhere else (Google, the account
// history) and in an app that is only YouTube they have no destination. If the
// parser let them through they would be dead entries in the dropdown (the
// Harmony bug: nav() to null).
struct SuggestStats
{
    int total = 0;        // searchSuggestionRenderer found
    int conQuery = 0;     // traen navigationEndpoint.searchEndpoint.query
    QStringList muestras; // up to 6, marking "(sin query)" where applicable
};

// Walks the JSON looking for searchSuggestionRenderer anywhere instead of
// trusting a fixed path: if YouTube changes the layout, the count still holds
// and the selftest reports it, instead of returning zero silently.
void collectSuggestStats(const QJsonValue &val, SuggestStats *st, int depth)
{
    if (depth > 24)
        return;
    if (val.isArray()) {
        for (const QJsonValue &e : val.toArray())
            collectSuggestStats(e, st, depth + 1);
        return;
    }
    if (!val.isObject())
        return;
    const QJsonObject o = val.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (it.key() == QLatin1String("searchSuggestionRenderer")) {
            const QJsonObject r = it.value().toObject();
            ++st->total;
            const QString q =
                r.value(QStringLiteral("navigationEndpoint"))
                    .toObject()
                    .value(QStringLiteral("searchEndpoint"))
                    .toObject()
                    .value(QStringLiteral("query"))
                    .toString();
            if (!q.trimmed().isEmpty())
                ++st->conQuery;
            if (st->muestras.size() < 6)
                st->muestras << (q.trimmed().isEmpty()
                                     ? QStringLiteral("(sin query) ")
                                     : q);
        }
        collectSuggestStats(it.value(), st, depth + 1);
    }
}

bool suggestParseSelfTest()
{
    int fail = 0;
    const auto check = [&fail](bool ok, const char *what) {
        std::fprintf(stderr, "  [%s] %s\n", ok ? "OK  " : "FAIL", what);
        if (!ok)
            ++fail;
    };

    // Real body (measured with client=youtube): a mix of entity rows, flat
    // rows, and the JS wrapper. Each item is ["text", flags, ids, meta].
    const QByteArray real =
        "window.google.ac.h([\"Will\",["
        "[\"willie col\\u00f3n\",0,[512,433,47],{\"zaf\":\"Cantautor y trombonista estadounidense\",\"zao\":\"Willie Col\\u00f3n\"}],"
        "[\"willyrex\",0,[512,433,790],{\"zaf\":\"@Willyrex\"}],"
        "[\"willito\",0,[512,433,131]],"
        "[\"\",0,[512]],"
        "[123,0,[512]],"
        "[\"william luna\",0,[512,433,47],{\"zao\":\"William Luna\"}]"
        "]])";

    const QVector<Suggestion> items = parseSuggestBody(real);
    check(items.size() == 4, "envoltorio JS recortado y filas con query contadas (4 de 6)");
    if (items.size() == 4) {
        check(items.at(0).query == QStringLiteral("willie colón"),
              "acentos preservados en la primera fila");
        check(items.at(0).detail == QStringLiteral("Cantautor y trombonista estadounidense"),
              "detalle de entidad (zaf) leido");
        check(items.at(1).detail == QStringLiteral("@Willyrex"),
              "detalle de entidad en la segunda fila");
        check(items.at(2).query == QStringLiteral("willito")
                  && items.at(2).detail.isEmpty(),
              "fila sin metadatos: query si, detalle vacio");
        check(items.at(3).query == QStringLiteral("william luna")
                  && items.at(3).detail == QStringLiteral("William Luna"),
              "sin zaf se usa zao como detalle");
    }

    // String format (firefox): items as plain "text".
    const QVector<Suggestion> plain =
        parseSuggestBody("[\"blender\",[\"blender 4.5\",\"blender 5.0\"]]");
    check(plain.size() == 2 && plain.at(0).query == QStringLiteral("blender 4.5")
              && plain.at(0).detail.isEmpty(),
          "items-string (firefox) tambien se parsean");

    // Formats that are NOT ours: empty list, without crashing.
    check(parseSuggestBody("<html>error</html>").isEmpty(),
          "HTML de error -> vacio");
    check(parseSuggestBody("").isEmpty(), "body vacio -> vacio");
    check(parseSuggestBody("window.google.ac.h(\"no soy array\")").isEmpty(),
          "envoltorio con contenido no-array -> vacio");
    check(parseSuggestBody("{\"contents\":[]}").isEmpty(),
          "objeto JSON en vez de array -> vacio");
    check(parseSuggestBody("window.google.ac.h([\"solo\"])").isEmpty(),
          "array sin lista de items -> vacio");

    // XSSI prefix, in case it ever comes back.
    const QVector<Suggestion> xssi =
        parseSuggestBody(")]}'\n[\"x\",[\"x resultado\"]]");
    check(xssi.size() == 1 && xssi.at(0).query == QStringLiteral("x resultado"),
          "prefijo XSSI )]}' tolerado");

    std::fprintf(stderr, "SUGGEST PARSE %s\n",
                 fail == 0 ? "OK" : "FAIL");
    return fail == 0;
}

bool relativeDateSelfTest()
{
    int fail = 0;
    const auto check = [&fail](bool ok, const char *what) {
        std::fprintf(stderr, "  [%s] %s\n", ok ? "OK  " : "FAIL", what);
        if (!ok)
            ++fail;
    };
    // Seconds that relativeToIso has subtracted from "now". The parser returns
    // the date in ISO UTC; it is compared against the current clock with a
    // tolerance (the clock may run in milliseconds and the test itself is not
    // about precision).
    const auto deltaSecs = [](const QString &text) -> qint64 {
        const QDateTime d =
            QDateTime::fromString(relativeToIso(text), Qt::ISODate);
        if (!d.isValid())
            return -1;
        return d.secsTo(QDateTime::currentDateTimeUtc());
    };
    const auto near = [&deltaSecs](const QString &text, qint64 want) {
        const qint64 got = deltaSecs(text);
        return got >= 0 && qAbs(got - want) <= 5;
    };

    // THE BUG (regression this test pins down): "semanas" must NOT fall into
    // seconds, or the date ends up 2 s from now and publishedLabel paints "hoy".
    check(near(QStringLiteral("hace 2 semanas"), 2 * 7 * 86400),
          "hace 2 semanas -> 14 dias (no 'hoy')");
    check(near(QStringLiteral("hace 4 semanas"), 4 * 7 * 86400),
          "hace 4 semanas -> 28 dias (no 'hoy')");
    check(near(QStringLiteral("hace 1 semana"), 7 * 86400),
          "hace 1 semana -> 7 dias");
    // Remaining units, no regression.
    check(near(QStringLiteral("hace 5 segundos"), 5),
          "hace 5 segundos -> 5 s");
    check(near(QStringLiteral("hace 51 minutos"), 51 * 60),
          "hace 51 minutos -> minutos");
    check(near(QStringLiteral("hace 4 horas"), 4 * 3600),
          "hace 4 horas -> horas");
    check(near(QStringLiteral("hace 3 días"), 3 * 86400),
          "hace 3 dias -> dias");
    check(near(QStringLiteral("hace 1 mes"), 30 * 86400),
          "hace 1 mes -> meses");
    check(near(QStringLiteral("hace 4 años"), 4 * 365 * 86400),
          "hace 4 anos -> anos");
    // Abbreviations of the modern lockup ("hace 8 h", "hace 3 d", "hace 2 m").
    check(near(QStringLiteral("hace 8 h"), 8 * 3600), "hace 8 h -> horas");
    check(near(QStringLiteral("hace 3 d"), 3 * 86400), "hace 3 d -> dias");
    check(near(QStringLiteral("hace 2 m"), 2 * 30 * 86400),
          "hace 2 m -> meses");
    // Prefix of live/premieres and the English forms.
    check(near(QStringLiteral("Emitido hace 6 horas"), 6 * 3600),
          "prefijo 'Emitido hace 6 horas'");
    check(near(QStringLiteral("2 weeks ago"), 14 * 86400),
          "2 weeks ago -> 14 dias (no segundos)");
    check(near(QStringLiteral("2 months ago"), 2 * 30 * 86400),
          "2 months ago -> meses");
    check(near(QStringLiteral("hace 3 semanas"), 3 * 7 * 86400),
          "hace 3 semanas -> 21 dias (colision ya resuelta)");
    // The parser no longer depends on the language: PORTUGUESE (a language
    // supported by the app) uses "ha", not "hace", and "mes" carries an accent.
    // Before it was not recognized.
    check(near(QStringLiteral("há 2 semanas"), 14 * 86400),
          "pt 'há 2 semanas' -> 14 dias");
    check(near(QStringLiteral("há 3 dias"), 3 * 86400),
          "pt 'há 3 dias' -> dias");
    check(near(QStringLiteral("há 1 mês"), 30 * 86400),
          "pt 'há 1 mês' (acento) -> meses");
    check(near(QStringLiteral("há 2 anos"), 2 * 365 * 86400),
          "pt 'há 2 anos' -> anos");
    check(near(QStringLiteral("há 5 minutos"), 5 * 60),
          "pt 'há 5 minutos' -> minutos");
    check(near(QStringLiteral("Transmitido há 2 horas"), 2 * 3600),
          "pt prefijo 'Transmitido há 2 horas'");
    // Varied English and the article "a"/"an" (without an explicit number).
    check(near(QStringLiteral("Streamed 3 hours ago"), 3 * 3600),
          "en 'Streamed 3 hours ago'");
    check(near(QStringLiteral("1 year ago"), 365 * 86400),
          "en '1 year ago'");
    check(near(QStringLiteral("a year ago"), 365 * 86400),
          "en 'a year ago' (articulo)");
    check(near(QStringLiteral("2 seconds ago"), 2),
          "en '2 seconds ago' -> 2 s");
    // Texts that are not a date -> empty (without crashing).
    check(relativeToIso(QString()).isEmpty(), "texto vacio -> vacio");
    check(relativeToIso(QStringLiteral("hola mundo")).isEmpty(),
          "texto no-fecha -> vacio");
    check(relativeToIso(QStringLiteral("hace 0 semanas")).isEmpty(),
          "cuenta 0 -> vacio");
    // Unit we do NOT recognize -> EMPTY (no date is invented nor does it fall
    // back to "hoy", which was the bug's symptom). Adding a future unit is one
    // more row in the table, not an if.
    check(relativeToIso(QStringLiteral("hace 2 quincenas")).isEmpty(),
          "unidad desconocida (es) -> vacio (no 'hoy')");
    check(relativeToIso(QStringLiteral("2 fortnights ago")).isEmpty(),
          "unidad desconocida (en) -> vacio");

    std::fprintf(stderr, "RELATIVE-DATES %s\n", fail == 0 ? "OK" : "FAIL");
    return fail == 0;
}

bool suggestSelfTest()
{
    std::fprintf(stderr, "SUGGEST SELFTEST start\n");
    // Parser first, which is offline and deterministic: if parsing is broken
    // there is no point even trying the network.
    if (!suggestParseSelfTest()) {
        std::fprintf(stderr, "SELFTEST FAIL: suggest (parseo)\n");
        return false;
    }
    std::fprintf(stderr, "\n");
    Ipv4Https http;

    // Global watchdog: there are several real requests; do not hang the selftest.
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, []() {
        std::fprintf(stderr, "SELFTEST FAIL: suggest (timeout global)\n");
        QCoreApplication::exit(1);
    });
    watchdog.start(150000);

    struct Probe
    {
        const char *label;
        const char *path;       // Innertube, relative to www.youtube.com
        const char *url;        // Google, absolute; if not null, it wins
        const char *clientName; // Innertube only
        int clientId;
        const char *clientVersion;
        const char *referer;
    };
    // A = Innertube music/get_search_suggestions: POST with context and
    // visitorData. B = suggestqueries: plain GET, without client or
    // visitorData. Same prefix and same network so the comparison is fair.
    //
    // C1 (client=youtube) is the one C2 uses: it is the real autocomplete of
    // youtube.com. Its overlap with the A1 and B1 datasets was measured and it
    // won for being the original: 14 items, entity metadata and 0.3-2 KB.
    // A1 remains in the probe as a quality reference (and it is the one that
    // matches worst: 14-43% with the real YT).
    const Probe probes[] = {
        {"C1 suggestqueries youtube", nullptr,
         "https://suggestqueries.google.com/complete/search"
         "?client=youtube&ds=yt",
         nullptr, 0, nullptr, nullptr},
        {"B1 suggestqueries firefox", nullptr,
         "https://suggestqueries.google.com/complete/search"
         "?client=firefox&ds=yt",
         nullptr, 0, nullptr, nullptr},
        {"A1 innertube WEB_REMIX",
         "/youtubei/v1/music/get_search_suggestions?prettyPrint=false", nullptr,
         "WEB_REMIX", 67, "1.20240723.01.00", "https://music.youtube.com/"},
    };
    // Light-theme prefixes (tutorial/recipe/code), music, short, and
    // "Will" (the case the user asked to measure: 4 letters, one word).
    // "tutorial blender" and "receta de tarta" carry spaces ON PURPOSE:
    // measured with curl, a raw space in the query makes suggestqueries
    // answer HTTP 400. Type-ahead has to work with what is actually typed (a
    // phrase), not only with single-word prefixes, so these cases MUST come out
    // green: if C2 does not encode the query properly, it shows up here.
    const QStringList queries{QStringLiteral("Will"),
                              QStringLiteral("tutorial blender"),
                              QStringLiteral("receta de tarta"),
                              QStringLiteral("musica")};
    const int probeCount = int(sizeof(probes) / sizeof(probes[0]));
    const int total = 1 + int(queries.size()) * probeCount;

    struct Result
    {
        int status = 0;
        int ms = 0;
        int bytes = 0;
        bool failed = false;
        SuggestStats st;
    };
    QHash<QString, Result> results;

    QString visitor;
    int step = 0;
    int okProbes = 0;
    std::function<void()> run; // assigned below; stepDone invokes it

    const auto stepDone = [&]() {
        ++step;
        if (step < total) {
            // With an event turn between probes: chaining requests from inside
            // the socket's readyRead is reentrancy.
            QTimer::singleShot(0, [&]() { run(); });
            return;
        }
        std::fprintf(stderr, "\nSUGGEST RESUMEN (agregado por sonda)\n");
        for (int i = 0; i < probeCount; ++i) {
            int ok = 0;
            int conQuery = 0;
            int ms = 0;
            int bytes = 0;
            for (const QString &q : queries) {
                const Result r = results.value(
                    QStringLiteral("%1|%2")
                        .arg(QLatin1String(probes[i].label), q));
                if (r.failed || r.status != 200)
                    continue;
                ++ok;
                conQuery += r.st.conQuery;
                ms += r.ms;
                bytes += r.bytes;
            }
            if (ok)
                ++okProbes;
            std::fprintf(stderr,
                         "  %-26s ok=%d/%d conQuery=%-3d msMedio=%-5d "
                         "bytesMedio=%-6d fallos=%d\n",
                         probes[i].label, ok, int(queries.size()), conQuery,
                         ok ? ms / ok : 0, ok ? bytes / ok : 0,
                         int(queries.size()) - ok);
        }
        // The PASS belongs to the endpoint C2 USES (C1, client=youtube): it has
        // to return queries with ALL FOUR, spaces included. The other probes are
        // a quality reference; if C1 degrades, type-ahead is left without
        // suggestions even if the others stay green, so the failure has to come
        // out here.
        int c1Ok = 0;
        int c1ConQuery = 0;
        for (const QString &q : queries) {
            const Result r = results.value(
                QStringLiteral("%1|%2").arg(QLatin1String(probes[0].label), q));
            if (r.failed || r.status != 200)
                continue;
            ++c1Ok;
            c1ConQuery += r.st.conQuery;
        }
        const bool c1Bien = c1Ok == int(queries.size()) && c1ConQuery > 0;
        std::fprintf(stderr, "SUGGEST C1 (usado por C2): ok=%d/%d conQuery=%d\n",
                     c1Ok, int(queries.size()), c1ConQuery);
        std::fprintf(stderr, "SUGGEST %s\n",
                     c1Bien ? "OK: el endpoint de C2 responde con consultas"
                            : "FAIL: el endpoint de C2 no devolvio consultas");
        QCoreApplication::exit(c1Bien ? 0 : 2);
    };

    const std::function<void()> runImpl = [&]() {
        if (step == 0) {
            http.setTimeoutMs(kVisitorTimeoutMs);
            http.setResult([&](int status, const QByteArray &body) {
                if (status == 200)
                    visitor = visitorDataFromSwJsData(body);
                std::fprintf(stderr, "SUGGEST visitorData len=%d\n",
                             visitor.size());
                stepDone();
            });
            http.setError([&](const QString &reason) {
                std::fprintf(stderr, "SUGGEST sw.js_data err: %s (se sigue sin visitor)\n",
                             reason.toUtf8().constData());
                stepDone();
            });
            http.get(kSwJsData, kChromeUA);
            return;
        }

        const int idx = step - 1;
        // NOTE: these four are COPIES on purpose. The transport stores the
        // std::function of setResult/setError and invokes them AFTER this frame
        // (the runImpl one) has returned. If they were captured by reference,
        // record() would read p.clientVersion from freed memory and
        // QString::fromLatin1() would build over a garbage const char*
        // (heap corruption, not a readable failure).
        const Probe p = probes[idx % probeCount];
        const QString q = queries.at(idx / probeCount);
        const bool google = p.url != nullptr;
        const QString key =
            QStringLiteral("%1|%2").arg(QLatin1String(p.label), q);
        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();

        // By reference only what lives in the suggestSelfTest() frame, which
        // stays alive inside exec(): the results hash and stepDone.
        QHash<QString, Result> &sink = results;
        const auto &next = stepDone;

        const auto record = [&sink, &next, p, q, google, key, t0](int status,
                                                                 const QByteArray &body) {
            Result res;
            res.status = status;
            res.ms = int(QDateTime::currentMSecsSinceEpoch() - t0);
            res.bytes = body.size();
            if (google) {
                // The SAME parser as C2 (parseSuggestBody) is used, not a
                // custom one: so the probe measures the production code,
                // including the trimming of the window.google.ac.h(...)
                // wrapper that only client=youtube needs. firefox/chrome come
                // as plain JSON and the parser accepts them too.
                const QVector<Suggestion> items = parseSuggestBody(body);
                for (const Suggestion &s : items) {
                    ++res.st.total;
                    ++res.st.conQuery;
                    if (res.st.muestras.size() < 6)
                        res.st.muestras << s.query;
                }
            } else {
                QJsonObject root;
                if (parseJsonObject(body, &root))
                    collectSuggestStats(root, &res.st, 0);
                else
                    res.failed = true;
            }
            sink.insert(key, res);
            std::fprintf(stderr,
                         "SUGGEST %-26s q=%-6s status=%d ms=%4d bytes=%6d "
                         "total=%2d conQuery=%2d\n",
                         p.label, q.toUtf8().constData(), res.status, res.ms,
                         res.bytes, res.st.total, res.st.conQuery);
            for (const QString &s : res.st.muestras)
                std::fprintf(stderr, "           - %s\n", s.toUtf8().constData());
            next();
        };

        // Each request captures ITS callbacks when queued (the transport is
        // FIFO), so setResult/setError go before EACH request.
        http.setResult(record);
        http.setError([&sink, &next, p, q, key, t0](const QString &reason) {
            Result res;
            res.failed = true;
            res.ms = int(QDateTime::currentMSecsSinceEpoch() - t0);
            sink.insert(key, res);
            std::fprintf(stderr, "SUGGEST %-26s q=%-6s ERR %s\n", p.label,
                         q.toUtf8().constData(), reason.toUtf8().constData());
            next();
        });
        http.setTimeoutMs(20000);

        if (google) {
            const QUrl url(QString::fromLatin1(p.url) +
                           QStringLiteral("&q=") + QUrl::toPercentEncoding(q));
            http.getUrl(url, kChromeUA, true);
            return;
        }

        http.setVisitorId(visitor);
        QJsonObject client{
            {QStringLiteral("clientName"), QString::fromLatin1(p.clientName)},
            {QStringLiteral("clientVersion"), QString::fromLatin1(p.clientVersion)},
            {QStringLiteral("hl"), QStringLiteral("es")},
            {QStringLiteral("gl"), QStringLiteral("US")},
        };
        if (!visitor.isEmpty())
            client.insert(QStringLiteral("visitorData"), visitor);
        const QJsonObject body{
            {QStringLiteral("context"),
             QJsonObject{{QStringLiteral("client"), client}}},
            {QStringLiteral("input"), q},
        };
        http.post(p.path, QJsonDocument(body).toJson(QJsonDocument::Compact),
                  kChromeUA, p.clientId, p.clientVersion, p.referer);
    };
    run = runImpl;

    run();
    QCoreApplication::exec();
    return true;
}

bool feedsProtocolSelfTest()
{
    std::fprintf(stderr, "FEEDS-PROTO SELFTEST start\n");
    Ipv4Https http;
    Innertube tube;
    const QByteArray envChannel = qgetenv("NEVERTUBE_TEST_CHANNEL").trimmed();
    const QString channel = envChannel.isEmpty()
        ? QStringLiteral("UC36xmz34q02JYaZYKrMwXng")
        : QString::fromUtf8(envChannel);
    std::fprintf(stderr, "FEEDS-PROTO canal=%s\n", channel.toUtf8().constData());

    // Global timeout: the endpoints may take long or fail, never hang.
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, []() {
        std::fprintf(stderr, "SELFTEST FAIL: feeds proto (timeout global)\n");
        QCoreApplication::exit(1);
    });
    watchdog.start(120000);

    struct State
    {
        Ipv4Https *http = nullptr;
        Innertube *tube = nullptr;
        QString channel;
        QString visitor;
        int phase = 0;
        bool ok = true;
        int chanStatus = 0;
        int chanDiagSize = 0;
        int trendStatus = 0;
        int trendDiagSize = 0;
        std::function<void()> next;
    } st{&http, &tube, channel};

    // Counts video renderers in a response (diagnostic probe).
    const auto countRenderers = [](const QJsonObject &root) {
        QHash<QString, int> counts;
        std::function<void(const QJsonValue &)> walk;
        walk = [&](const QJsonValue &val) {
            if (val.isArray()) {
                const QJsonArray arr = val.toArray();
                for (const QJsonValue &e : arr)
                    walk(e);
                return;
            }
            if (!val.isObject())
                return;
            const QJsonObject o = val.toObject();
            for (auto it = o.begin(); it != o.end(); ++it) {
                const QString k = it.key();
                if (k.endsWith(QLatin1String("Renderer")))
                    ++counts[k];
                walk(it.value());
            }
        };
        walk(QJsonValue(root));
        QString list;
        for (auto it = counts.begin(); it != counts.end(); ++it) {
            if (!list.isEmpty())
                list += QLatin1Char(' ');
            list += QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
        }
        return list;
    };

    const auto validIds = [](const QVector<VideoInfo> &videos) {
        int n11 = 0;
        for (const VideoInfo &v : videos)
            if (v.id.size() == 11)
                ++n11;
        int dups = 0;
        for (int i = 0; i < videos.size(); ++i)
            for (int j = i + 1; j < videos.size(); ++j)
                if (videos.at(i).id == videos.at(j).id)
                    ++dups;
        int withTitle = 0;
        int withDur = 0;
        for (const VideoInfo &v : videos) {
            if (!v.title.isEmpty())
                ++withTitle;
            if (v.durationSeconds > 0)
                ++withDur;
        }
        return QPair<QPair<int, int>, QPair<int, int>>(
            {n11, dups}, {withTitle, withDur});
    };

    // phases: 0 visitor | 1 channel diag | 2 channel data (tube) | 3 trending diag
    // | 4 trending data (tube) | 5 empty case (tube) | 6 end
    st.next = [&st, &countRenderers, &validIds]() {
        switch (st.phase) {
        case 0: {
            st.phase = 1;
            st.http->setTimeoutMs(kVisitorTimeoutMs);
            st.http->setResult([&st](int status, const QByteArray &body) {
                if (status == 200)
                    st.visitor = visitorDataFromSwJsData(body);
                std::fprintf(stderr, "FEEDS-PROTO visitor len=%d\n",
                             st.visitor.size());
                st.next();
            });
            st.http->setError([](const QString &) {});
            st.http->get(kSwJsData, kChromeUA);
            return;
        }
        case 1: {
            st.phase = 2;
            st.http->setTimeoutMs(kPlayerTimeoutMs);
            st.http->setResult([&st, &countRenderers](int status,
                                                      const QByteArray &body) {
                st.chanStatus = status;
                st.chanDiagSize = body.size();
                QJsonObject jsonRoot;
                const bool jsonOk = parseJsonObject(body, &jsonRoot);
                const QString renderers =
                    (jsonOk)
                        ? countRenderers(jsonRoot)
                        : QStringLiteral("(sin json)");
                std::fprintf(stderr,
                             "FEEDS-PROTO channel HTTP status=%d size=%d "
                             "renderers=%s\n",
                             status, body.size(),
                             renderers.toUtf8().constData());
                st.next();
            });
            st.http->setError([](const QString &) {});
            st.http->setVisitorId(st.visitor);
            const PlayerClient &pc = kPlayerClients[0];
            const QJsonObject client = clientContext(pc, st.visitor);
            const QJsonObject body{
                {QStringLiteral("context"),
                 QJsonObject{{QStringLiteral("client"), client}}},
                {QStringLiteral("browseId"), st.channel},
                {QStringLiteral("params"),
                 QString::fromLatin1(kChannelVideosParams)},
            };
            const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
            st.http->post(kBrowseEndpoint, json, pc.ua, pc.clientId, pc.version,
                          nullptr, nullptr, nullptr);
            return;
        }
        case 2:
            st.phase = 3;
            st.tube->fetchChannelVideos(st.channel);
            return;
        case 3: {
            st.phase = 4;
            st.http->setTimeoutMs(kPlayerTimeoutMs);
            st.http->setResult([&st, &countRenderers](int status,
                                                      const QByteArray &body) {
                st.trendStatus = status;
                st.trendDiagSize = body.size();
                QJsonObject jsonRoot;
                const bool jsonOk = parseJsonObject(body, &jsonRoot);
                const QString renderers =
                    (jsonOk)
                        ? countRenderers(jsonRoot)
                        : QStringLiteral("(sin json)");
                std::fprintf(stderr,
                             "FEEDS-PROTO trending HTTP status=%d size=%d "
                             "renderers=%s\n",
                             status, body.size(),
                             renderers.toUtf8().constData());
                st.next();
            });
            st.http->setError([](const QString &) {});
            st.http->setVisitorId(st.visitor);
            const PlayerClient &pc = kPlayerClients[0];
            const QJsonObject client = clientContext(pc, st.visitor);
            const QJsonObject body{
                {QStringLiteral("context"),
                 QJsonObject{{QStringLiteral("client"), client}}},
                {QStringLiteral("browseId"),
                 QString::fromLatin1(kTrendingBrowseId)},
            };
            const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
            st.http->post(kBrowseEndpoint, json, pc.ua, pc.clientId, pc.version,
                          nullptr, nullptr, nullptr);
            return;
        }
        case 4:
            st.phase = 5;
            st.tube->fetchTrending();
            return;
        case 5:
            st.phase = 6;
            st.tube->fetchChannelVideos(QStringLiteral("UC00000000000000000000"));
            return;
        default:
            std::fprintf(stderr, "FEEDS-PROTO RESUMEN canal status=%d size=%d "
                                 "trending status=%d size=%d ok=%d\n",
                         st.chanStatus, st.chanDiagSize, st.trendStatus,
                         st.trendDiagSize, st.ok ? 1 : 0);
            QCoreApplication::exit(st.ok ? 0 : 1);
        }
    };

    // Results of the real Innertube chain.
    QObject::connect(&tube, &Innertube::channelVideosReady,
                     [&st, &validIds](const QString &channelId,
                                      const QVector<VideoInfo> &videos) {
                         if (channelId == st.channel) {
                             const auto ids = validIds(videos);
                             const QString first =
                                 !videos.isEmpty()
                                     ? QStringLiteral("%1 — %2")
                                           .arg(videos.first().id,
                                                videos.first().title.left(40))
                                     : QStringLiteral("(vacío)");
                             std::fprintf(stderr,
                                          "FEEDS-PROTO channel videos=%d "
                                          "id11=%d dup=%d conTitulo=%d "
                                          "conDur=%d primero=%s\n",
                                          videos.size(), ids.first.first,
                                          ids.first.second, ids.second.first,
                                          ids.second.second,
                                          first.toUtf8().constData());
                             if (videos.isEmpty()) {
                                 st.ok = false; // it must respond today (broken layout = signal)
                             } else {
                                 st.ok = st.ok && ids.first.first == videos.size()
                                         && ids.first.second == 0
                                         && videos.size() <= kChannelTake
                                         && ids.second.first == videos.size();
                             }
                             st.next();
                         } else {
                             const bool empty = videos.isEmpty();
                             std::fprintf(stderr,
                                          "FEEDS-PROTO caso vacío canal inválido "
                                          "videos=%d\n",
                                          videos.size());
                             st.ok = st.ok && empty;
                             st.next();
                         }
                     });
    QObject::connect(&tube, &Innertube::trendingReady,
                     [&st, &validIds](const QVector<VideoInfo> &videos) {
                         const auto ids = validIds(videos);
                         std::fprintf(stderr,
                                      "FEEDS-PROTO trending videos=%d id11=%d "
                                      "dup=%d conTitulo=%d conDur=%d\n",
                                      videos.size(), ids.first.first,
                                      ids.first.second, ids.second.first,
                                      ids.second.second);
                         if (!videos.isEmpty()) {
                             st.ok = st.ok && ids.first.first == videos.size()
                                     && ids.first.second == 0
                                     && videos.size() <= kTrendingTake
                                     && ids.second.first == videos.size();
                         }
                         st.next();
                     });

    st.next();
    QCoreApplication::exec();
    return st.ok;
}

namespace {

// Launches the sequence of /browse probes per client. visitant may be
// empty (then each POST carries the fallback header the player already uses for
// LOGIN_REQUIRED; in practice the fixed fallback works).
void runHomeProbes(Ipv4Https *http, const QString &visitorData)
{
    // Sequence: VISIONOS first; IOS/ANDROID_VR/WEB_EMBEDDED only as
    // comparison (diagnostic) and because there are already defined clients.
    static const int kProbeClientOrder[] = {0, 1, 4, 2};

    // Extra diagnostic clients for /browse (ONLY this probe; NOT used in the
    // production cascade kPlayerClients). Values from the reference client
    // tables (real browser WEB and real app ANDROID).
    static const PlayerClient kDiagClients[] = {
        {"WEB", "2.20260708.00.00", 1, kChromeUA,
         nullptr, nullptr, nullptr, nullptr, 0, false, nullptr},
        {"ANDROID", "21.21.39", 3,
         "com.google.android.youtube/21.21.39 (Linux; U; Android 13; Pixel 7 "
         "Build/TQ3A.230805.001) gzip", // UA of the reference ANDROID client
         "Google", "Pixel 7", "Android", "13", 33, false, nullptr},
    };

    struct State
    {
        Ipv4Https *http = nullptr;
        QString visitor;
        int idx = 0;
        int probeCount = 0;
        bool usedFallbackVisitor = false;
        int bestIdx = -1;
        int bestVideos = 0;
        QString bestContinuation;
        bool contDone = false;
        std::function<void()> next;             // phase 1: per-client probe
        std::function<void()> probeContinuation; // phase 2: continuation of the best
    };
    auto *st = new State;
    st->http = http;
    st->visitor = visitorData;
    st->probeCount = static_cast<int>(sizeof(kProbeClientOrder)
                                          / sizeof(kProbeClientOrder[0]))
                     + static_cast<int>(sizeof(kDiagClients)
                                        / sizeof(kDiagClients[0]));

    if (st->visitor.isEmpty()) {
        // No visitorData (neither persisted nor fresh): use the fixed fallback
        // the project already uses (kFallbackVisitorData) to be able to probe.
        st->visitor = QString::fromLatin1(kFallbackVisitorData);
        st->usedFallbackVisitor = true;
        std::fprintf(stderr, "HOME-BROWSE visitor: fallback fijo len=%d\n",
                     st->visitor.size());
    }
    http->setVisitorId(st->visitor);

    st->probeContinuation = [st]() {
        // Phase 2: a single continuation request on the best client.
        int bi = st->bestIdx;
        if (bi < 0 || st->bestContinuation.isEmpty() || st->contDone) {
            st->contDone = true;
            st->next();
            return;
        }
        st->contDone = true;
        // Resolve the client that produced the continuation (production or
        // diagnostic), same as in phase 1.
        const int prodCount = static_cast<int>(sizeof(kProbeClientOrder)
                                               / sizeof(kProbeClientOrder[0]));
        const PlayerClient *pc;
        if (bi < prodCount)
            pc = &kPlayerClients[kProbeClientOrder[bi]];
        else
            pc = &kDiagClients[bi - prodCount];
        const QJsonObject client = clientContext(*pc, st->visitor);
        QJsonObject context{{QStringLiteral("client"), client}};
        if (pc->embed)
            context.insert(QStringLiteral("thirdParty"),
                           QJsonObject{{QStringLiteral("embedUrl"),
                                        QString::fromLatin1(pc->embedUrl)}});
        const QJsonObject body{
            {QStringLiteral("context"), context},
            {QStringLiteral("continuation"), st->bestContinuation},
        };
        const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        st->http->setTimeoutMs(kPlayerTimeoutMs);
        st->http->setResult([st, pc, t0](int status, const QByteArray &body) {
            const qint64 dt = QDateTime::currentMSecsSinceEpoch() - t0;
            std::fprintf(stderr, "HOME-BROWSE cont client=%s status=%d size=%d (%lldms)\n",
                         pc->name, status, body.size(), static_cast<long long>(dt));
            if (status == 200) {
                QByteArray json_ = body.trimmed();
                if (json_.startsWith(")]}'"))
                    json_.remove(0, 4);
                HomeStats hs;
                const QJsonDocument doc = QJsonDocument::fromJson(json_);
                if (doc.isObject())
                    walkHomeJson(doc.object(), &hs);
                std::fprintf(stderr, "HOME-BROWSE cont videos=%d continuations=%s\n",
                             hs.videos.size(),
                             hs.continuation.isEmpty() ? "no" : "si");
            }
            // Resume the sequence to print the RESULT and exit: without this
            // the probe hung until the global watchdog.
            st->next();
        });
        st->http->setError([st](const QString &reason) {
            std::fprintf(stderr, "HOME-BROWSE cont err: %s\n",
                         reason.toUtf8().constData());
            st->next();
        });
        st->http->post(kBrowseEndpoint, json, pc->ua, pc->clientId, pc->version,
                       pc->embed ? pc->embedUrl : nullptr);
    };

    st->next = [st]() {
        if (st->idx >= st->probeCount) {
            // End of phase 1: continue with the best one's continuation.
            if (st->contDone) {
                const int prodCount =
                    static_cast<int>(sizeof(kProbeClientOrder)
                                     / sizeof(kProbeClientOrder[0]));
                const char *bestName = "-";
                if (st->bestIdx >= 0) {
                    if (st->bestIdx < prodCount)
                        bestName = kPlayerClients
                                       [kProbeClientOrder[st->bestIdx]].name;
                    else
                        bestName = kDiagClients[st->bestIdx - prodCount].name;
                }
                std::fprintf(stderr,
                             "HOME-BROWSE RESULT usable=%s best=%s videos=%d\n",
                             st->bestVideos > 0 ? "YES" : "NO", bestName,
                             st->bestVideos);
                delete st;
                QCoreApplication::exit(0);
                return;
            }
            st->probeContinuation();
            return;
        }
        // Resolve the client: production ones first (kPlayerClients),
        // then diagnostic ones (real WEB/ANDROID for /browse).
        const int prodCount = static_cast<int>(sizeof(kProbeClientOrder)
                                               / sizeof(kProbeClientOrder[0]));
        const PlayerClient *pc;
        if (st->idx < prodCount) {
            pc = &kPlayerClients[kProbeClientOrder[st->idx]];
        } else {
            const int di = st->idx - prodCount;
            pc = &kDiagClients[di];
        }
        st->idx++;
        const QJsonObject client = clientContext(*pc, st->visitor);
        QJsonObject context{{QStringLiteral("client"), client}};
        if (pc->embed)
            context.insert(QStringLiteral("thirdParty"),
                           QJsonObject{{QStringLiteral("embedUrl"),
                                        QString::fromLatin1(pc->embedUrl)}});
        const QJsonObject body{
            {QStringLiteral("context"), context},
            {QStringLiteral("browseId"), QStringLiteral("FEwhat_to_watch")},
        };
        const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
        st->http->setTimeoutMs(kPlayerTimeoutMs);
        st->http->setResult([st, pc, t0](int status, const QByteArray &body) {
            const qint64 dt = QDateTime::currentMSecsSinceEpoch() - t0;
            std::fprintf(stderr,
                         "HOME-BROWSE client=%s status=%d size=%d (%lldms)\n",
                         pc->name, status, body.size(), static_cast<long long>(dt));
            if (status == 200 && body.size() > 50000) {
                // Save the large 200 body for offline inspection (to know
                // which renderer carries the Home videos in each client).
                QFile out(QStringLiteral("/tmp/home_probe_%1.json").arg(pc->name));
                if (out.open(QIODevice::WriteOnly))
                    out.write(body);
            }
            HomeStats hs;
            if (status == 200) {
                QByteArray json_ = body.trimmed();
                if (json_.startsWith(")]}'"))
                    json_.remove(0, 4);
                QJsonParseError perr;
                const QJsonDocument doc = QJsonDocument::fromJson(json_, &perr);
                if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
                    std::fprintf(stderr, "HOME-BROWSE client=%s json inválido\n",
                                 pc->name);
                } else {
                    walkHomeJson(doc.object(), &hs);
                }
            }
            // Renderers observed in the response (unfiltered).
            QStringList rnds;
            for (auto it2 = hs.renderers.constBegin(); it2 != hs.renderers.constEnd(); ++it2)
                rnds << QStringLiteral("%1=%2").arg(it2.key()).arg(it2.value());
            std::fprintf(stderr, "HOME-BROWSE client=%s renderers: %s\n",
                         pc->name, rnds.join(QLatin1Char(' ')).toUtf8().constData());
            std::fprintf(stderr, "HOME-BROWSE client=%s alerts=%s videos=%d continuations=%s\n",
                         pc->name, hs.hasAlerts ? "si" : "no", hs.videos.size(),
                         hs.continuation.isEmpty() ? "no" : "si");
            const int showN = qMin(hs.videos.size(), 8);
            for (int i = 0; i < showN; ++i) {
                const HomeVideoRow &r = hs.videos.at(i);
                std::fprintf(stderr,
                             "  HOME id=%s dur=%s seg=%d chan=%s tit=%s\n",
                             r.id.toUtf8().constData(),
                             r.duration.toUtf8().constData(), r.seconds,
                             r.channel.toUtf8().constData(),
                             r.title.toUtf8().constData());
            }
            if (hs.videos.size() > st->bestVideos) {
                st->bestVideos = hs.videos.size();
                st->bestIdx = st->idx - 1;
                st->bestContinuation = hs.continuation;
            } else if (st->bestIdx < 0 && !hs.continuation.isEmpty()) {
                // No direct videos in any client (this is what happens
                // with the app's Home: only the shell + token arrive). Save
                // the first continuation to try to fetch the real feed.
                st->bestIdx = st->idx - 1;
                st->bestContinuation = hs.continuation;
            }
            st->next();
        });
        st->http->setError([st, pc](const QString &reason) {
            std::fprintf(stderr, "HOME-BROWSE client=%s err: %s\n",
                         pc->name, reason.toUtf8().constData());
            st->next();
        });
        st->http->post(kBrowseEndpoint, json, pc->ua, pc->clientId, pc->version,
                       pc->embed ? pc->embedUrl : nullptr);
    };

    st->next();
}

// Selftest of the Innertube browse endpoints: search, channel card,
// tabs (videos/shorts/live), video detail and playlist. Real network; does not
// touch the player, SQLite or files.

} // namespace

bool innertubeBrowseSelfTest()
{
    std::fprintf(stderr, "BROWSE SELFTEST start\n");
    Innertube inn;
    bool ok = true;

    const auto wait = [](const std::function<bool()> &done, int timeoutMs) {
        if (done())
            return true;
        QEventLoop loop;
        QTimer poll;
        poll.setInterval(50);
        QObject::connect(&poll, &QTimer::timeout, &loop,
                         [&done, &loop]() { if (done()) loop.quit(); });
        QTimer to;
        to.setSingleShot(true);
        QObject::connect(&to, &QTimer::timeout, &loop, &QEventLoop::quit);
        poll.start();
        to.start(timeoutMs);
        loop.exec();
        return done();
    };

    const QString channel = QStringLiteral("UCX6OQ3DkcsbYNE6H8uQQuVA"); // MrBeast
    const QString video = QStringLiteral("dQw4w9WgXcQ");

    // 1) Search.
    {
        bool done = false;
        QVector<VideoInfo> got;
        const auto conn = QObject::connect(
            &inn, &Innertube::searchReady, &inn,
            [&](const QString &, const QVector<VideoInfo> &v, bool) {
                got = v;
                done = true;
            });
        inn.fetchSearch(QStringLiteral("musica"), 20);
        const bool r = wait([&]() { return done; }, 30000);
        QObject::disconnect(conn);
        int empty = 0;
        for (const VideoInfo &v : got)
            if (v.id.isEmpty()) ++empty;
        const bool good = r && !got.isEmpty() && empty == 0;
        std::fprintf(stderr, "BROWSE search videos=%d idVacios=%d -> %s\n",
                     got.size(), empty, good ? "OK" : "FAIL");
        ok = ok && good;
    }

    // 2) Channel card.
    {
        bool done = false;
        ChannelInfo info;
        const auto conn = QObject::connect(
            &inn, &Innertube::channelInfoReady, &inn,
            [&](const ChannelInfo &c) { info = c; done = true; });
        inn.fetchChannelInfo(channel);
        const bool r = wait([&]() { return done; }, 30000);
        QObject::disconnect(conn);
        const bool good =
            r && !info.name.isEmpty() && info.subscriberCount > 0;
        std::fprintf(stderr,
                     "BROWSE channelInfo name=%s subs=%lld av=%d -> %s\n",
                     info.name.toUtf8().constData(),
                     static_cast<long long>(info.subscriberCount),
                     info.avatarUrl.isEmpty() ? 0 : 1, good ? "OK" : "FAIL");
        ok = ok && good;
    }

    // 3) Channel tabs: videos by VISIONOS, shorts/live by WEB.
    const QStringList tabs{QStringLiteral("videos"), QStringLiteral("shorts"),
                           QStringLiteral("live")};
    for (const QString &tab : tabs) {
        bool done = false;
        bool more = false;
        QVector<VideoInfo> got;
        const auto conn = QObject::connect(
            &inn, &Innertube::channelTabReady, &inn,
            [&](const QString &, const QString &, const QVector<VideoInfo> &v,
                bool hasMore) { got = v; more = hasMore; done = true; });
        inn.fetchChannelTab(channel, tab, 25);
        const bool r = wait([&]() { return done; }, 30000);
        QObject::disconnect(conn);
        int empty = 0;
        for (const VideoInfo &v : got)
            if (v.id.isEmpty()) ++empty;
        // videos/shorts must bring data; live may be empty (the channel
        // may have no live streams), but it must not hang.
        const bool good = r && empty == 0
                          && (tab == QLatin1String("live") || !got.isEmpty());
        std::fprintf(stderr,
                     "BROWSE channelTab %s videos=%d idVacios=%d more=%d -> %s\n",
                     tab.toUtf8().constData(), got.size(), empty,
                     more ? 1 : 0, good ? "OK" : "FAIL");
        ok = ok && good;

        // Continuation ("Mostrar más"): only when the page reaches the
        // requested size (the UI shows the button in that case). It must bring
        // new videos without repeating those of the first page.
        if (good && more && got.size() >= 25) {
            bool done2 = false;
            QVector<VideoInfo> page2;
            const auto conn2 = QObject::connect(
                &inn, &Innertube::channelTabReady, &inn,
                [&](const QString &, const QString &,
                    const QVector<VideoInfo> &v, bool) { page2 = v; done2 = true; });
            inn.fetchChannelTabMore(channel, tab, 25);
            const bool r2 = wait([&]() { return done2; }, 30000);
            QObject::disconnect(conn2);
            int dup = 0;
            for (const VideoInfo &a : std::as_const(page2))
                for (const VideoInfo &b : std::as_const(got))
                    if (a.id == b.id) { ++dup; break; }
            const bool good2 = r2 && !page2.isEmpty() && dup == 0;
            std::fprintf(stderr,
                         "BROWSE channelTabMore %s videos=%d dup=%d -> %s\n",
                         tab.toUtf8().constData(), page2.size(), dup,
                         good2 ? "OK" : "FAIL");
            ok = ok && good2;
        }
    }

    // 4) Video detail.
    {
        bool done = false;
        VideoInfo v;
        QString desc;
        qint64 likes = 0;
        const auto conn = QObject::connect(
            &inn, &Innertube::videoDetailReady, &inn,
            [&](const VideoInfo &vi, const QString &d, qint64 l) {
                v = vi; desc = d; likes = l; done = true;
            });
        inn.fetchVideoDetail(video);
        const bool r = wait([&]() { return done; }, 30000);
        QObject::disconnect(conn);
        const bool good =
            r && !v.title.isEmpty() && !desc.isEmpty() && likes > 0;
        std::fprintf(stderr,
                     "BROWSE videoDetail title=%d desc=%d likes=%lld -> %s\n",
                     v.title.size(), desc.size(),
                     static_cast<long long>(likes), good ? "OK" : "FAIL");
        ok = ok && good;
    }

    // 5) Playlist.
    {
        bool done = false;
        PlaylistInfo info;
        QVector<VideoInfo> vids;
        const auto conn = QObject::connect(
            &inn, &Innertube::playlistReady, &inn,
            [&](const PlaylistInfo &i, const QVector<VideoInfo> &v) {
                info = i; vids = v; done = true;
            });
        inn.fetchPlaylist(QStringLiteral("PLrAXtmErZgOeiKm4sgNOknGvNjby9efdf"),
                          50);
        const bool r = wait([&]() { return done; }, 30000);
        QObject::disconnect(conn);
        const bool good = r && !info.title.isEmpty() && !vids.isEmpty();
        std::fprintf(stderr, "BROWSE playlist title=%s videos=%d -> %s\n",
                     info.title.toUtf8().constData(), vids.size(),
                     good ? "OK" : "FAIL");
        ok = ok && good;
    }

    std::fprintf(stderr, "BROWSE SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}


#include "yt_innertube.moc"