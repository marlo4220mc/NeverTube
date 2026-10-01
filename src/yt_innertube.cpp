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
// Sugerencias de búsqueda (type-ahead del campo).
//
// El endpoint elegido es el del autocompletado REAL de youtube.com:
// suggestqueries.google.com con client=youtube. Lo decide la medición de
// /selftest-suggest, no una preferencia (ver suggestSelfTest):
//   - Innertube music/get_search_suggestions solo coincide con el YT real en
//     el 14-43% de las sugerencias, da 6 en prefijos cortos y arrastra un
//     carrusel de musica de 4-52 KB que el type-ahead no usa.
//   - client=firefox coincide en el 50-71%, pero recorta a 10 items y tira
//     los metadatos de entidad.
//   - client=youtube ES el dataset del buscador de youtube.com: 14 items,
//     0.3-2 KB, ~280 ms, y trae la entidad (descripcion, avatar, wikipedia)
//     de las filas con panel de conocimiento.
// No toca la sesion Innertube: es un GET plano a un host de Google, asi que no
// puede sufrir el gate 403 de sesion documentado en AGENTS.md.
const char kSuggestUrl[] =
    "https://suggestqueries.google.com/complete/search"
    "?client=youtube&ds=yt&hl=es&gl=US&q=";
// Envoltura de JS que pone Google alrededor del JSON. NO es JavaScript que
// ejecutar: es un prefijo y un parentesis que se recortan para dejar el JSON
// plano, que se parsea con QJsonDocument. No hay motor JS en la app.
const char kSuggestJsPrefix[] = "window.google.ac.h(";

