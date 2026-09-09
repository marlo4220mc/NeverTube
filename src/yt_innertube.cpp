#include "yt_innertube.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslError>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <algorithm>
#include <functional>
#include <utility>

#include "debug.h"

namespace {

const char kYoutubeHost[] = "https://www.youtube.com";
const char kYoutubeName[] = "www.youtube.com";
const char kSwJsData[] = "/sw.js_data";
const char kPlayerEndpoint[] =
    "/youtubei/v1/player?prettyPrint=false";

// UA del fork de youtube_explode_dart usado para /sw.js_data.
const char kChromeUA[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/132.0.0.0 Safari/537.36";

// UA de cliente VISIONOS (coincide con el del body player).
const char kVisionOSUA[] =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) "
    "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";

const char kFieldMask[] =
    "playabilityStatus.status,playabilityStatus.reason,"
    "streamingData.hlsManifestUrl,streamingData.adaptiveFormats,"
    "videoDetails.videoId";

// Alturas de vídeo únicas y ascendentes extraídas de streamingData.
// Solo se tienen en cuenta entradas de vídeo (con width/height > 0);
// los itags de solo audio (139/140/249/…) se ignoran.
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

QJsonObject visionOsClient(const QString &visitorData)
{
    return QJsonObject{
        {QStringLiteral("clientName"), QStringLiteral("VISIONOS")},
        {QStringLiteral("clientVersion"), QStringLiteral("1.02")},
        {QStringLiteral("deviceMake"), QStringLiteral("Apple")},
        {QStringLiteral("deviceModel"), QStringLiteral("RealityDevice17,1")},
        {QStringLiteral("userAgent"), QString::fromLatin1(kVisionOSUA)},
        {QStringLiteral("osName"), QStringLiteral("visionOS")},
        {QStringLiteral("osVersion"), QStringLiteral("26.5.23O471")},
        {QStringLiteral("hl"), QStringLiteral("en")},
        {QStringLiteral("visitorData"), visitorData},
    };
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

} // namespace

// Cliente HTTP/HTTPS mínimo que fuerza conexión por IPv4 manteniendo SNI y
// validación de certificado contra www.youtube.com (QSslSocket usa
// setPeerVerifyName para ambos). YouTube bloquea por IPv6 ciertos videos
// desde esta red; por IPv4 responden sin gate anti-bot.
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

    // GET a una URL HTTPS absoluta (p.ej. el master HLS en
    // manifest.googlevideo.com). Reutiliza el mismo transporte IPv4.
    void getUrl(const QUrl &url, const char *ua)
    {
        HttpRequest req;
        req.verb = QByteArrayLiteral("GET");
        const QString host = url.host();
        QString path = url.path();
        if (url.hasQuery())
            path += QLatin1Char('?') + url.query();
        req.host = host.toUtf8();
        req.path = path.toUtf8();
        req.headers = QList<QPair<QByteArray, QByteArray>>{
            {QByteArrayLiteral("Accept-Encoding"), QByteArrayLiteral("identity")},
            {QByteArrayLiteral("Connection"), QByteArrayLiteral("close")},
            {QByteArrayLiteral("Host"), req.host},
            {QByteArrayLiteral("User-Agent"), QByteArray(ua)},
        };
        start(req);
    }

