#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include "models.h"

class Ipv4Https;

// Resolución de streams de YouTube mediante el API Innertube, de forma
// independiente de la UI, del reproductor y de yt-dlp.
//
// Flujo: GET /sw.js_data → visitorData → POST youtubei/v1/player (cascada de
// clientes estilo NewPipe/yt-dlp: VISIONOS primario, luego IOS,
// WEB_EMBEDDED_PLAYER, TVHTML5_SIMPLY y ANDROID_VR) → streamingData
// (hlsManifestUrl + adaptiveFormats, este último para conocer qué alturas de
// vídeo existen sin alimentar a mpv). fetchMaster() descarga el master HLS
// indicado (cualquier host HTTPS).
//
// El transporte HTTP/HTTPS propio fuerza conexión por IPv4 (con SNI y
// verificación TLS contra el host de destino). YouTube bloquea por IPv6
// ciertos videos ("Sign in to confirm you're not a bot") en esta red;
// por IPv4 no.
//
// No conoce ni PlayerWidget, ni mpv, ni ninguna página. Solo HTTP + JSON.
class Innertube : public QObject
{
    Q_OBJECT

public:
    explicit Innertube(QObject *parent = nullptr);
    ~Innertube() override;

    bool isBusy() const;

    // Usar un visitorData persistido de una sesión anterior (si la firma aún
    // es válida, evita el GET sw.js_data al arrancar). Lo llama Yt/la app con
    // el valor guardado en settings; si caducó, se ignora y se refresca solo.
    void seedVisitorData(const QString &visitorData, qint64 expiresAt);

public slots:
    void fetchHls(const QString &videoId, int preferredClient = -1);
    void fetchMaster(const QString &videoId, const QString &masterUrl);

    // Descarga los relacionados ("A continuación" / recommendations) del vídeo
    // mediante POST youtubei/v1/next con el cliente primario (VISIONOS). No
    // toca el flujo player/master; reusa el visitorData vigente.
    void fetchNext(const QString &videoId);

    // Watch-playlist "radio" estilo YouTube Music: POST /next con
    // playlistId RDAMVM<videoId> (+ params de radio) que devuelve la cola del
    // mix generado para el vídeo. fetchRadioMore() pide el siguiente batch con
    // el token de continuación. Cada vídeo responde por radioReady().
    void fetchRadio(const QString &videoId);
    void fetchRadioMore();

    // Home real (POST /browse, browseId FEwhat_to_watch) con el cliente VISIONOS.
    // Es el único endpoint probado que devuelve el feed (compactVideoRenderer/
    // shelfRenderer/verticalListRenderer) con un visitorData fresco; un
    // visitorData persistido antiguo devuelve HTTP 500. Por eso fetchHome()
    // fuerza un sw.js_data fresco (una vez por sesión) antes del POST. Los
    // resultados llegan por homeReady(); vacíos = hay que usar el fallback
    // (el Home actual por yt-dlp se mantiene). fetchHomeMore() pide la
    // continuación (homeMoreReady). Sin reintentos ni regeneraciones en bucle.
    void fetchHome();
    void fetchHomeMore();

    // Vídeos del canal suscrito (POST /browse, browseId del canal + params de
    // la pestaña "Vídeos") con el cliente VISIONOS. Reutiliza el mismo parser
    // compactVideoRenderer del Home; llegan por channelVideosReady(). Vacíos =
    // el feed cae al fallback yt-dlp (pestaña UU de uploads).
    void fetchChannelVideos(const QString &channelId);
    // Tendencias (POST /browse, browseId FEtrending) con VISIONOS. llegan por
    // trendingReady(). Hoy YouTube retira/redirige las tendencias anónimas
    // (400/redirección a la Home), así que esta fase suele quedar vacía y la
    // app cae al fallback yt-dlp o a ocultar la sección.
    void fetchTrending();

    // True si la última respuesta de Home trajo un token de continuación.
    bool hasHomeContinuation() const { return !m_homeContinuation.isEmpty(); }