// UA del fork de youtube_explode_dart usado para /sw.js_data.
const char kChromeUA[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/132.0.0.0 Safari/537.36";

// UA de cliente VISIONOS (coincide con el del body player). Valores tomados
// de las tablas de clientes de referencia (NewPipeExtractor).
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

// Timeouts por fase del transporte Ipv4Https (ms). La regeneración de
// visitorData no debe esperar el timeout largo de player.
constexpr int kVisitorTimeoutMs = 10000;
constexpr int kPlayerTimeoutMs = 12000;
constexpr int kMasterTimeoutMs = 15000;

// Cascada de clientes para /player, en orden de preferencia. VISIONOS es el
// primario (único que hoy no requiere poToken y devuelve hlsManifestUrl en
// VODs); los demás son el fallback estilo NewPipe para vídeos que
// exigen login (edad/miembros) o que no responden en VISIONOS. Los valores
// (versión actual de 2026, device/os/UA) vienen de las tablas de clientes de
// referencia (NewPipeExtractor), no son inventados.
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

// Cliente WEB (solo browse de pestañas Shorts/Directos del canal): VISIONOS no
// expone esas pestañas (devuelve la Home) y WEB sí, aunque con el layout
// moderno (*ViewModel). No requiere X-Goog-Api-Key (verificado).
constexpr int kWebClientId = 1;
const char kWebClientVersion[] = "2.20240726.00.00";

const char kFieldMask[] =
    "playabilityStatus.status,playabilityStatus.reason,"
    "streamingData.hlsManifestUrl,streamingData.adaptiveFormats,"
    "videoDetails.videoId,videoDetails.lengthSeconds";

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
// (valores reales de los clientes). "hl" se fija en el idioma de la app
// (auto "es" por el fix de títulos originales: sin esto YouTube auto-traduce
// los títulos al inglés) y todos los clientes llevan visitorData.
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

// Contexto del cliente WEB (browse de pestañas shorts/live). Solo nombre,
// versión, idioma y visitorData: WEB no necesita device/os ni API key.
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

// Cuerpo de una respuesta de youtubei/v1 → QJsonObject. Quita el prefijo
// anti-XSSI (")]}'") que YouTube antepone a algunas respuestas y tolera
// cuerpos no-JSON (errores/HTML). Devuelve false si no hay objeto JSON.
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

// Body de /complete/search -> lista de sugerencias.
//
// El cuerpo es JSON envuelto en una llamada JS: window.google.ac.h([...]). No
// se ejecuta nada: se recorta el prefijo y el parentesis final, y lo que queda
// es un array JSON normal que se parsea con QJsonDocument. No hay motor JS en
// la app. Si Google cambiara el envoltorio, el recorte no aplica y el parseo
// falla -> lista vacia, que es el fallo correcto (el desplegable se oculta) en
// vez de pintar filas basura.
//
// Cada item es ["texto", flags, [ids...], {metadatos}]. Solo se conserva el
// texto, y SOLO si es una cadena no vacia: el filtro es explicito porque los
// items sin texto no son consultas buscables y no deben convertirse en filas
// en blanco del desplegable (el fallo tipico de este endpoint).
QVector<Suggestion> parseSuggestBody(const QByteArray &body)
{
    QVector<Suggestion> out;
    QString text = QString::fromUtf8(body).trimmed();
    if (text.startsWith(QLatin1String(")]}'"))) {
        text.remove(0, 4);
        text = text.trimmed(); // el XSSI suele seguir de un \n
    }
    const QString prefix = QLatin1String(kSuggestJsPrefix);
    if (text.startsWith(prefix)) {
        text = text.mid(prefix.size()).trimmed();
        if (text.endsWith(QLatin1Char(')')))
            text.chop(1);
    } else if (!text.startsWith(QLatin1Char('['))) {
        // Sin la envoltura esperada no es el formato conocido: no se intenta
        // adivinar ni a medias.
        return out;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray())
        return out;

    // arr[1] es la lista de items; arr[0] es el prefijo ya escrito.
    const QJsonArray arr = doc.array();
    if (arr.size() < 2)
        return out;
    const QJsonArray items = arr.at(1).toArray();
    for (const QJsonValue &v : items) {
        // El endpoint usa DOS formatos segun el client, y hay que leer los dos:
        //   client=youtube/chrome -> ["texto", flags, [ids...], {meta}]
        //   client=firefox        -> "texto" (string pelado)
        // Se cubre el de youtube (el que usa la app) y, si el item llega como
        // string, tambien se acepta: es el mismo texto y no cuesta nada. Un item
        // que no sea ninguno de los dos se descarta.
        QString query;
        QString detail;
        if (v.isString()) {
            query = v.toString().trimmed();
        } else if (v.isArray()) {
            const QJsonArray item = v.toArray();
            if (item.isEmpty())
                continue;
            query = item.at(0).toString().trimmed();
            // Metadatos de entidad del panel de conocimiento (solo algunas
            // filas los traen): "Cantante y compositor peruano", avatar,
            // wikipedia.
            const QJsonObject meta = item.at(3).toObject();
            detail = meta.value(QStringLiteral("zaf")).toString().trimmed();
            if (detail.isEmpty())
                detail = meta.value(QStringLiteral("zao")).toString().trimmed();
        } else {
            continue;
        }
        if (query.isEmpty())
            continue; // sin consulta real: no es una fila valida
        Suggestion s;
        s.query = query;
        s.detail = detail;
        out.append(s);
    }
    return out;
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
        // FullyEncoded (no el path()/query() por defecto, que DEVUELVEN la
        // forma decodificada): el request-line tiene que llevar al servidor lo
        // mismo que el caller escribio. Con query() sin FullyEncoded, un "&q="
        // que el caller percent-encoded a mano vuelve con los espacios en crudo
        // y el endpoint responde 400. Para el master HLS no se nota (sus URLs
        // ya vienen consistentes), pero en /complete/search lo rompia.
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
    // El gate de YouTube es de SESIÓN, no de URL: descartar el visitorData
    // vigente (aunque no haya caducado) fuerza sw.js_data -> visitorData nuevo.
    m_visitorData.clear();
    m_visitorExpiresAt = 0;
    m_retryingVisitor = false;
    // URLs firmadas cacheadas de la sesión anterior: todas potencialmente
    // gateadas. Descartarlas todas (no solo el vídeo en curso, por si el usuario
    // reproduce otro con esa caché) para que el siguiente fetchHls repita el
    // POST /player con el visitorData nuevo.
    if (!m_hlsCache.isEmpty()) {
        m_hlsCache.clear();
        saveHlsCacheToDisk();
    }
    NT_DEBUG("E2E innertube sesión rotada (visitorData + URLs descartados)\n");
}

void Innertube::fetchHls(const QString &videoId, int preferredClient,
                         bool forceRefresh)
{
    // Frescura forzada: descartar la entrada cacheada (hlsUrl + adaptiveFormats
    // con sus URLs firmadas) para que el POST /player devuelva URLs nuevas. Si
    // no, una segunda reproducción reusa la URL de videoplayback ya agotada
    // (presupuesto por petición) y YouTube responde 403 → vídeo congelado.
    if (forceRefresh && m_hlsCache.remove(videoId) > 0) {
        saveHlsCacheToDisk();
        NT_DEBUG("E2E innertube hls cache invalidada id=%s\n",
                 videoId.toUtf8().constData());
    }
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
    if (!hasFreshVisitor()) {
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
    // Pestañas de canal pendientes de visitorData: relanzar las que esperaban
    // (puede haber varias: la UI pide videos/shorts/live a la vez).
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

// Detalle traducido del vídeo actual con el /next normal. Cliente primario
// VISIONOS, sin cascada ni siembra: solo se emite currentDetailReady.
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

// videoMetadataRenderer del /next: título y descripción en el idioma "hl"
// solicitado (traducción de YouTube). Se busca tanto en el /next tipo
// twoColumnWatchNextResults (cliente de escritorio/TV) como en el
// singleColumnWatchNextResults → watchNextTabbedResultsRenderer que
// devuelve VISIONOS (vista por pestañas: la pestaña de reproducción trae el
// videoMetadataRenderer del vídeo actual).
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

    // Vista por pestañas (VISIONOS): recorrer la primera pestaña cuya sección
    // contenga el renderer del vídeo actual.
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

    // Ruta clásica results.results.contents[].itemSectionRenderer.contents[].
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
        // El mix RDAMVM solo trae panel (playlistPanelVideoRenderer) para
        // vídeos con edición de música; para el resto el /next responde la
        // mezcla como recomendaciones (compactVideoRenderer). Se guarda la
        // primera mezcla vista y se sigue cascadeando por si otro cliente
        // trae el panel real; si ninguno lo trae, se emite la mezcla.
        const QVector<VideoInfo> fb = parseRelated(jsonRoot);
        // Cliente principal (VISIONOS): responde la mezcla como
        // recomendaciones (compactVideoRenderer, SIN fecha y con el texto de
        // visitas localizado). En vez de emitirla ya, se pide UNA vez el mismo
        // /next al cliente WEB, que devuelve lockupViewModel con visitas y
        // fecha; si esa petición falla, se emite la de VISIONOS tal cual. Sigue
        // siendo una sola petición extra, no las 4 de la cascada completa
        // (~12-15 s), y el panel real de música lo siguen trayendo IOS/otros.
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

// Segunda petición del fallback de radio: el mismo /next con el cliente WEB.
// Su layout moderno (lockupViewModel) trae las visitas y la fecha que el
// compactVideoRenderer de VISIONOS no tiene; es lo único que hace que "A
// continuación" muestre los tres campos como el resto de la app. Sin API key
// (igual que el browse de pestañas de canal).
void Innertube::requestRadioWeb(const QString &videoId)
{
    // La radio sigue "en vuelo" (handleRadio acaba de bajar el flag): sin esto
    // un fetchRadio concurrente lanzaría otra petición y pisaría estos
    // callbacks.
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
    // Si el WEB no dio lista utilizable, se usa la que ya trajo VISIONOS: la
    // cola se sirve igual, solo que sin fecha.
    if (out.isEmpty())
        out = m_radioVisionFallback;
    m_radioVisionFallback.clear();
    NT_DEBUG("E2E innertube radioWeb ok id=%s n=%d\n",
             videoId.toUtf8().constData(), out.size());
    emit radioReady(videoId, out);
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
// Params de las pestañas Shorts y Directos (browse del canal). VISIONOS los
// ignora y devuelve la Home; el cliente WEB sí las sirve.
const char kChannelShortsParams[] = "EgZzaG9ydHPyBgUKA5oBAA==";
const char kChannelLiveParams[] = "EgdzdHJlYW1z8gYECgJ6AA==";
// Pestaña "Información" del canal (browse): descripción, país, url canónica.
const char kChannelAboutParams[] = "EgVhYm91dPIGBAoCEgA=";
// Tamaño de página de las pestañas de canal y de la búsqueda: la UI muestra
// "Mostrar más" cuando la página llega a este tamaño.
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
    // El visitorData persistido vigente se usa directamente: ya no se fuerza
    // el refresh una vez por sesión (gana una RTT en el arranque). Si el
    // browse responde 500/vacío, handleHome refresca visitorData UNA vez y
    // reintenta (m_homeVisitorRetried) sin bucle.
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

    // VISIONOS: el único cliente probado que devuelve el feed real del Home
    // (62 compactVideoRenderer en la PRUEBA 1). Sin cascada: si falla/500, se
    // reintenta una vez con visitorData fresco (no en bucle).
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
// Búsqueda y peticiones de browse de la UI (canal, pestañas, detalle,
// playlist). Mismo patrón que el Home: single-flight, visitorData y un
// reintento con visitorData fresco si el browse responde vacío/error.

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
    // Sin texto no hay nada que sugerir, y un prefijo de 1 letra solo trae
    // ruido de entidades; el minimo de 2 evita una peticion por cada tecla al
    // arrancar la palabra.
    const QString q = query.trimmed();
    if (q.size() < 2) {
        m_suggestInFlight = false;
        emit suggestReady(query, QVector<Suggestion>());
        return;
    }
    if (m_suggestInFlight && m_suggestQuery == q)
        return; // mismo prefijo ya pedido

    m_suggestInFlight = true;
    m_suggestQuery = q;
    // El query viaja en la URL, asi que codificarlo es OBLIGATORIO: con
    // espacios en crudo el endpoint devuelve 400. QUrl::toPercentEncoding deja
    // intactos los caracteres seguros y escapa el resto (acentos incluidos).
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
    m_http->setTimeoutMs(8000); // type-ahead: caduca antes que una busqueda
    // getUrl() valida TLS contra el host de la propia URL, asi que sirve para
    // suggestqueries.google.com sin tocar el transporte.
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
    // Se emite siempre, incluso vacio: la UI lo trata como "cierra el
    // desplegable", no como "no hagas nada".
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

    // Cliente WEB para las tres pestañas: VISIONOS solo expone "Vídeos" y su
    // continuación devuelve páginas vacías; WEB sirve videos/shorts/live con el
    // layout moderno (lockups) y continuación funcional.
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
            // Las tres pestañas van por el cliente WEB (lockups modernos); el
            // parser clásico queda como respaldo por si el layout cambia.
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
        // Un solo reintento con visitorData fresco por (canal, pestaña).
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

// browseId del canal en un byline compartido (compactVideoRenderer /
// playlistPanelVideoRenderer): runs[0].navigationEndpoint.browseEndpoint.
// Sin esto, los vídeos que llegan por la cola se quedaban SIN id de canal y no
// se podía ni entrar al canal ni suscribirse desde la página Player.
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

// "hace 3 días" / "hace 8 h" / "2 months ago" → fecha ISO aproximada (para
// ordenar feeds sin depender de una marca exacta). Si no reconoce el texto,
// devuelve vacío. Acepta las unidades abreviadas del lockupViewModel del /next
// ("hace 8 h", "hace 2 m", "hace 3 d"): sin ellas, la fecha se perdía y "A
// continuación" salía sin ella.
QString relativeToIso(const QString &text)
{
    const QString t = text.trimmed().toLower();
    QStringList parts = t.split(QLatin1Char(' '), Qt::SkipEmptyParts);
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
    } else if (parts.at(2) == QLatin1String("ago")) {   // "X unidad ago"
        countText = parts.at(0);
        unit = parts.at(1);
    } else {
        return QString();
    }
    bool ok = false;
    double n = countText.toDouble(&ok);
    if (!ok) {
        // "hace un mes" / "hace una hora": el lockup usa cifras, pero por si
        // acaso se acepta el artículo.
        if (countText == QLatin1String("un") || countText == QLatin1String("una")
            || countText == QLatin1String("an") || countText == QLatin1String("a"))
            n = 1;
        else
            return QString();
    }
    qint64 secs = 0;
    if (unit.contains(QStringLiteral("segund")) || unit.startsWith(QLatin1Char('s'))
        || unit.startsWith(QLatin1String("sec")))
        secs = qint64(n);
    else if (unit.contains(QStringLiteral("min")))
        secs = qint64(n * 60);
    else if (unit.contains(QStringLiteral("hora")) || unit.startsWith(QLatin1Char('h'))
             || unit.startsWith(QLatin1String("hour")))
        secs = qint64(n * 3600);
    else if (unit.contains(QStringLiteral("día"))
             || unit.contains(QStringLiteral("dia"))
             || unit.startsWith(QLatin1Char('d'))
             || unit.startsWith(QLatin1String("day")))
        secs = qint64(n * 86400);
    else if (unit.contains(QStringLiteral("sem"))
             || unit.startsWith(QLatin1String("week")))
        secs = qint64(n * 7 * 86400);
    else if (unit.contains(QStringLiteral("mes"))
             || unit.startsWith(QLatin1String("month"))
             || unit == QLatin1String("m"))
        secs = qint64(n * 30 * 86400);
    else if (unit.contains(QStringLiteral("año"))
             || unit.contains(QStringLiteral("ano"))
             || unit.startsWith(QLatin1String("year"))
             || unit == QLatin1String("a"))
        secs = qint64(n * 365 * 86400);
    else
        return QString();
    if (secs <= 0)
        return QString();
    return QDateTime::currentDateTimeUtc().addSecs(-secs)
        .toString(Qt::ISODate);
}

// Cuenta numérica de un texto localizado ("16,9 M de suscriptores",
// "47 M de visualizaciones", "1.234 suscriptores"). En "es" la coma es
// decimal y el punto millar; el sufijo M/K/B multiplica.
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
            // Multiplicador o separador decimal: coma decimal, punto de millar.
            num.remove(QLatin1Char('.'));
            num.replace(QLatin1Char(','), QLatin1Char('.'));
        } else {
            // Sin multiplicador y 3 dígitos tras el separador: millar.
            num.remove(QLatin1Char(','));
            num.remove(QLatin1Char('.'));
        }
    }
    bool ok = false;
    const double v = num.toDouble(&ok);
    return ok ? qRound64(v * mult) : 0;
}

