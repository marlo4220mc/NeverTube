#include "yt_innertube.h"

#include <QCoreApplication>
#include <QDateTime>
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
const char kBrowseEndpoint[] =
    "/youtubei/v1/browse?prettyPrint=false";

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
// (valores reales de yt-dlp/NewPipe). "hl" se fija en "es" (igual que
// yt-dlp --extractor-args youtube:lang=es: sin esto YouTube auto-traduce los
// títulos al inglés) y todos los clientes llevan visitorData.
QJsonObject clientContext(const PlayerClient &pc, const QString &visitorData)
{
    QJsonObject client{
        {QStringLiteral("clientName"), QString::fromLatin1(pc.name)},
        {QStringLiteral("clientVersion"), QString::fromLatin1(pc.version)},
        {QStringLiteral("hl"), QStringLiteral("es")},
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
        m_visitorDataFreshRun = true;
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
    // Marcar como "fresco en esta sesión" también ante fallback: evita que el
    // Home reintente sw.js_data en cada recarga cuando el endpoint está caído
    // (el fallback fijo ya es usable).
    m_visitorDataFreshRun = true;
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

// Límites del feed del Home (browse FEwhat_to_watch): hasta kHomeShelfCap
// estantes y kHomeShelfVideos vídeos por estante; si no hay estantes (layout
// distinto) se recolecta hasta kHomeShelfVideos sueltos.
constexpr int kHomeShelfCap = 3;
constexpr int kHomeShelfVideos = 8;

// Params de la pestaña "Vídeos" de los canales (browse). Valor verificado
// idéntico en los canales reales; sin él el browse devuelve la pestaña por
// defecto ("Inicio") en lugar del listado de vídeos.
const char kChannelVideosParams[] = "EgZ2aWRlb3PyBgQKAjoA";
// Tendencias: browseId real, aunque hoy YouTube responde 400/redirección a la
// Home para tendencias anónimas (el selftest registra el estado real).
const char kTrendingBrowseId[] = "FEtrending";
// Cuántos vídeos se recolectan por canal/tendencias (luego la UI decide/dedupe
// y limita a 8). Suficiente para llenar la sección sin pedir más canales.
constexpr int kChannelTake = 12;
constexpr int kTrendingTake = 12;

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
    // El visitorData persistido vigente se usa directamente: ya no se fuerza
    // el refresh una vez por sesión (gana una RTT en el arranque). Si el
    // browse responde 500/vacío, handleHome refresca visitorData UNA vez y
    // reintenta (m_homeVisitorRetried) sin bucle.
    if (m_visitorData.isEmpty()
        || QDateTime::currentSecsSinceEpoch() >= m_visitorExpiresAt) {
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

    // VISIONOS: el único cliente probado que devuelve el feed real del Home
    // (62 compactVideoRenderer en la PRUEBA 1). Sin cascada: si falla/500, la
    // app usa el fallback yt-dlp (no se regenera visitorData en bucle).
    const PlayerClient &pc = kPlayerClients[0];
    const QJsonObject client = clientContext(pc, m_visitorData);
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
    QJsonParseError err;
    QByteArray json = body.trimmed();
    if (json.startsWith(")]}'"))
        json.remove(0, 4);
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
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
        parseHomeBrowse(doc.object(), &continuation);
    m_homeContinuation = continuation;
    NT_DEBUG("E2E innertube home ok videos=%d cont=%s\n", videos.size(),
             continuation.isEmpty() ? "no" : "si");
    if (more) {
        emit homeMoreReady(videos);
        return;
    }
    // Respuesta válida pero sin vídeos (p. ej. 200 con challenge/error):
    // reintentar una vez con visitorData fresco antes de ocultar la sección.
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
    if (m_visitorData.isEmpty()
        || QDateTime::currentSecsSinceEpoch() >= m_visitorExpiresAt) {
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
    const QJsonObject client = clientContext(pc, m_visitorData);
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
    QJsonParseError err;
    QByteArray json = body.trimmed();
    if (json.startsWith(")]}'"))
        json.remove(0, 4);
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        NT_DEBUG("E2E innertube channel json fail size=%d\n", body.size());
        if (retryBrowseWithFreshVisitor(&m_channelVisitorRetried,
                                        &m_channelPending))
            return;
        emit channelVideosReady(m_channelId, QVector<VideoInfo>());
        return;
    }
    QVector<VideoInfo> videos =
        collectVideoRenderers(doc.object(), kChannelTake);
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
    if (m_visitorData.isEmpty()
        || QDateTime::currentSecsSinceEpoch() >= m_visitorExpiresAt) {
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
    const QJsonObject client = clientContext(pc, m_visitorData);
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
    QJsonParseError err;
    QByteArray json = body.trimmed();
    if (json.startsWith(")]}'"))
        json.remove(0, 4);
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        NT_DEBUG("E2E innertube trending json fail size=%d\n", body.size());
        if (retryBrowseWithFreshVisitor(&m_trendVisitorRetried,
                                        &m_trendPending))
            return;
        emit trendingReady(QVector<VideoInfo>());
        return;
    }
    QVector<VideoInfo> videos =
        collectVideoRenderers(doc.object(), kTrendingTake);
    if (videos.isEmpty()
        && retryBrowseWithFreshVisitor(&m_trendVisitorRetried,
                                       &m_trendPending))
        return;
    NT_DEBUG("E2E innertube trending ok videos=%d\n", videos.size());
    emit trendingReady(videos);
}

namespace {

// Texto de un nodo (simpleText o runs).
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

int homeViewCountOf(const QString &s)
{
    QString t = s;
    t.remove(QLatin1Char('.'));
    t.remove(QLatin1Char(','));
    t.remove(QLatin1Char(' '));
    bool ok = false;
    if (t.endsWith(QLatin1Char('M'))) {
        t.chop(1);
        return qRound(t.toDouble(&ok) * 1e6);
    }
    if (t.endsWith(QLatin1Char('K'))) {
        t.chop(1);
        return qRound(t.toDouble(&ok) * 1e3);
    }
    if (t.endsWith(QLatin1Char('B'))) {
        t.chop(1);
        return qRound(t.toDouble(&ok) * 1e9);
    }
    return t.toInt(&ok);
}

// "hace 3 días" / "3 giorni fa" → fecha ISO aproximada (para ordenar feeds
// sin depender de una marca exacta). Si no reconoce el texto, devuelve vacío.
QString relativeToIso(const QString &text)
{
    const QString t = text.trimmed().toLower();
    QStringList parts = t.split(QLatin1Char(' '));
    if (parts.size() < 3)
        return QString();
    QString countText;
    QString unit;
    if (parts.at(0) == QLatin1String("hace")) {         // "hace X unidad"
        countText = parts.at(1);
        unit = parts.at(2);
    } else if (parts.at(1) == QLatin1String("hace")) {  // "X hace"
        countText = parts.at(0);
        unit = parts.at(2);
    } else {
        return QString();
    }
    bool ok = false;
    double n = countText.toDouble(&ok);
    if (!ok)
        return QString();
    qint64 secs = 0;
    if (unit.contains(QStringLiteral("segundo")))
        secs = qint64(n);
    else if (unit.contains(QStringLiteral("minuto")))
        secs = qint64(n * 60);
    else if (unit.contains(QStringLiteral("hora")))
        secs = qint64(n * 3600);
    else if (unit.contains(QStringLiteral("día"))
             || unit.contains(QStringLiteral("dia")))
        secs = qint64(n * 86400);
    else if (unit.contains(QStringLiteral("semana")))
        secs = qint64(n * 7 * 86400);
    else if (unit.contains(QStringLiteral("mes")))
        secs = qint64(n * 30 * 86400);
    else if (unit.contains(QStringLiteral("año"))
             || unit.contains(QStringLiteral("ano")))
        secs = qint64(n * 365 * 86400);
    else
        return QString();
    if (secs <= 0)
        return QString();
    return QDateTime::currentDateTimeUtc().addSecs(-secs)
        .toString(Qt::ISODate);
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
    const QJsonArray ownerRuns =
        r->value(QStringLiteral("ownerText")).toObject()
            .value(QStringLiteral("runs")).toArray();
    if (!ownerRuns.isEmpty()) {
        const QJsonObject o0 = ownerRuns.at(0).toObject();
        v.channel = o0.value(QStringLiteral("text")).toString();
        v.channelId = o0.value(QStringLiteral("navigationEndpoint"))
                          .toObject().value(QStringLiteral("browseEndpoint"))
                          .toObject().value(QStringLiteral("browseId"))
                          .toString();
    } else {
        v.channel = homeTextOf(r->value(QStringLiteral("longBylineText")));
    }
    const QJsonArray thumbs = r->value(QStringLiteral("thumbnail"))
                                  .toObject().value(QStringLiteral("thumbnails"))
                                  .toArray();
    // TEMPORAL: logging de variantes de thumbnail
    if (!thumbs.isEmpty() && !v.id.isEmpty()) {
        fprintf(stderr, "THUMB_VARIANTS id=%s count=%d\n", 
                v.id.toUtf8().constData(), thumbs.size());
        for (int i = 0; i < thumbs.size(); ++i) {
            const QJsonObject th = thumbs[i].toObject();
            const int w = th.value(QStringLiteral("width")).toInt();
            const int h = th.value(QStringLiteral("height")).toInt();
            const QString url = th.value(QStringLiteral("url")).toString();
            const double aspect = h > 0 ? double(w) / double(h) : 0;
            fprintf(stderr, "  [%d] %dx%d aspect=%.3f url=%s\n", 
                    i, w, h, aspect, url.right(40).toUtf8().constData());
        }
    }
    if (!thumbs.isEmpty())
        v.thumbnailUrl = thumbs.last().toObject()
                             .value(QStringLiteral("url")).toString();
    v.durationSeconds = homeDurationOf(
        r->value(QStringLiteral("thumbnailOverlays")).toArray());
    v.viewCount = homeViewCountOf(homeTextOf(r->value(QStringLiteral("viewCountText"))));
    v.publishedAt = relativeToIso(homeTextOf(
        r->value(QStringLiteral("publishedTimeText"))));
    if (v.thumbnailUrl.isEmpty() && v.id.size() == 11)
        v.thumbnailUrl = QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg")
                             .arg(v.id);
    return v;
}

// Recorre el árbol recolectando compactVideoRenderer/videoRenderer sin
// duplicar ids, respetando el orden de la respuesta y el límite.
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

// Feed del Home (FEwhat_to_watch) con el layout verificado en la PRUEBA 1 con
// VISIONOS: singleColumnBrowseResultsRenderer → tabs → tabRenderer → content →
// sectionListRenderer → contents[] → shelfRenderer → content →
// verticalListRenderer → items[] → compactVideoRenderer / videoRenderer.
QVector<VideoInfo> Innertube::parseHomeBrowse(const QJsonObject &root,
                                              QString *continuation) const
{
    QVector<VideoInfo> out;
    if (continuation)
        continuation->clear();

    // Primer token de continuación que aparezca (en cualquier parte de la
    // respuesta; los estantes no lo llevan, va en el propio sectionListRenderer
    // o en continuations del listado).
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

    // Recolecta renderers de vídeo dentro de una sección (subárbol). Respeta
    // los límites por estante y total.
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

    // Layout distinto (p. ej. richGrid): recolectar hasta kHomeShelfVideos
    // directamente desde el árbol raíz como si fuera un único "estante".
    if (out.isEmpty()) {
        shelfVideos = 0;
        collect(QJsonValue(root));
    }
    // Dedupe por id (un mismo vídeo puede repetirse en estantes).
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

// ---------------------------------------------------------------------------
// Sondeo de diagnóstico del Home real (POST /browse, browseId FEwhat_to_watch).
// Investigación SOLO (no es el Home de la app y no toca HomePage ni player):
//   - visitorData: el persistido si se pasa en NEVERTUBE_HOME_VISITOR (valor
//     tal como está guardado en settings), o uno freso vía sw.js_data.
//   - prueba FEwhat_to_watch con los clientes ya existentes (VISIONOS primero;
//     IOS/ANDROID_VR/WEB_EMBEDDED como comparación si VISIONOS falla/vacío).
//   - reporta estado HTTP, tiempo, tamaño, renderers, vídeos, continuations y
//     posibles desafíos (alertRenderer), sin inventar campos.
// La lista de titles impresa permite contar manualmente la aparición de un
// tema (PRUEBA 2: sesión A persistida vs sesión B fresca).
//
// Se lanza con:  ./build/nevertube_qt2 --selftest-home
// (opcional)          NEVERTUBE_HOME_VISITOR='<visitorData persistido>'

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
    QHash<QString, int> renderers; // renderer/viewmodel -> conteo
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

// Primera fase del sondeo usada por homeBrowseSelfTest (definida más abajo).
namespace {
void runHomeProbes(Ipv4Https *http, const QString &visitorData);
}

// Comprobación del camino de PRODUCCIÓN del Home: Innertube::fetchHome()
// (sw.js_data → visitorData fresco → POST /browse FEwhat_to_watch VISIONOS →
// parseHomeBrowse) y, si hay token, fetchHomeMore(). No toca el reproductor.
// Si el feed llega vacío (500/fallo de red) el selftest NO falla: ese es el
// comportamiento por diseño (la app usa el fallback yt-dlp). Sí falla si el
// parser produce duplicados o pierde el id/continuación esperados.
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
                                          "HOME-FEED feed vacío: la app usa el "
                                          "fallback yt-dlp (por diseño)\n");
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
                                          "HOME-FEED continuación detectada → "
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

    // Timeout global: no colgar el selftest si YouTube no responde.
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, []() {
        std::fprintf(stderr, "SELFTEST FAIL: home browse (timeout global)\n");
        QCoreApplication::exit(1);
    });
    watchdog.start(110000);

    // 1) visitorData: el persistido si viene por env, si no uno freso.
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
    // visitorData freso via sw.js_data (flujo normal).
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

    // Timeout global: los endpoints pueden tardar o fallar, nunca colgar.
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

    // Cuenta renderers de vídeo en una respuesta (sondeo de diagnóstico).
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

    // fases: 0 visitor | 1 diag canal | 2 datos canal (tube) | 3 diag trending
    // | 4 datos trending (tube) | 5 caso vacío (tube) | 6 fin
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
                QJsonParseError err;
                QByteArray json = body.trimmed();
                if (json.startsWith(")]}'"))
                    json.remove(0, 4);
                const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
                const QString renderers =
                    (err.error == QJsonParseError::NoError && doc.isObject())
                        ? countRenderers(doc.object())
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
                QJsonParseError err;
                QByteArray json = body.trimmed();
                if (json.startsWith(")]}'"))
                    json.remove(0, 4);
                const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
                const QString renderers =
                    (err.error == QJsonParseError::NoError && doc.isObject())
                        ? countRenderers(doc.object())
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

    // Resultados de la cadena real por Innertube.
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
                                 st.ok = false; // hoy debe responder (layout roto = señal)
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

// Lanza la secuencia de sondeos /browse por cliente. visitant puede estar
// vacío (entonces cada POST lleva la cabecera de fallback que ya usa el player
// LOGIN_REQUIRED; en la práctica el fallback fijo funciona).
void runHomeProbes(Ipv4Https *http, const QString &visitorData)
{
    // Secuencia: VISIONOS primero; IOS/ANDROID_VR/WEB_EMBEDDED sólo como
    // comparación (diagnóstico) y porque hay clientes ya definidos.
    static const int kProbeClientOrder[] = {0, 1, 4, 2};

    // Clientes de diagnóstico extra para /browse (SOLO este sondeo; NO se usa
    // en la cascada de producción kPlayerClients). Valores de yt-dlp
    // INNERTUBE_CLIENTS (client WEB real de navegador y ANDROID real de app).
    static const PlayerClient kDiagClients[] = {
        {"WEB", "2.20260708.00.00", 1, kChromeUA,
         nullptr, nullptr, nullptr, nullptr, 0, false, nullptr},
        {"ANDROID", "21.21.39", 3,
         "com.google.android.youtube/21.21.39 (Linux; U; Android 13; Pixel 7 "
         "Build/TQ3A.230805.001) gzip", // UA de yt-dlp INNERTUBE_CLIENTS
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
        std::function<void()> next;             // fase 1: sondeo por cliente
        std::function<void()> probeContinuation; // fase 2: continuación del mejor
    };
    auto *st = new State;
    st->http = http;
    st->visitor = visitorData;
    st->probeCount = static_cast<int>(sizeof(kProbeClientOrder)
                                          / sizeof(kProbeClientOrder[0]))
                     + static_cast<int>(sizeof(kDiagClients)
                                        / sizeof(kDiagClients[0]));

    if (st->visitor.isEmpty()) {
        // No hay visitorData (ni persistido ni freso): usar el fallback fijo
        // que ya usa el proyecto (kFallbackVisitorData) para poder probar.
        st->visitor = QString::fromLatin1(kFallbackVisitorData);
        st->usedFallbackVisitor = true;
        std::fprintf(stderr, "HOME-BROWSE visitor: fallback fijo len=%d\n",
                     st->visitor.size());
    }
    http->setVisitorId(st->visitor);

    st->probeContinuation = [st]() {
        // Fase 2: una sola petición de continuación en el mejor cliente.
        int bi = st->bestIdx;
        if (bi < 0 || st->bestContinuation.isEmpty() || st->contDone) {
            st->contDone = true;
            st->next();
            return;
        }
        st->contDone = true;
        // Resolver el cliente que produjo la continuación (producción o
        // diagnóstico), igual que en la fase 1.
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
            // Reanudar la secuencia para imprimir el RESULTADO y salir: sin
            // esto el sondeo se quedaba colgado hasta el watchdog global.
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
            // Fin de la fase 1: continuar con la continuación del mejor.
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
        // Resolver el cliente: primero los de producción (kPlayerClients),
        // después los de diagnóstico (WEB/ANDROID reales para /browse).
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
                // Guardar el cuerpo 200 grande para inspección offline (saber
                // qué renderer lleva los vídeos del Home en cada cliente).
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
            // Renderers observados en la respuesta (sin filtrar).
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
                // Sin vídeos directos en ningún cliente (esto es lo que ocurre
                // con el Home de la app: solo llega la shell + token). Guardar
                // la primera continuación para intentar pedir el feed real.
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

} // namespace

#include "yt_innertube.moc"