    // Cliente Innertube que produjo el hlsReady más reciente, o -1 si aún
    // ninguno ganó (por ejemplo tras regenerar visitorData). Lo usa Yt para
    // recordar qué cliente logró reproducir y darle prioridad en cambios de
    // resolución.
    int lastWinClient() const { return m_lastWinClient; }

signals:
    // videoHeights: alturas de vídeo (únicas, ascendentes) presentes en
    // adaptiveFormats de la respuesta player. Vacío si no se pudo extraer.
    void hlsReady(const QString &videoId, const QString &hlsUrl,
                  const QVector<int> &videoHeights);
    void masterReady(const QString &videoId, const QByteArray &masterBody);
    void failed(const QString &videoId, const QString &reason);
    // Relacionados del vídeo pedido con /next. Lista vacía si no se pudo
    // extraer o la respuesta no los trae.
    void relatedReady(const QString &videoId, const QVector<VideoInfo> &videos);
    // Cola del mix/radio del vídeo pedido con /next (RDAMVM). Videos vacíos si
    // el cliente no devolvió cola (entonces la app decide mezclar o seguir).
    void radioReady(const QString &videoId, const QVector<VideoInfo> &videos);
    // Feed del Home (browse FEwhat_to_watch). Vacío = el endpoint no devolvió
    // vídeos (500 con visitorData antiguo, cambio de layout, sin feed
    // disponible); la app usa entonces el fallback yt-dlp.
    void homeReady(const QVector<VideoInfo> &videos);
    void homeMoreReady(const QVector<VideoInfo> &videos);
    // Vídeos de un canal (browse "Vídeos" tab) y Tendencias (browse
    // FEtrending). Vacíos = usar el fallback yt-dlp.
    void channelVideosReady(const QString &channelId,
                            const QVector<VideoInfo> &videos);
    void trendingReady(const QVector<VideoInfo> &videos);
    // visitorData renovado o usado por primera vez; la app lo persiste para
    // evitar el GET sw.js_data en el siguiente arranque.
    void visitorDataReady(const QString &visitorData, qint64 expiresAt);

private:
    void fetchVisitorData();
    void requestPlayer();
    void requestNext();
    void requestRadio();
    void requestHome(const QString &continuation);
    void requestChannel();
    void requestTrending();
    void handleHome(bool ok, const QString &error, const QByteArray &body,
                    bool more);
    void handleChannel(bool ok, const QString &error, const QByteArray &body);
    void handleTrending(bool ok, const QString &error, const QByteArray &body);
    void handleNext(const QString &videoId, bool ok, const QString &error,
                    const QByteArray &body);
    void handleRadio(const QString &videoId, bool ok, const QString &error,
                     const QByteArray &body);
    void useFallbackVisitorData();
    // Retry único por fase de las secciones del Home: si el browse (Home,
    // canal, tendencias) responde 500/vacío con el visitorData persistido
    // vigente, se refresca visitorData UNA vez y se reencola la fase (el
    // dispatcher lo relanza). Evita el refresh forzado de sw.js_data en cada
    // arranque; un fallo real apenas añade un GET + reintento.
    bool retryBrowseWithFreshVisitor(bool *retried, bool *pending);
    void dispatchPending();
    void handlePlayer(bool ok, const QString &error, const QByteArray &body);
    void handleMaster(bool ok, const QString &error, const QByteArray &body);
    void fail(const QString &reason);
    QVector<VideoInfo> parseRelated(const QJsonObject &root) const;
    // Renderer compactVideoRenderer/videoRenderer → VideoInfo. Rellena
    // publishedAt con una marca de tiempo aproximada si solo hay texto
    // relativo (p. ej. "hace 3 días"). Se usa en Home, canales y tendencias.
    VideoInfo videoFromHomeRenderer(const QJsonObject &item) const;
    // Recorre el árbol buscando compactVideoRenderer/videoRenderer (sin
    // duplicar ids) hasta maxCount, respetando el orden de la respuesta.
    QVector<VideoInfo> collectVideoRenderers(const QJsonObject &root,
                                             int maxCount) const;
    // Cola del mix de reproducción (playlistPanelVideoRenderer) + token de
    // continuación para pedir más batches. out vacío si el cliente no trae
    // panel de reproducción en el /next.
    void parseWatchQueue(const QJsonObject &root, QVector<VideoInfo> *out,
                         QString *continuation) const;
    // Parseador del feed del Home (FEwhat_to_watch): recorre el layout
    // verified singleColumnBrowseResultsRenderer → sectionListRenderer →
    // shelfRenderer → verticalListRenderer → compactVideoRenderer, limitando
    // a kHomeShelfCap shelves × kHomeShelfVideos vídeos. Si no encuentra
    // estantes (layout distinto), recolecta hasta kHomeShelfVideos. Deja el
    // primer token de continuación (si aparece) en *continuation.
    QVector<VideoInfo> parseHomeBrowse(const QJsonObject &root,
                                       QString *continuation) const;
    // Devuelve hlsReady/masterReady desde la caché (firma vigente) usando el
    // generador/esperador de petición única. Si no hay caché válida o hay una
    // petición en curso para ese videoId, no hace nada y la petición en curso
    // termina reemitiendo hlsReady/masterReady a todos los que esperan.
    bool serveCachedHls(const QString &videoId);
    bool serveCachedMaster(const QString &videoId);
    qint64 expireFromUrl(const QString &url) const;