// Primer valor con esa clave en el árbol (Undefined si no aparece).
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
    // El canal va en ownerText (Home/canal) o longBylineText/shortBylineText
    // (search): el primer run con browseEndpoint.browseId da nombre e id.
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
    v.viewCount = countFromText(homeTextOf(r->value(QStringLiteral("viewCountText"))));
    v.publishedAt = relativeToIso(homeTextOf(
        r->value(QStringLiteral("publishedTimeText"))));
    if (v.thumbnailUrl.isEmpty())
        v.thumbnailUrl = fallbackThumbnailUrl(v.id);
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

// ---------------------------------------------------------------------------
// Renderers modernos del cliente WEB (pestañas Shorts/Directos del canal, que
// VISIONOS no expone): shortsLockupViewModel y lockupViewModel.

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

int webLockupDuration(const QJsonObject &contentImage)
{
    const QJsonArray overlays = contentImage
                                    .value(QStringLiteral("thumbnailViewModel"))
                                    .toObject().value(QStringLiteral("overlays"))
                                    .toArray();
    for (const QJsonValue &ov : overlays) {
        const QJsonArray badges = ov.toObject()
            .value(QStringLiteral("thumbnailBottomOverlayViewModel")).toObject()
            .value(QStringLiteral("badges")).toArray();
        for (const QJsonValue &b : badges) {
            const QString txt = b.toObject()
                .value(QStringLiteral("thumbnailBadgeViewModel")).toObject()
                .value(QStringLiteral("text")).toString();
            const QStringList parts = txt.split(QLatin1Char(':'));
            if (parts.size() == 2)
                return parts.at(0).toInt() * 60 + parts.at(1).toInt();
            if (parts.size() == 3)
                return parts.at(0).toInt() * 3600 + parts.at(1).toInt() * 60
                       + parts.at(2).toInt();
        }
    }
    return 0;
}

