#pragma once

#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QVector>

#include "models.h"

class Ipv4Https;

// Una fila del desplegable de sugerencias del buscador.
// query: la consulta que se ejecutaria al pulsarla. detail: texto de apoyo de
// la entidad cuando YouTube lo envia ("Cantante y compositor peruano"), vacio
// en las filas normales.
struct Suggestion
{
    QString query;
    QString detail;
};

// Resolución de streams de YouTube mediante el API Innertube, de forma
// independiente de la UI y del reproductor.
//
// Flujo: GET /sw.js_data → visitorData → POST youtubei/v1/player (cascada de
// clientes estilo NewPipe: VISIONOS primario, luego IOS,
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

    // Idioma "hl" para las peticiones Innertube (traducción de títulos,
    // descripciones y vistas). La app lo fija según audio_language: es/en/pt
    // traducen a ese idioma; "original" deja el hl por defecto (es, como
    // estaba antes: evita la auto-traducción automática al inglés).
    void setHl(const QString &hl);
    QString hl() const { return m_hl; }

    // Invalida la sesión vigente (visitorData + URLs firmadas cacheadas) para
    // que la siguiente petición player se haga con un visitorData NUEVO (sesión
    // de YouTube distinta). Es la recuperación de la sesión "gateada" (403 a
    // mitad de reproducción en audio y/o segmentos HLS): una re-resolución
    // normal con forceRefresh pide un pot nuevo pero DENTRO de la misma sesión
    // gateada y vuelve a fallar; rotar el visitorData es lo único que sale de
    // la sesión. No toca peticiones en curso ni el transporte.
    void rotateSession();

public slots:
    // forceRefresh=true ignora y descarta la caché de sesión del vídeo y repite
    // el POST /player: las URLs firmadas de videoplayback traen un presupuesto
    // por petición y una segunda reproducción con la misma URL da 403.
    void fetchHls(const QString &videoId, int preferredClient = -1,
                  bool forceRefresh = false);
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

    // Detalle traducido del vídeo actual solicitado explícitamente para el
    // watch (cuando /next no se pide para sembrar la cola, p. ej. Radio):
    // una variante de fetchNext que solo emite currentDetailReady.
    void fetchCurrentDetail(const QString &videoId);

    // Home real (POST /browse, browseId FEwhat_to_watch) con el cliente VISIONOS.
    // Es el único endpoint probado que devuelve el feed (compactVideoRenderer/
    // shelfRenderer/verticalListRenderer) con un visitorData fresco; un
    // visitorData persistido antiguo devuelve HTTP 500. Por eso fetchHome()
    // fuerza un sw.js_data fresco (una vez por sesión) antes del POST. Los
    // resultados llegan por homeReady(); vacíos = sin feed. fetchHomeMore()
    // pide la continuación (homeMoreReady). Sin reintentos ni regeneraciones
    // en bucle.
    void fetchHome();
    void fetchHomeMore();

    // Vídeos del canal suscrito (POST /browse, browseId del canal + params de
    // la pestaña "Vídeos") con el cliente VISIONOS. Reutiliza el mismo parser
    // compactVideoRenderer del Home; llegan por channelVideosReady(). Vacíos =
    // sin vídeos.
    void fetchChannelVideos(const QString &channelId);
    // Tendencias (POST /browse, browseId FEtrending) con VISIONOS. llegan por
    // trendingReady(). Hoy YouTube retira/redirige las tendencias anónimas
    // (400/redirección a la Home), así que esta fase suele quedar vacía y la
    // app oculta la sección.
    void fetchTrending();

    // Búsqueda (POST /search, VISIONOS). fetchSearch() trae la primera página;
    // fetchSearchMore() la continuación. searchReady() entrega solo la página
    // nueva (hasta limit) y si queda continuación.
    void fetchSearch(const QString &query, int limit = 20);
    void fetchSearchMore(int limit = 20);

    // Sugerencias de type-ahead para el campo de búsqueda. GET plano a
    // suggestqueries.google.com (el autocompletado real de youtube.com), sin
    // tocar la sesión Innertube. suggestReady emite SIEMPRE el query pedido
    // para que la UI descarte respuestas fuera de orden al seguir escribiendo.
    void fetchSuggest(const QString &query);

    // Ficha de canal (POST /browse, pestaña "Información"): nombre, avatar,
    // suscriptores y descripción. channelInfoReady().
    void fetchChannelInfo(const QString &channelId);

    // Pestaña de canal (videos|shorts|live). "videos" va por VISIONOS
    // (compactVideoRenderer); shorts/live por el cliente WEB, porque VISIONOS
    // no expone esas pestañas (devuelve la Home). channelTabReady() con la
    // página y si hay más.
    void fetchChannelTab(const QString &channelId, const QString &tab,
                         int limit = 25);
    void fetchChannelTabMore(const QString &channelId, const QString &tab,
                             int limit = 25);

    // Detalle del vídeo (título, canal, vistas, fecha, descripción y "me
    // gusta") desde el /next. videoDetailReady().
    void fetchVideoDetail(const QString &videoId);

    // Detalle de playlist (POST /browse, browseId VL<id>): cabecera + vídeos.
    void fetchPlaylist(const QString &playlistId, int limit = 50);

    // True si la última respuesta de Home trajo un token de continuación.
    bool hasHomeContinuation() const { return !m_homeContinuation.isEmpty(); }

    // Cliente Innertube que produjo el hlsReady más reciente, o -1 si aún
    // ninguno ganó (por ejemplo tras regenerar visitorData). Lo usa Yt para
    // recordar qué cliente logró reproducir y darle prioridad en cambios de
    // resolución.
    int lastWinClient() const { return m_lastWinClient; }

    // Ruta del archivo JSON de la caché de resolución (hls+master) en disco.
    // Público para que el reseteo de la aplicación pueda borrarlo con la
    // misma ruta que usa la persistencia.
    static QString hlsCacheFilePath();

