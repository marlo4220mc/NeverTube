#include "yt_innertube.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QQueue>
#include <QSslError>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>
#include <QList>

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
const char kNextEndpoint[] =
    "/youtubei/v1/next?prettyPrint=false";

// UA del fork de youtube_explode_dart usado para /sw.js_data.
const char kChromeUA[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/132.0.0.0 Safari/537.36";

// UA de cliente VISIONOS (coincide con el del body player). Valores tomados
// de yt-dlp INNERTUBE_CLIENTS y NewPipeExtractor ClientsConstants.
const char kVisionOSUA[] =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) "
    "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";

// UA del cliente IOS (com.google.ios.youtube).
const char kIOSUA[] =
    "com.google.ios.youtube/21.26.4 "
    "(iPhone16,2; U; CPU iOS 18_3_2 like Mac OS X;)";

// UA del cliente ANDROID_VR (Oculus/Quest).
const char kAndroidVRUA[] =
    "com.google.android.apps.youtube.vr.oculus/1.65.10 "
    "(Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip";

const char kFallbackVisitorData[] =
    "CgttN24wcmd5UzNSWSi2lvq2BjIKCgJKUBIEGgAgYQ%3D%3D";

// Timeouts por fase del transporte Ipv4Https (ms). El fallback a yt-dlp y la
// regeneración de visitorData no deben esperar el timeout largo de player.
constexpr int kVisitorTimeoutMs = 10000;
constexpr int kPlayerTimeoutMs = 12000;
constexpr int kMasterTimeoutMs = 15000;