// shortsLockupViewModel: id en entityId="shorts-shelf-item-<id>", título y
// vistas en overlayMetadata, miniatura vertical en thumbnailViewModel.
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

// ¿El texto es un recuento tipo "21 K", "5,4 M", "684"? El lockup del /next
// da las vistas como número + multiplicador, SIN la palabra "vistas" (esa
// palabra solo aparece en el layout de canal). Se exige que empiece por dígito
// y que lo que queda sea solo el sufijo del multiplicador, para no confundir el
// nombre del canal ("3 Doors Down" empieza por dígito pero sigue con letras).
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

// lockupViewModel: id=contentId, título en lockupMetadataViewModel, vistas y
// fecha en metadataRows, miniatura+duración en contentImage.
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
            if (t.contains(QStringLiteral("hace"))
                || t.contains(QStringLiteral("ago")))
                v.publishedAt = relativeToIso(t);
            else if (t.contains(QStringLiteral("visualizacion"))
                     || t.contains(QStringLiteral("view")))
                v.viewCount = countFromText(t);
            else if (looksLikeCountText(t))
                v.viewCount = countFromText(t);
            else if (v.channel.isEmpty())
                v.channel = t; // el primer dato que no es visitas ni fecha
        }
    }
    // Canal: el avatar del lockup lleva el browseEndpoint del canal, así que se
    // resuelve también el id (lo necesita el menú "Ver canal" de la fila).
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
    v.durationSeconds = webLockupDuration(contentImage);
    if (v.thumbnailUrl.isEmpty())
        v.thumbnailUrl = fallbackThumbnailUrl(v.id);
    return v;
}

} // namespace