    void post(const char *path, const QByteArray &jsonBody, const char *ua)
    {
        HttpRequest req;
        req.verb = QByteArrayLiteral("POST");
        req.path = QByteArray(path);
        req.body = jsonBody;
        QList<QPair<QByteArray, QByteArray>> headers{
            {QByteArrayLiteral("Accept-Encoding"), QByteArrayLiteral("identity")},
            {QByteArrayLiteral("Connection"), QByteArrayLiteral("close")},
            {QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")},
            {QByteArrayLiteral("Host"), QByteArray(kYoutubeName)},
            {QByteArrayLiteral("Origin"), QByteArrayLiteral("https://www.youtube.com")},
            {QByteArrayLiteral("Sec-Fetch-Mode"), QByteArrayLiteral("navigate")},
            {QByteArrayLiteral("User-Agent"), QByteArray(ua)},
            {QByteArrayLiteral("X-Goog-FieldMask"), QByteArray(kFieldMask)},
            {QByteArrayLiteral("X-Youtube-Client-Name"), QByteArrayLiteral("VISIONOS")},
            {QByteArrayLiteral("X-Youtube-Client-Version"), QByteArrayLiteral("1.02")},
            {QByteArrayLiteral("Content-Length"), QByteArray::number(req.body.size())},
        };
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
    };

    void start(const HttpRequest &req)
    {
        m_request = req;
        m_response.clear();
        m_responseHeaderSize = -1;
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

        // Resolver el host de destino a una dirección IPv4.
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
        auto fn = std::move(m_result);
        reset(nullptr);
        if (fn)
            fn(status, body);
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

    // Decodifica un cuerpo transfer-encoding: chunked (el que YouTube usa en
    // sw.js_data y youtubei/v1/player).
    static QByteArray dechunkBody(const QByteArray &raw)
    {
        QByteArray out;
        int pos = 0;
        while (pos < raw.size()) {
            const int eol = raw.indexOf('\n', pos);
            if (eol < 0)
                break;
            int sizeEnd = eol;
            const int ext = raw.indexOf(';', pos); // "1a;ext" opcional
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
            pos = start + size + 2; // CRLF tras el bloque
        }
        return out;
    }

    void abortWithError(const QString &reason)
    {
        auto fn = std::move(m_error);
        reset(nullptr);
        if (fn)
            fn(reason);
    }

    void reset(QSslSocket *newSocket)
    {
        m_timeout->stop();
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
};

// ---------------------------------------------------------------------------

Innertube::Innertube(QObject *parent)
    : QObject(parent)
    , m_http(new Ipv4Https(this))
{
}

Innertube::~Innertube()
{
    m_http->abort();
}

bool Innertube::isBusy() const
{
    return m_http->isBusy();
}

void Innertube::fetchHls(const QString &videoId)
{
    m_videoId = videoId;
    NT_DEBUG("E2E innertube fetchHls id=%s\n", videoId.toUtf8().constData());
    if (m_visitorData.isEmpty())
        fetchVisitorData();
    else
        requestPlayer();
}

void Innertube::fetchMaster(const QString &videoId, const QString &masterUrl)
{
    if (videoId != m_videoId) {
        fail(QStringLiteral("fetchMaster para id distinto del actual"));
        return;
    }
    const QUrl url(masterUrl);
    if (!url.isValid() || url.scheme() != QLatin1String("https")) {
        fail(QStringLiteral("master URL inválida"));
        return;
    }
    NT_DEBUG("E2E innertube fetchMaster id=%s\n", videoId.toUtf8().constData());
    m_http->setResult([this](int status, const QByteArray &body) {
        handleMaster(status == 200, QString(), body);
    });
    m_http->setError([this](const QString &reason) {
        handleMaster(false, reason, QByteArray());
    });
    m_http->getUrl(url, kVisionOSUA);
}

void Innertube::fetchVisitorData()
{
    m_http->setResult([this](int status, const QByteArray &body) {
        handleVisitorData(status == 200, QString(), body);
    });
    m_http->setError([this](const QString &reason) {
        handleVisitorData(false, reason, QByteArray());
    });
    m_http->get(kSwJsData, kChromeUA);
}

void Innertube::requestPlayer()
{
    m_http->setResult([this](int status, const QByteArray &body) {
        handlePlayer(status == 200, QString(), body);
    });
    m_http->setError([this](const QString &reason) {
        handlePlayer(false, reason, QByteArray());
    });
    m_http->setVisitorId(m_visitorData);

    const QJsonObject body{
        {QStringLiteral("context"),
         QJsonObject{{QStringLiteral("client"), visionOsClient(m_visitorData)}}},
        {QStringLiteral("videoId"), m_videoId},
        {QStringLiteral("contentCheckOk"), true},
        {QStringLiteral("racyCheckOk"), true},
    };
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);

    // X-Goog-Visitor-Id se añade al request dinámicamente; lo insertamos
    // directamente en las cabeceras por estado.
    m_http->post(kPlayerEndpoint, json, kVisionOSUA);
}

void Innertube::handleVisitorData(bool ok, const QString &error, const QByteArray &body)
{
    if (!ok) {
        fail(error.isEmpty() ? QStringLiteral("sw.js_data HTTP err") : error);
        return;
    }
    const QString visitorData = visitorDataFromSwJsData(body);
    if (visitorData.isEmpty()) {
        fail(QStringLiteral("no se pudo extraer visitorData de sw.js_data"));
        return;
    }
    m_visitorData = visitorData;
    NT_DEBUG("E2E innertube visitorData ok len=%d\n", visitorData.size());
    requestPlayer();
}

void Innertube::handlePlayer(bool ok, const QString &error, const QByteArray &body)
{
    if (!ok) {
        fail(error.isEmpty() ? QStringLiteral("player HTTP err") : error);
        return;
    }
    QJsonParseError err;
    QByteArray json = body.trimmed();
    if (json.startsWith(")]}'"))
        json.remove(0, 4);
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        NT_DEBUG("E2E innertube player json fail err=%s size=%d prefix=%s\n",
                 err.errorString().toUtf8().constData(), body.size(),
                 body.left(120).constData());
        fail(QStringLiteral("respuesta player no es JSON válido"));
        return;
    }
    const QJsonObject root = doc.object();
    const QJsonObject playability =
        root.value(QStringLiteral("playabilityStatus")).toObject();
    const QString status = playability.value(QStringLiteral("status")).toString();
    if (status != QLatin1String("OK")) {
        const QString reason =
            playability.value(QStringLiteral("reason")).toString();
        fail(reason.isEmpty()
                 ? QStringLiteral("player status=%1").arg(status)
                 : QStringLiteral("player status=%1: %2").arg(status, reason));
        return;
    }
    const QJsonObject streamingData =
        root.value(QStringLiteral("streamingData")).toObject();
    const QString hls =
        streamingData.value(QStringLiteral("hlsManifestUrl")).toString();
    if (hls.isEmpty()) {
        fail(QStringLiteral("player OK pero sin hlsManifestUrl"));
        return;
    }
    const QVector<int> heights = videoHeights(streamingData);
    NT_DEBUG("E2E innertube hls ok len=%d alturas=%d\n", hls.size(),
             heights.size());
    emit hlsReady(m_videoId, hls, heights);
}

void Innertube::handleMaster(bool ok, const QString &error,
                             const QByteArray &body)
{
    if (!ok) {
        fail(error.isEmpty() ? QStringLiteral("master HTTP err") : error);
        return;
    }
    if (body.isEmpty()) {
        fail(QStringLiteral("master vacío"));
        return;
    }
    NT_DEBUG("E2E innertube master ok len=%d\n", body.size());
    emit masterReady(m_videoId, body);
}

void Innertube::fail(const QString &reason)
{
    const QString id = m_videoId;
    NT_DEBUG("E2E innertube failed id=%s reason=%s\n", id.toUtf8().constData(),
             reason.toUtf8().constData());
    emit failed(id, reason);
}

bool innertubeSelfTest()
{
    const QString videoId = QStringLiteral("dQw4w9WgXcQ");
    Innertube tube;
    bool success = false;

    auto *timeout = new QTimer;
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, [&success]() {
        std::fprintf(stderr, "SELFTEST FAIL: innertube timeout sin HLS\n");
        QCoreApplication::exit(1);
    });

    QObject::connect(&tube, &Innertube::hlsReady,
                     [&success, videoId](const QString &vid,
                                         const QString &hlsUrl,
                                         const QVector<int> &heights) {
                         success = vid == videoId &&
                                   hlsUrl.startsWith(QLatin1String("https://")) &&
                                   !heights.isEmpty();
                         if (!success) {
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
                                      "SELFTEST OK (visitorData→player→hls+alturas) "
                                      "id=%s hls=%s alturas=%d\n",
                                      vid.toUtf8().constData(),
                                      hlsUrl.toUtf8().constData(),
                                      heights.size());
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

#include "yt_innertube.moc"