signals:
    // videoHeights: alturas de vídeo (únicas, ascendentes) presentes en
    // adaptiveFormats de la respuesta player. Vacío si no se pudo extraer.
    void hlsReady(const QString &videoId, const QString &hlsUrl,
                  const QVector<int> &videoHeights);
    void masterReady(const QString &videoId, const QByteArray &masterBody);
    // adaptiveFormats completos (con url directa por formato) de la respuesta
    // del /player. Lo consume Yt para construir el master "edge" de un solo
    // vídeo + audio desde Innertube; si está vacío (o sus entradas no traen
    // url), Yt cae al modo clásico hlsManifestUrl → fetchMaster.
    void formatsReady(const QString &videoId, const QJsonArray &adaptiveFormats,
                      int durationSeconds);
    void failed(const QString &videoId, const QString &reason);
    // Relacionados del vídeo pedido con /next. Lista vacía si no se pudo
    // extraer o la respuesta no los trae.
    void relatedReady(const QString &videoId, const QVector<VideoInfo> &videos);
    // Cola del mix/radio del vídeo pedido con /next (RDAMVM). Videos vacíos si
    // el cliente no devolvió cola (entonces la app decide mezclar o seguir).
    void radioReady(const QString &videoId, const QVector<VideoInfo> &videos);
    // Detalle del vídeo actual devuelto por /next: título y descripción en el
    // idioma "hl" solicitado (traducción de YouTube). Vacío de descripción =
    // el cliente no incluyó el metadata renderer del vídeo en reproducción.
    void currentDetailReady(const QString &videoId, const QString &title,
                            const QString &description);
    // Feed del Home (browse FEwhat_to_watch). Vacío = el endpoint no devolvió
    // vídeos (500 con visitorData antiguo, cambio de layout, sin feed
    // disponible).
    void homeReady(const QVector<VideoInfo> &videos);
    void homeMoreReady(const QVector<VideoInfo> &videos);
    // Vídeos de un canal (browse "Vídeos" tab) y Tendencias (browse
    // FEtrending). Vacíos = sin resultados.
    void channelVideosReady(const QString &channelId,
                            const QVector<VideoInfo> &videos);
    void trendingReady(const QVector<VideoInfo> &videos);
    // Página de resultados de búsqueda (searchReady) y de una pestaña de canal
    // (channelTabReady). hasMore indica si YouTube emitió token de continuación.
    void searchReady(const QString &query, const QVector<VideoInfo> &videos,
                     bool hasMore);
    // Sugerencias de type-ahead para searchQuery. Vacía si la petición falló o
    // el cuerpo no traía consultas válidas: la UI oculta el desplegable.
    void suggestReady(const QString &searchQuery,
                      const QVector<Suggestion> &suggestions);
    void channelTabReady(const QString &channelId, const QString &tab,
                         const QVector<VideoInfo> &videos, bool hasMore);
    // Ficha del canal (nombre, avatar, suscriptores, descripción).
    void channelInfoReady(const ChannelInfo &channel);
    // Detalle del vídeo actual (title/description traducidos + likes) del /next.
    void videoDetailReady(const VideoInfo &video, const QString &description,
                          qint64 likeCount);
    // Cabecera + vídeos de una playlist.
    void playlistReady(const PlaylistInfo &info,
                       const QVector<VideoInfo> &videos);
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
    void requestSearch(bool more);
    void handleSuggest(const QString &query, bool ok, const QByteArray &body);
    void handleSearch(const QString &query, bool ok, const QString &error,
                      const QByteArray &body, bool more);
    void requestChannelInfo();
    void handleChannelInfo(const QString &channelId, bool ok,
                           const QString &error, const QByteArray &body);
    void requestChannelTab(const QString &channelId, const QString &tab,
                           bool more);
    void handleChannelTab(const QString &channelId, const QString &tab, bool ok,
                          const QString &error, const QByteArray &body,
                          bool more);
    void requestVideoDetail();
    void handleVideoDetail(const QString &videoId, bool ok,
                           const QString &error, const QByteArray &body);
    void requestPlaylist();
    void handlePlaylist(const QString &playlistId, bool ok,
                        const QString &error, const QByteArray &body);
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
    // True si el visitorData vigente sirve (no vacío y sin caducar). Cuando es
    // false, la fase correspondiente debe marcar su *Pending, llamar a
    // fetchVisitorData() y salir: el dispatcher la relanzará.
    bool hasFreshVisitor() const;
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
    // Página de /search: compactVideoRenderer del sectionListRenderer +
    // primer token de continuación. Limitado a maxCount.
    QVector<VideoInfo> parseSearchBrowse(const QJsonObject &root, int maxCount,
                                         QString *continuation) const;
    // Renderers modernos del cliente WEB: shortsLockupViewModel y
    // lockupViewModel (pestañas shorts/live del canal). Recorre el árbol sin
    // duplicar ids; maxCount limita la página.
    QVector<VideoInfo> collectWebLockups(const QJsonObject &root,
                                         int maxCount) const;
    // Ficha del canal desde el browse "Información": c4TabbedHeaderRenderer
    // (título, avatar, suscriptores, handle) + channelAboutMetadataRenderer
    // (descripción, país, url canónica).
    static void parseChannelInfo(const QJsonObject &root, ChannelInfo *info);
    // Detalle del /next: videoMetadataRenderer (título, vistas, fecha, canal,
    // descripción) + likeButtonRenderer (me gusta).
    static void parseVideoDetail(const QJsonObject &root, VideoInfo *video,
                                 QString *description, qint64 *likeCount);
    // Playlist (browse VL<id>): playlistHeaderRenderer + playlistVideoRenderer.
    static void parsePlaylistBrowse(const QJsonObject &root, PlaylistInfo *info,
                                    QVector<VideoInfo> *videos);
    // Primer token de continuación que aparezca en el árbol (cualquier
    // layout: continuationCommand / nextContinuationData /
    // reloadContinuationData). Vacío si no hay.
    static QString firstContinuationToken(const QJsonObject &root);
    // Devuelve hlsReady/masterReady desde la caché (firma vigente) usando el
    // generador/esperador de petición única. Si no hay caché válida o hay una
    // petición en curso para ese videoId, no hace nada y la petición en curso
    // termina reemitiendo hlsReady/masterReady a todos los que esperan.
    bool serveCachedHls(const QString &videoId);
    bool serveCachedMaster(const QString &videoId);
    qint64 expireFromUrl(const QString &url) const;

    // Detalle traducido del vídeo actual: POST /next normal y emite
    // currentDetailReady con el videoMetadataRenderer (título y descripción
    // en el "hl" activo). Sin cascada de clientes (VISIONOS primario).
    void requestCurrentDetail();
    // Título y descripción del videoMetadataRenderer del /next, en el idioma
    // "hl" solicitado. Salida vacía si el cliente no incluyó el renderer.
    static void parseCurrentDetail(const QJsonObject &root, QString *title,
                                   QString *description);

    // Persistencia de la caché de resolución (hls+master) a disco, para
    // reabrir vídeos ya resueltos sin repetir POST player + GET master
    // mientras la firma de la URL siga vigente. Mismo patrón que el
    // visitorData persistido (SettingsRepo), pero en JSON.
    void saveHlsCacheToDisk() const;
    void loadHlsCacheFromDisk();

    Ipv4Https *m_http = nullptr;
    QString m_videoId;
    QString m_visitorData;
    // Idioma "hl" de las peticiones (traducción de títulos/descripciones por
    // la app según audio_language). "es" es el default histórico (fix de
    // títulos originales frente a la auto-traducción al inglés).
    QString m_hl = QStringLiteral("es");
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
        // adaptiveFormats del /player (con url directa) + duración del vídeo:
        // permiten reconstruir el master edge sin repetir el POST player.
        QJsonArray adaptiveFormats;
        int durationSeconds = 0;
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

    // Detalle traducido del vídeo actual (/next normal): título y descripción
    // en el "hl" solicitado. Independiente de la cola: nunca emite
    // relatedReady ni toca la siembra. m_currentDetailPending marca la espera
    // del refresh de visitorData.
    bool m_currentDetailPending = false;
    bool m_currentDetailInFlight = false;
    QString m_currentDetailVideoId;

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
    // responde vacía, la app oculta la sección.
    bool m_trendPending = false;
    bool m_trendInFlight = false;
    bool m_trendVisitorRetried = false;

    // Búsqueda (/search). Primera página y continuación; el token guardado
    // permite fetchSearchMore().
    bool m_searchPending = false;
    bool m_searchInFlight = false;
    bool m_searchVisitorRetried = false;
    QString m_searchQuery;
    QString m_searchContinuation;
    int m_searchLimit = 20;
    // Type-ahead de sugerencias. No lleva estado de paging ni de reintento: es
    // una petición de ida por prefijo. Solo se guarda el query en vuelo para
    // descartar respuestas que lleguen tarde (el usuario sigue escribiendo).
    bool m_suggestInFlight = false;
    QString m_suggestQuery;
    // Ficha de canal (browse "Información").
    bool m_chInfoPending = false;
    bool m_chInfoInFlight = false;
    bool m_chInfoVisitorRetried = false;
    QString m_chInfoId;
    // Pestañas de canal (videos por VISIONOS; shorts/live por WEB). La UI pide
    // las 3 a la vez, así que cada (canal, pestaña) tiene su propio estado:
    // un único juego de miembros hacia que las respuestas concurrentes se
    // etiquetaran con la última pestaña pedida.
    struct ChannelTabState
    {
        QString channelId;
        QString tab;
        bool inFlight = false;
        bool moreInFlight = false;
        bool pending = false;
        bool visitorRetried = false;
        QString continuation;
        int limit = 25;
    };
    QHash<QString, ChannelTabState> m_tabStates;
    static QString tabKey(const QString &channelId, const QString &tab);
    // Detalle del vídeo (/next).
    bool m_detailPending = false;
    bool m_detailInFlight = false;
    bool m_detailVisitorRetried = false;
    QString m_detailId;
    // Detalle de playlist (browse VL<id>).
    bool m_plPending = false;
    bool m_plInFlight = false;
    bool m_plVisitorRetried = false;
    QString m_plId;
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
// "Vídeos" tab + params; canal configurable con NEVERTUBE_TEST_CHANNEL) y
// Tendencias (FEtrending). Verifica estado HTTP, tamaño, renderers, vídeos
// reales (id, título, duración), orden, duplicados, límite y el comportamiento
// vacío/error. No toca el reproductor ni SQLite.
bool feedsProtocolSelfTest();

// Selftest de los endpoints de browse de Innertube: búsqueda, ficha de canal,
// pestañas (videos/shorts/live), detalle de vídeo y playlist.
bool innertubeBrowseSelfTest();

// Sonda de los endpoints de sugerencias (type-ahead del campo de búsqueda).
// Mide el endpoint que USA el type-ahead (C1, suggestqueries con
// client=youtube) contra dos referencias de calidad, con el mismo prefijo y la
// misma red. Imprime estado, latencia, tamano y cuantas sugerencias son
// CONSULTA real. Ademas ejecuta suggestParseSelfTest() (offline) para fijar el
// comportamiento del parser. El PASS/FAIL es de C1: es el que usa la app.
bool suggestSelfTest();

// Selftest OFFLINE del parseo de /complete/search (sin red ni GPU, va al CI).
// Fija los tres puntos donde este endpoint puede romperse en silencio:
//   1) el envoltorio window.google.ac.h(...) se recorta y se lee el JSON;
//   2) los items SIN consulta (o con texto no-cadena) se descartan, no salen
//      como filas en blanco;
//   3) un body que no es el formato conocido (HTML de error, JSON vacio,
//      envoltorio cambiado) devuelve lista vacia y NO revienta.
bool suggestParseSelfTest();