// Primer token de continuación del árbol, sea cual sea el layout
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

// Página de /search: compactVideoRenderer del sectionListRenderer.
QVector<VideoInfo> Innertube::parseSearchBrowse(const QJsonObject &root,
                                                int maxCount,
                                                QString *continuation) const
{
    if (continuation)
        *continuation = firstContinuationToken(root);
    return collectVideoRenderers(root, maxCount);
}

// Recorre el árbol recolectando los lockups modernos del cliente WEB sin
// duplicar ids (pestañas Shorts/Directos del canal).
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

// Ficha de canal del browse "Información": c4TabbedHeaderRenderer (título,
// avatar, suscriptores) + channelAboutMetadataRenderer (descripción).
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

// Detalle de vídeo del /next: videoMetadataRenderer (título, vistas, fecha,
// canal, descripción) + likeButtonRenderer (me gusta).
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
        video->viewCount = countFromText(
            homeTextOf(vm.value(QStringLiteral("viewCountText"))));
        video->publishedAt = relativeToIso(
            homeTextOf(vm.value(QStringLiteral("dateText"))));
        const QJsonObject owner = vm.value(QStringLiteral("owner")).toObject()
                                      .value(QStringLiteral("videoOwnerRenderer"))
                                      .toObject();
        video->channel = homeTextOf(owner.value(QStringLiteral("title")));
        video->channelId = owner.value(QStringLiteral("navigationEndpoint"))
                               .toObject().value(QStringLiteral("browseEndpoint"))
                               .toObject().value(QStringLiteral("browseId"))
                               .toString();
        // Vídeos con varios canales de COLABORACIÓN: el owner no lleva
        // navigationEndpoint (solo avatarStack + subscribeButton), así que el id
        // sale del botón de suscripción. Sin esta vía, esos canales no se podían
        // entrar ni suscribir desde la página Player (medido: channelId vacío
        // en el detalle y el botón "Canal" sin hacer nada).
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
            // Id del canal, igual que en la cola: sin él, un vídeo abierto
            // desde una playlist no podía entrar al canal ni suscribirse hasta
            // que llegara el detalle.
            v.channelId = channelIdFromByline(
                pv.value(QStringLiteral("shortBylineText")).toObject());
            if (v.channelId.isEmpty()) {
                v.channelId = channelIdFromByline(
                    pv.value(QStringLiteral("longBylineText")).toObject());
            }
            v.durationSeconds =
                pv.value(QStringLiteral("lengthSeconds")).toString().toInt();
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
            // Mismo parser localizado que el resto ("1,2 M de visitas"): con
            // toInt() directo un panel con texto daba visitas=0.
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
        // Reutiliza el parser de texto localizado: "85.805 visualizaciones",
        // "1,2 M de vistas", "85 mil visualizaciones". El parser propio que
        // había aquí quitaba puntos/comas/espacios y hacía toLongLong del
        // resultado, así que "85.805 visualizaciones" daba 0 y "A continuación"
        // se quedaba SIN las visitas (medido en el render).
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
            // Byline compartido (compactVideoRenderer): el nombre va en
            // longBylineText/shortBylineText y el id del canal en sus runs.
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
        v.viewCount = viewCountOf(homeTextOf(r->value(QStringLiteral("viewCountText"))));
        v.publishedAt = relativeToIso(homeTextOf(
            r->value(QStringLiteral("publishedTimeText"))));
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

    // Cliente WEB: la lista de recomendaciones viene en el layout moderno
    // (lockupViewModel), no en compactVideoRenderer. Es el layout que SÍ trae
    // las visitas y la fecha en la metadata del lockup, así que sin esta rama
    // un /next del cliente WEB salía vacío y "A continuación" se quedaba sin
    // esos dos campos (que es lo que hace el cliente VISIONOS, cuyo
    // compactVideoRenderer no tiene fecha).
    if (out.isEmpty())
        out = collectWebLockups(root, 20);

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
    QJsonObject jsonRoot;
    const bool jsonOk = parseJsonObject(body, &jsonRoot);
    if (!jsonOk) {
        NT_DEBUG("E2E innertube player json fail size=%d prefix=%s\n",
                 body.size(), body.left(120).constData());
        fail(QStringLiteral("respuesta player no es JSON válido"));
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
    const QJsonArray adaptiveFormats =
        streamingData.value(QStringLiteral("adaptiveFormats")).toArray();
    int durationSeconds = root.value(QStringLiteral("videoDetails"))
                              .toObject()
                              .value(QStringLiteral("lengthSeconds"))
                              .toInt();
    // Algunos contextos (p.ej. VISIONOS) no incluyen lengthSeconds; la URL
    // firmada de cada formato trae "dur=<segundos>" con la duración real.
    // Usarla evita que el master edge declare EXTINF falsos (~1h) que rompen
    // la barra de progreso y el tiempo final del reproductor.
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
    const qint64 elapsed =
        QDateTime::currentMSecsSinceEpoch() - m_hlsStartMs;
    NT_DEBUG("E2E innertube hls ok len=%d alturas=%d client=%s (%lldms)\n",
             hls.size(), heights.size(), kPlayerClients[m_clientIndex].name,
             static_cast<long long>(elapsed));
    // Caché de sesión: guarda hlsManifestUrl firmada, su expiración y los
    // adaptiveFormats (con url directa) + duración, para no repetir el POST
    // player en cambios de resolución del mismo vídeo.
    const qint64 exp = expireFromUrl(hls);
    m_hlsCache.insert(m_videoId,
                      HlsCacheEntry{hls, QByteArray(), heights, exp,
                                    m_clientIndex, adaptiveFormats,
                                    durationSeconds});
    saveHlsCacheToDisk();
    m_hlsInFlight = false;
    mMasterInFlight = false;
    // Los adaptiveFormats (con url por formato) primero: Yt construye el
    // master edge (1 vídeo + 1 audio) sin tocar el master de YouTube. Si el
    // edge no es posible, el flujo clásico hlsReady → fetchMaster lo cubre.
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
    // Emitir primero los adaptiveFormats cacheados: Yt puede reconstruir el
    // master edge sin repetir el POST player; si no procede, hlsReady cubre
    // el camino clásico (fetchMaster).
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
// comportamiento por diseño (la app oculta la sección). Sí falla si el
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

// --- C1: que endpoint de sugerencias usar ----------------------------------
//
// No es un PASS/FAIL: la pregunta es "que endpoint sirve, con que cliente y a
// que precio", asi que se miden los dos candidatos y se imprime la tabla para
// decidir. El numero que mas importa no es cuantas sugerencias salen, sino
// cuantas son CONSULTAS y cuantas son ATAJOS: las que solo traen
// serviceEndpoint apuntan a otro sitio (Google, el historial de la cuenta) y en
// una app que es solo YouTube no tienen destino. Si el parseo las dejara pasar
// serian filas muertas en el desplegable (el bug de Harmony: nav() a null).
struct SuggestStats
{
    int total = 0;        // searchSuggestionRenderer encontrados
    int conQuery = 0;     // traen navigationEndpoint.searchEndpoint.query
    QStringList muestras; // hasta 6, marcando "(sin query)" donde toca
};

// Recorre el JSON buscando searchSuggestionRenderer en cualquier parte en vez
// de fiarse de una ruta fija: si YouTube cambia el layout, el conteo sigue
// siendo valido y el selftest lo delata, en vez de devolver cero en silencio.
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

    // Cuerpo real (medido con client=youtube): mix de filas con entidad, filas
    // planas, y el envoltorio JS. Cada item es ["texto", flags, ids, meta].
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

    // Formato string (client=firefox): items como "texto" a secas.
    const QVector<Suggestion> plain =
        parseSuggestBody("[\"blender\",[\"blender 4.5\",\"blender 5.0\"]]");
    check(plain.size() == 2 && plain.at(0).query == QStringLiteral("blender 4.5")
              && plain.at(0).detail.isEmpty(),
          "items-string (firefox) tambien se parsean");

    // Formatos que NO son el nuestro: lista vacia, sin crashear.
    check(parseSuggestBody("<html>error</html>").isEmpty(),
          "HTML de error -> vacio");
    check(parseSuggestBody("").isEmpty(), "body vacio -> vacio");
    check(parseSuggestBody("window.google.ac.h(\"no soy array\")").isEmpty(),
          "envoltorio con contenido no-array -> vacio");
    check(parseSuggestBody("{\"contents\":[]}").isEmpty(),
          "objeto JSON en vez de array -> vacio");
    check(parseSuggestBody("window.google.ac.h([\"solo\"])").isEmpty(),
          "array sin lista de items -> vacio");

    // Prefijo XSSI, por si algun dia vuelve.
    const QVector<Suggestion> xssi =
        parseSuggestBody(")]}'\n[\"x\",[\"x resultado\"]]");
    check(xssi.size() == 1 && xssi.at(0).query == QStringLiteral("x resultado"),
          "prefijo XSSI )]}' tolerado");

    std::fprintf(stderr, "SUGGEST PARSE %s\n",
                 fail == 0 ? "OK" : "FAIL");
    return fail == 0;
}