    Ipv4Https *m_http = nullptr;
    QString m_videoId;
    QString m_visitorData;
    qint64 m_visitorExpiresAt = 0;
    bool m_retryingVisitor = false;
    int m_clientIndex = 0;
    int m_lastWinClient = -1;
    // True si el visitorData vigente se obtuvo fresco en esta sesión
    // (sw.js_data). Las fases player/next/radio lo piden al arrancar; las
    // secciones del Home (home/feed/tendencias) usan directamente el
    // persistido vigente y solo refrescan si un browse responde 500/vacío
    // (retry único mediante m_*VisitorRetried).
    bool m_visitorDataFreshRun = false;
    // Pendiente tras refrescar visitorData: el dispatcher lanza las fases que
    // quedaron esperando (player primero). m_visitorInFlight evita lanzar dos
    // GET sw.js_data a la vez cuando player + next/radio piden visitorData
    // casi simultáneamente.
    bool m_playerPending = false;
    bool m_visitorInFlight = false;

    // Caché de sesión por videoId: hlsManifestUrl firmada, cuerpo del master y
    // su expiración (del parámetro expire de la URL firmada). Evita repetir
    // POST player + GET master en cambios de resolución del mismo vídeo.
    struct HlsCacheEntry
    {
        QString hlsUrl;
        QByteArray masterBody;
        QVector<int> heights; // alturas adaptiveFormats (para filtrar master)
        qint64 expiresAt = 0; // momento Unix tras el cual la firma caduca
        int clientIndex = -1;
    };
    QHash<QString, HlsCacheEntry> m_hlsCache;
    // Single-flight por fase: si ya hay una petición HLS o master en curso
    // para m_flightVideoId, las peticiones para el mismo videoId se resuelven
    // cuando la primera termina (los guards de Yt filtran por gen y todas
    // reciben las mismas señales hlsReady/masterReady/failed).
    QString m_flightVideoId;
    bool m_hlsInFlight = false;
    bool mMasterInFlight = false;
    qint64 m_hlsStartMs = 0;
    qint64 m_masterStartMs = 0;

    // Petición de relacionados (/next). Independiente del flujo player; usa
    // el mismo visitorData. m_nextPending se marca cuando hay que refrescar
    // visitorData antes de poder lanzar el POST.
    bool m_nextPending = false;
    bool m_nextInFlight = false;
    QString m_nextVideoId;
    int m_nextClientIndex = 0;
    QVector<VideoInfo> m_bestNext;

    // Petición de la cola radio (/next con playlistId RDAMVM). Comparte
    // visitorData y el mismo transporte. El token de continuación permite
    // ir pidiendo batches del mix mientras se reproduce.
    bool m_radioPending = false;
    bool m_radioInFlight = false;
    QString m_radioVideoId;
    QString m_radioContinuation;
    // Mezcla de recomendaciones del primer cliente sin panel RDAMVM (fallback).
    QVector<VideoInfo> m_radioFallback;
    int m_radioClientIndex = 0;

    // Petición del Home real (/browse FEwhat_to_watch). Independiente del
    // flujo player; usa visitorData fresco. m_homeContinuation guarda el
    // token para fetchHomeMore(); m_homePending marca la espera del refresh
    // de visitorData.
    bool m_homePending = false;
    bool m_homeInFlight = false;
    bool m_homeMoreInFlight = false;
    QString m_homeContinuation;
    bool m_homeVisitorRetried = false;

    // Petición de vídeos de un canal (browse "Vídeos" tab). Una a la vez
    // (HomePage las encadena); usa visitorData fresco y el mismo transporte.
    bool m_channelPending = false;
    bool m_channelInFlight = false;
    QString m_channelId;
    QString m_channelContinuation;
    bool m_channelVisitorRetried = false;

    // Petición de Tendencias (browse FEtrending). Independiente; si falla/
    // vacía, la app cae al fallback yt-dlp o a ocultar la sección.
    bool m_trendPending = false;
    bool m_trendInFlight = false;
    bool m_trendVisitorRetried = false;
};

// Selftest aislado de la cadena sw.js_data → visitorData → player →
// hlsManifestUrl → alturas adaptiveFormats. No toca el reproductor.
// Devuelve true si la cadena completa responde con HLS y alturas.
bool innertubeSelfTest();

// Sondeo de diagnóstico del Home real (POST /browse, browseId FEwhat_to_watch).
// NO es el Home de la app y NO toca HomePage ni el reproductor: solo investiga
// si el endpoint de Home responde para los clientes existentes (VISIONOS
// primero, luego IOS/WEB/ANDROID como fallback de diagnóstico), con o sin
// visitorData persistido (NEVERTUBE_HOME_VISITOR). Imprime estado HTTP,
// tiempos, tamaño, renderers, vídeos, continuations y desafíos anti-bot.
bool homeBrowseSelfTest();

// Selftest de los feeds del Home por Innertube: vídeos de un canal (browse
// "Vídeos" tab + params, Yt-dlp como referencia del channelId por
// NEVERTUBE_TEST_CHANNEL) y Tendencias (FEtrending). Verifica estado HTTP,
// tamaño, renderers, vídeos reales (id, título, duración), orden, duplicados,
// límite, y el comportamiento vacío/error. No toca el reproductor ni SQLite.
bool feedsProtocolSelfTest();