// Cascada de clientes para /player, en orden de preferencia. VISIONOS es el
// primario (único que hoy no requiere poToken y devuelve hlsManifestUrl en
// VODs); los demás son el fallback estilo NewPipe/yt-dlp para vídeos que
// exigen login (edad/miembros) o que no responden en VISIONOS. Los valores
// (versión actual de 2026, device/os/UA) vienen de yt-dlp INNERTUBE_CLIENTS
// y de NewPipeExtractor ClientsConstants, no son inventados.
struct PlayerClient
{
    const char *name;
    const char *version;
    int clientId; // X-Youtube-Client-Name numérico
    const char *ua;
    const char *deviceMake; // nullptr si no aplica
    const char *deviceModel;
    const char *osName;
    const char *osVersion;
    int androidSdkVersion; // 0 si no aplica
    bool embed;            // requiere thirdParty.embedUrl + Referer
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

// Contexto de cliente genérico para la cascada, construido a partir de la
// tabla kPlayerClients. Cada cliente aporta sus campos device/os/UA propios
// (valores reales de yt-dlp/NewPipe). "hl"/"gl" se fijan en "en"/"US" y
// todos los clientes llevan visitorData.
QJsonObject clientContext(const PlayerClient &pc, const QString &visitorData)
{
    QJsonObject client{
        {QStringLiteral("clientName"), QString::fromLatin1(pc.name)},
        {QStringLiteral("clientVersion"), QString::fromLatin1(pc.version)},
        {QStringLiteral("hl"), QStringLiteral("en")},
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

    // GET a una URL HTTPS absoluta (p.ej. el master HLS en
    // manifest.googlevideo.com). Reutiliza el mismo transporte IPv4.
    // highPriority=true adelanta la petición al frente de la cola: el master
    // es imprescindible para arrancar el vídeo y no debe esperar detrás de la
    // siembra radio/next (que no bloquea la reproducción).
    void getUrl(const QUrl &url, const char *ua, bool highPriority = false)
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
        // Tiempo de espera de la PRÓXIMA petición que se encuele (cada
        // petición lleva el suyo: el transporte es FIFO).
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
        // Cada petición captura SUS callbacks y timeout al encolarse: si otra
        // fase (player/next/radio/master) ya está usando el socket, la nueva
        // espera en la cola FIFO en vez de abortar la conexión en curso.
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

void Innertube::fetchHls(const QString &videoId, int preferredClient)
{
    // Caché de sesión: si ya hay una hlsManifestUrl firmada y vigente, se
    // entrega al instante sin repetir el POST player.
    if (serveCachedHls(videoId)) {
        NT_DEBUG("E2E innertube hls caché hit id=%s\n",
                 videoId.toUtf8().constData());
        return;
    }
    // Single-flight: si ya hay una petición player en curso para este
    // videoId (p.ej. dos cambios de resolución seguidos), no lanzar otra
    // HTTP. La petición en curso emitirá hlsReady y los guards de Yt filtran
    // por gen/propósito y la reciben.
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
    m_clientIndex = 0;
    if (preferredClient >= 0 && preferredClient < kPlayerClientCount)
        m_clientIndex = preferredClient;
    m_hlsStartMs = QDateTime::currentMSecsSinceEpoch();
    NT_DEBUG("E2E innertube fetchHls id=%s client=%s\n",
             videoId.toUtf8().constData(), kPlayerClients[m_clientIndex].name);
    if (m_visitorData.isEmpty()
        || QDateTime::currentSecsSinceEpoch() >= m_visitorExpiresAt) {
        m_playerPending = true;
        fetchVisitorData();
    } else
        requestPlayer();
}

void Innertube::fetchMaster(const QString &videoId, const QString &masterUrl)
{
    // Caché de sesión del cuerpo del master (mismo videoId y firma vigente).
    if (serveCachedMaster(videoId)) {
        NT_DEBUG("E2E innertube master caché hit id=%s\n",
                 videoId.toUtf8().constData());
        return;
    }
    // Single-flight: si otra petición del mismo videoId está descargando el
    // master, esperar su masterReady en vez de lanzar un segundo GET.
    if (mMasterInFlight && m_flightVideoId == videoId) {
        NT_DEBUG("E2E innertube master single-flight id=%s\n",
                 videoId.toUtf8().constData());
        return;
    }
    if (videoId != m_videoId) {
        // No es el videoId de la petición hls en curso: no se puede servir.
        fail(QStringLiteral("fetchMaster para id distinto del actual"));
        return;
    }
    const QUrl url(masterUrl);
    if (!url.isValid() || url.scheme() != QLatin1String("https")) {
        fail(QStringLiteral("master URL inválida"));
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
    // High priority: el master es imprescindible para arrancar el vídeo; se
    // adelanta a la siembra radio/next ya encolada (que no bloquea).
    m_http->getUrl(url, kVisionOSUA, /*highPriority=*/true);
}

void Innertube::fetchVisitorData()
{
    // El flujo documentado (AGENTS.md) es sw.js_data -> visitorData. La
    // homepage (/) devuelve un body vacío (JS challenge) y no sirve.
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
        NT_DEBUG("E2E innertube visitorData sw.js_data len=%d\n",
                 visitorData.size());
        emit visitorDataReady(m_visitorData, m_visitorExpiresAt);
        // Despachar TODAS las fases que esperaban visitorData (el player
        // primero para arrancar el vídeo cuanto antes), no solo una.
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
    NT_DEBUG("E2E innertube visitorData fixed fallback len=%d\n",
             m_visitorData.size());
    dispatchPending();
}

// Dispatcher tras refrescar visitorData: lanza las fases player/next/radio
// que quedaron esperando, con prioridad al player. El transporte FIFO
// serializa los POSTs sin abortarlos entre sí.
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
    if (m_visitorData.isEmpty()
        || QDateTime::currentSecsSinceEpoch() >= m_visitorExpiresAt) {
        m_nextPending = true;
        fetchVisitorData();
        return;
    }
    requestNext();
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
    const QJsonObject client = clientContext(pc, m_visitorData);
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
    // Cascada de clientes estilo NewPipe: probar el siguiente cliente antes
    // de abandonar con una respuesta vacía.
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
    QJsonParseError err;
    QByteArray json = body.trimmed();
    if (json.startsWith(")]}'"))
        json.remove(0, 4);
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        NT_DEBUG("E2E innertube next json fail size=%d prefix=%s\n", body.size(),
                 body.left(120).constData());
        if (tryNextClient())
            return;
        emit relatedReady(videoId, m_bestNext);
        m_bestNext.clear();
        return;
    }
    const QVector<VideoInfo> related = parseRelated(doc.object());
    NT_DEBUG("E2E innertube next ok id=%s relacionados=%d\n",
             videoId.toUtf8().constData(), related.size());
    // Seleccionar la mejor respuesta: quedarse con la lista más larga y seguir
    // probando clientes mientras no llegue a un "A continuación" razonable.
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
    m_radioClientIndex = 0;
    NT_DEBUG("E2E innertube fetchRadio id=%s\n", videoId.toUtf8().constData());
    if (m_visitorData.isEmpty()
        || QDateTime::currentSecsSinceEpoch() >= m_visitorExpiresAt) {
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
    const QJsonObject client = clientContext(pc, m_visitorData);
    QJsonObject context{{QStringLiteral("client"), client}};
    if (pc.embed) {
        context.insert(QStringLiteral("thirdParty"),
                       QJsonObject{{QStringLiteral("embedUrl"),
                                    QString::fromLatin1(pc.embedUrl)}});
    }

    QJsonObject body;
    if (m_radioContinuation.isEmpty()) {
        // Primer batch: el mix RDAMVM<videoId> (radio) de YouTube.
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
        // Batch siguiente del mismo mix, por token de continuación.
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
    // Cascada de clientes solo en el primer batch: el token de continuación
    // es específico del cliente que lo emitió.
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
    QJsonParseError err;
    QByteArray json = body.trimmed();
    if (json.startsWith(")]}'"))
        json.remove(0, 4);
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        NT_DEBUG("E2E innertube radio json fail size=%d prefix=%s\n",
                 body.size(), body.left(120).constData());
        if (tryNextClient())
            return;
        emit radioReady(videoId, QVector<VideoInfo>());
        return;
    }

    QVector<VideoInfo> videos;
    QString continuation;
    parseWatchQueue(doc.object(), &videos, &continuation);
    if (videos.isEmpty()) {
        // El mix RDAMVM solo trae panel (playlistPanelVideoRenderer) para
        // vídeos con edición de música; para el resto el /next responde la
        // mezcla como recomendaciones (compactVideoRenderer). Se guarda la
        // primera mezcla vista y se sigue cascadeando por si otro cliente
        // trae el panel real; si ninguno lo trae, se emite la mezcla.
        const QVector<VideoInfo> fb = parseRelated(doc.object());
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

// Cola "watch playlist" del mix RDAMVM: playlistPanelVideoRenderer de la
// respuesta /next. Extrae el token de continuación (nextRadioContinuationData
// / nextContinuationData / continuationItemRenderer) si el mix tiene más
// batches. Soporta twoColumnWatchNextResults y el renderizado de una sola
// columna (musicQueueRenderer del cliente de música).
void Innertube::parseWatchQueue(const QJsonObject &root, QVector<VideoInfo> *out,
                                QString *continuation) const
{
    const QJsonObject contents =
        root.value(QStringLiteral("contents")).toObject();

    auto textOf = [](const QJsonValue &val) {
        const QJsonObject o = val.toObject();
        const QJsonValue simple = o.value(QStringLiteral("simpleText"));
        if (simple.isString())
            return simple.toString();
        const QJsonArray runs = o.value(QStringLiteral("runs")).toArray();
        QString t;
        for (const QJsonValue &r : runs)
            t += r.toObject().value(QStringLiteral("text")).toString();
        return t;
    };

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
            v.title = textOf(pvr.value(QStringLiteral("title")));
            v.channel = textOf(pvr.value(QStringLiteral("longBylineText")));
            const QJsonArray thumbs = pvr.value(QStringLiteral("thumbnail"))
                                          .toObject()
                                          .value(QStringLiteral("thumbnails"))
                                          .toArray();
            if (!thumbs.isEmpty())
                v.thumbnailUrl = thumbs.last().toObject()
                                     .value(QStringLiteral("url")).toString();
            QString durationText =
                textOf(pvr.value(QStringLiteral("lengthText")));
            if (durationText.isEmpty()) {
                const QJsonArray overlays =
                    pvr.value(QStringLiteral("thumbnailOverlays")).toArray();
                for (const QJsonValue &over : overlays) {
                    durationText = textOf(
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
            v.viewCount =
                textOf(pvr.value(QStringLiteral("viewCountText"))).toInt();
            out->append(v);
        }
    };

    auto handlePanel = [&](const QJsonObject &panel) {
        parsePanelContents(panel.value(QStringLiteral("contents")).toArray());
        grabContinuation(panel);
    };

    // 1) Layout de dos columnas: la cola del mix vive en playlist.playlist.
    const QJsonObject tw =
        contents.value(QStringLiteral("twoColumnWatchNextResults")).toObject();
    handlePanel(tw.value(QStringLiteral("playlist")).toObject()
                    .value(QStringLiteral("playlist")).toObject());

    // 1b) Layout de una columna de VISIONOS/ANDROID_VR: el mix también está
    //     en playlist.playlist (contenidos playlistPanelVideoRenderer).
    const QJsonObject scw =
        contents.value(QStringLiteral("singleColumnWatchNextResults")).toObject();
    handlePanel(scw.value(QStringLiteral("playlist")).toObject()
                    .value(QStringLiteral("playlist")).toObject());

    // 2) Cliente de música (singleColumnMusicWatchNextResultsRenderer): la
    //    cola está en musicQueueRenderer → playlistPanelRenderer.
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

    // Texto de un nodo de texto (runs o simpleText).
    auto textOf = [](const QJsonValue &val) {
        const QJsonObject o = val.toObject();
        const QJsonValue simple = o.value(QStringLiteral("simpleText"));
        if (simple.isString())
            return simple.toString();
        const QJsonArray runs = o.value(QStringLiteral("runs")).toArray();
        QString t;
        for (const QJsonValue &r : runs)
            t += r.toObject().value(QStringLiteral("text")).toString();
        return t;
    };

    auto durationSecondsOf = [textOf](const QJsonArray &overlays) {
        for (const QJsonValue &val : overlays) {
            const QJsonObject o = val.toObject();
            if (!o.contains(QStringLiteral("thumbnailOverlayTimeStatusRenderer")))
                continue;
            const QString t = textOf(o.value(
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
        QString t = s;
        t.remove(QLatin1Char('.'));
        t.remove(QLatin1Char(','));
        t.remove(QLatin1Char(' '));
        QLocale locale;
        if (t.contains(QLatin1Char('M'))) {
            t.remove(QLatin1Char('M'));
            return qRound(t.toDouble() * 1e6);
        }
        if (t.contains(QLatin1Char('K'))) {
            t.remove(QLatin1Char('K'));
            return qRound(t.toDouble() * 1e3);
        }
        bool ok = false;
        const qlonglong n = t.toLongLong(&ok);
        return ok ? static_cast<int>(n) : 0;
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
        v.title = textOf(r->value(QStringLiteral("title")));
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
            v.channel = textOf(r->value(QStringLiteral("longBylineText")));
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
        v.viewCount = viewCountOf(textOf(r->value(QStringLiteral("viewCountText"))));
        out.append(v);
    };

    // Recorre cualquier contenedor de resultados (results y secondaryResults),
    // bajando por richItemRenderer/itemSectionRenderer.
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
    // Clientes de una sola columna (VISIONOS/ANDROID): la lista completa de
    // recomendaciones vive en watchNextTabbedResultsRenderer → tabs[]
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
    // Clientes de una sola columna (VISIONOS): la cola de auto-reproducción
    // vive en autoplay.autoplay.sets[].autoplayVideoRenderer.compactVideoRenderer.
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

    // Dedupe por id (un mismo recomendado puede aparecer en ambos contenedores).
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
        m_hlsInFlight = true; // asegurar el single-flight si se entró vía visitor
    m_http->setResult([this](int status, const QByteArray &body) {
        handlePlayer(status == 200, QString(), body);
    });
    m_http->setError([this](const QString &reason) {
        handlePlayer(false, reason, QByteArray());
    });
    m_http->setTimeoutMs(kPlayerTimeoutMs);
    m_http->setVisitorId(m_visitorData);

    const PlayerClient &pc = kPlayerClients[m_clientIndex];
    const QJsonObject client = clientContext(pc, m_visitorData);
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

    // X-Goog-Visitor-Id se añade al request dinámicamente; lo insertamos
    // directamente en las cabeceras por estado.
    m_http->post(kPlayerEndpoint, json, pc.ua, pc.clientId, pc.version,
                 pc.embed ? pc.embedUrl : nullptr);
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
    const QString reason = playability.value(QStringLiteral("reason")).toString();
    if (status != QLatin1String("OK")) {
        if (status == QLatin1String("LOGIN_REQUIRED") && !m_retryingVisitor) {
            m_retryingVisitor = true;
            m_visitorData.clear();
            m_visitorExpiresAt = 0;
            // El visitor Data regenerado invalida las URLs firmadas previas
            // para este vídeo (podrían quedar ligadas al token anterior).
            m_hlsCache.remove(m_videoId);
            NT_DEBUG("E2E innertube regenerating visitorData after "
                     "LOGIN_REQUIRED\n");
            m_playerPending = true;
            fetchVisitorData();
            return;
        }
        // Cascada de clientes estilo NewPipe: agotado el cliente actual o el
        // intento con un visitorData fresco, probar el siguiente cliente antes
        // de abandonar.
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

    // Respuesta falseada estilo "no disponible en esta app" (la detecta
    // NewPipe) o rate-limit (Piped): el videoId devuelto no es el pedido.
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
        fail(QStringLiteral("respuesta player no válida (videoId distinto)"));
        return;
    }

    const QJsonObject streamingData =
        root.value(QStringLiteral("streamingData")).toObject();
    const QString hls =
        streamingData.value(QStringLiteral("hlsManifestUrl")).toString();
    if (hls.isEmpty()) {
        // En VODs solo los clientes Apple devuelven hlsManifestUrl; agotar la
        // cascada antes de fallar.
        NT_DEBUG("E2E innertube client %s sin hlsManifestUrl\n",
                 kPlayerClients[m_clientIndex].name);
        if (m_clientIndex + 1 < kPlayerClientCount) {
            ++m_clientIndex;
            m_retryingVisitor = false;
            requestPlayer();
            return;
        }
        fail(QStringLiteral("player OK pero sin hlsManifestUrl"));
        return;
    }
    const QVector<int> heights = videoHeights(streamingData);
    m_lastWinClient = m_clientIndex;
    const qint64 elapsed =
        QDateTime::currentMSecsSinceEpoch() - m_hlsStartMs;
    NT_DEBUG("E2E innertube hls ok len=%d alturas=%d client=%s (%lldms)\n",
             hls.size(), heights.size(), kPlayerClients[m_clientIndex].name,
             static_cast<long long>(elapsed));
    // Caché de sesión: guarda hlsManifestUrl firmada y su expiración para
    // no repetir el POST player en cambios de resolución del mismo vídeo.
    const qint64 exp = expireFromUrl(hls);
    m_hlsCache.insert(m_videoId, HlsCacheEntry{hls, QByteArray(), heights, exp,
                                               m_clientIndex});
    m_hlsInFlight = false;
    mMasterInFlight = false;
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
        fail(QStringLiteral("master vacío"));
        return;
    }
    NT_DEBUG("E2E innertube master ok len=%d (%lldms)\n", body.size(),
             static_cast<long long>(elapsed));
    // Guarda el cuerpo del master en la caché del videoId para evitar
    // re-descargarlo en cambios de resolución.
    auto it = m_hlsCache.find(m_videoId);
    if (it != m_hlsCache.end())
        it->masterBody = body;
    else
        m_hlsCache.insert(m_videoId,
                          HlsCacheEntry{QString(), body, QVector<int>(), 0,
                                        m_clientIndex});
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
        return false; // la petición en curso se resolverá sola
    if (m_http->isBusy())
        return false; // transporte ocupado (POST de otro vídeo): no emitir
                      // señales que dispararían un GET real y lo abortaría
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
    m_videoId = videoId;
    emit hlsReady(videoId, it->hlsUrl, it->heights);
    return true;
}

bool Innertube::serveCachedMaster(const QString &videoId)
{
    if (mMasterInFlight && m_flightVideoId == videoId)
        return false;
    if (m_http->isBusy())
        return false; // ver serveCachedHls
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
    // La URL firmada incluye /expire/<timestamp unix>/. Si no aparece (o no
    // es numérico) devolvemos 0 → la caché se considera caducada y se
    // descarta en la siguiente consulta.
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
                                      "SELFTEST OK (visitorData→player→hls+alturas) "
                                      "id=%s hls=%s alturas=%d → next\n",
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
                                      "(1º: %s) → radio\n",
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

#include "yt_innertube.moc"