bool suggestSelfTest()
{
    std::fprintf(stderr, "SUGGEST SELFTEST start\n");
    // Primero el parser, que es offline y determinista: si el parseo esta roto
    // no tiene sentido ni intentar la red.
    if (!suggestParseSelfTest()) {
        std::fprintf(stderr, "SELFTEST FAIL: suggest (parseo)\n");
        return false;
    }
    std::fprintf(stderr, "\n");
    Ipv4Https http;

    // Watchdog global: son varias peticiones reales; no colgar el selftest.
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
        const char *path;       // Innertube, relativo a www.youtube.com
        const char *url;        // Google, absoluta; si no es null, gana
        const char *clientName; // solo Innertube
        int clientId;
        const char *clientVersion;
        const char *referer;
    };
    // A = Innertube music/get_search_suggestions: POST con contexto y
    // visitorData. B = suggestqueries: GET plano, sin cliente ni visitorData.
    // Mismo prefijo y misma red para que la comparacion sea justa.
    //
    // C1 (client=youtube) es el que usa C2: es el autocompletado real de
    // youtube.com. Se midio su solapamiento con el dataset de A1 y de B1 y
    // gano por ser el original: 14 items, metadatos de entidad y 0.3-2 KB.
    // A1 se mantiene en la sonda como referencia de calidad (y es la que peor
    // coincide: 14-43% con el YT real).
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
    // Prefijos de theme claro (tutorial/receta/codigo), de musica, corto, y
    // "Will" (el caso que pidio medir el usuario: 4 letras, una palabra).
    // "tutorial blender" y "receta de tarta" llevan espacios A PROPOSITO:
    // medido con curl, un espacio crudo en la query hace que suggestqueries
    // conteste HTTP 400. El type-ahead tiene que funcionar con lo que se
    // escribe de verdad (una frase), no solo con prefijos de una palabra, asi
    // que estos casos DEBEN quedar verdes: si C2 no codifica bien la query, se
    // ve aqui.
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
    std::function<void()> run; // se asigna abajo; stepDone lo invoca

    const auto stepDone = [&]() {
        ++step;
        if (step < total) {
            // Con un turno de evento entre sondas: encadenar peticiones desde
            // dentro del readyRead del socket es reentrada.
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
        // El PASS es del endpoint que USA C2 (C1, client=youtube): tiene que
        // devolver consultas con las CUATRO, incluidas las de espacios. Las
        // demas sondas son referencia de calidad; si C1 se degrada, el type-ahead
        // se queda sin sugerencias aunque las otras sigan verdes, asi que el
        // fallo tiene que salir de aqui.
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
        // OJO: estas cuatro son COPIAS a proposito. El transporte guarda los
        // std::function de setResult/setError y los invoca DESPUES de que este
        // marco (el de runImpl) haya retornado. Si se capturaran por
        // referencia, record() leeria p.clientVersion de memoria liberada y
        // QString::fromLatin1() construiria sobre un const char* basura
        // (corrupcion de heap, no un fallo legible).
        const Probe p = probes[idx % probeCount];
        const QString q = queries.at(idx / probeCount);
        const bool google = p.url != nullptr;
        const QString key =
            QStringLiteral("%1|%2").arg(QLatin1String(p.label), q);
        const qint64 t0 = QDateTime::currentMSecsSinceEpoch();

        // Por referencia solo lo que vive en el marco de suggestSelfTest(), que
        // sigue activo dentro de exec(): el hash de resultados y stepDone.
        QHash<QString, Result> &sink = results;
        const auto &next = stepDone;

        const auto record = [&sink, &next, p, q, google, key, t0](int status,
                                                                 const QByteArray &body) {
            Result res;
            res.status = status;
            res.ms = int(QDateTime::currentMSecsSinceEpoch() - t0);
            res.bytes = body.size();
            if (google) {
                // Se usa el MISMO parser que C2 (parseSuggestBody), no uno
                // propio: asi la sonda mide el codigo de produccion, incluido
                // el recorte del envoltorio window.google.ac.h(...) que solo
                // necesita client=youtube. firefox/chrome vienen en JSON pelado
                // y el parser tambien los acepta.
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

        // Cada peticion captura SUS callbacks al encolarse (el transporte es
        // FIFO), asi que setResult/setError van antes de CADA request.
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
    // en la cascada de producción kPlayerClients). Valores de las tablas de
    // clientes de referencia (WEB real de navegador y ANDROID real de app).
    static const PlayerClient kDiagClients[] = {
        {"WEB", "2.20260708.00.00", 1, kChromeUA,
         nullptr, nullptr, nullptr, nullptr, 0, false, nullptr},
        {"ANDROID", "21.21.39", 3,
         "com.google.android.youtube/21.21.39 (Linux; U; Android 13; Pixel 7 "
         "Build/TQ3A.230805.001) gzip", // UA del cliente ANDROID de referencia
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

// Selftest de los endpoints de browse de Innertube: búsqueda, ficha de
// canal, pestañas (videos/shorts/live), detalle de vídeo y playlist. Red real;
// no toca el reproductor ni SQLite ni ficheros.

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

    // 1) Búsqueda.
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

    // 2) Ficha de canal.
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

    // 3) Pestañas del canal: videos por VISIONOS, shorts/live por WEB.
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
        // videos/shorts deben traer datos; live puede estar vacío (el canal
        // puede no tener directos), pero no debe colgarse.
        const bool good = r && empty == 0
                          && (tab == QLatin1String("live") || !got.isEmpty());
        std::fprintf(stderr,
                     "BROWSE channelTab %s videos=%d idVacios=%d more=%d -> %s\n",
                     tab.toUtf8().constData(), got.size(), empty,
                     more ? 1 : 0, good ? "OK" : "FAIL");
        ok = ok && good;

        // Continuación ("Mostrar más"): solo cuando la página llega al tamaño
        // pedido (la UI muestra el botón en ese caso). Debe traer vídeos nuevos
        // sin repetir los de la primera.
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

    // 4) Detalle de vídeo.
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