#pragma once

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QObject>
#include <QVector>

#include "hls_server.h"
#include "models.h"
#include "yt_innertube.h"

// Modo de generación de "A continuación" (selección por NEVERTUBE_NEXT_MODE:
// none | suggestions | radio). Deja elegir entre:
//   none         -> sin siembra (la cola queda como vino de la entrada),
//   suggestions  -> /next estático: ~20 recomendaciones del vídeo actual,
//   radio        -> watch-playlist RDAMVM<videoId> + continuación (estilo
//                   YouTube Music), regenerada según el vídeo actual.
enum class NextMode
{
    None,
    Suggestions,
    Radio
};
NextMode nextMode();
void setNextMode(NextMode m);

class SettingsRepo;

class Yt : public QObject
{
    Q_OBJECT

public:
    explicit Yt(QObject *parent = nullptr);

    // Persistencia del visitorData (settings): si el Innertube consigue uno
    // nuevo se guarda aquí, y al arrancar se reutiliza para evitar el GET
    // sw.js_data (una RTT menos antes del POST /player).
    void setVisitorStorage(SettingsRepo *settings);

    void search(const QString &query, int limit = 20, int offset = 0);
    // Sugerencias de type-ahead del buscador. No lanza una búsqueda: solo pide
    // las sugerencias del prefijo y las entrega por suggestReady(). Es un GET
    // plano que no toca la sesión Innertube (ver Innertube::fetchSuggest).
    void suggest(const QString &query);
    void trending(int limit = 30);
    void channelVideos(const QString &channelId, int limit = 20);
    void channelTabVideos(const QString &channelId, const QString &tab,
                          int limit = 100, int offset = 0);
    void channelInfo(const QString &channelId);
    void getDetail(const QString &videoId);
    void playlistDetail(const QString &playlistId, int limit = 50);
    // forceRefresh=true re-resuelve con el POST /player aunque haya caché de
    // sesión del vídeo (URLs firmadas frescas; ver Innertube::fetchHls).
    void resolveStream(const QString &videoId, int maxHeight,
                       bool forceRefresh = false);
    // Rota la sesión Innertube (visitorData + URLs firmadas cacheadas): la
    // siguiente resolveStream se hará con una sesión de YouTube NUEVA. La usa
    // PlayerController en la recuperación tras una sesión gateada (403 a mitad
    // de reproducción), donde re-resolver dentro de la misma sesión no sirve.
    void rotateStreamSession();
    // Dispara la carga de los relacionados ("A continuación") del vídeo por
    // Innertube /next. Los resultados llegan por relatedReady().
    void relatedVideos(const QString &videoId);
    // Detalle traducido del vídeo actual (/next normal): título y descripción
    // en el idioma "hl" solicitado, para mostrar en el watch sin tocar la cola.
    void currentDetail(const QString &videoId);
    // Modo radio (modelo 3): pide la cola del mix RDAMVM del vídeo y, si el
    // mix tiene más batches, radioMore() encadena el siguiente cuando se
    // agota. Llegan por radioReady().
    void radioVideos(const QString &videoId);
    void radioMore();
    // Home real por Innertube (browse FEwhat_to_watch). Los resultados llegan
    // por homeReady(); vacíos = sin feed. homeMore() pide la continuación si
    // la respuesta la trajo.
    void home();
    void homeMore();
    bool homeHasContinuation() const;

    static QString videoIdFromUrl(const QString &url);

    // Un fragmento (subsegmento) del fMP4 según la caja sidx del indexRange.
    struct Av1Fragment
    {
        qint64 size = 0;
        double duration = 0.0; // segundos
    };

    // Texto de la media playlist HLS sintética para un adaptiveFormat AV1
    // (sin red ni ficheros): #EXT-X-MAP + segmentos #EXT-X-BYTERANGE con
    // sufijo /seg.mp4 y &nt distinto por segmento. Vacío si no hay AV1
    // <= maxHeight con url/initRange. heightOut recibe la altura elegida.
    // Si `fragments` viene informado, cada segmento equivale a un fragmento
    // real del fMP4 (fronteras exactas del sidx): imprescindible para que el
    // seek funcione. Sin él se cae a trozos fijos de ~2 MB (válido solo para
    // reproducción secuencial desde el inicio). Expuesto para el selftest.
    static QString av1HlsPlaylist(const QJsonArray &adaptiveFormats,
                                  int maxHeight, int *heightOut,
                                  const QVector<Av1Fragment> &fragments = {},
                                  qint64 fragmentsStart = -1);

    // Parsea la caja sidx (bytes del indexRange) y devuelve los fragmentos.
    // firstOffsetOut recibe el offset (relativo al final del indexRange) del
    // primer subsegmento. Vacío si no hay sidx válido.
    static QVector<Av1Fragment> av1ParseSidx(const QByteArray &indexBox,
                                             qint64 *firstOffsetOut);

    // Preferencia de idioma de audio para el master filtrado. Vacío (o
    // "original") = pista original. Un código de idioma (p. ej. "en-US" o
    // "pt") prioriza el doblaje de IA que coincida, con la original de
    // respaldo. Ajustes aún no lo configura; queda preparado.
    void setAudioLanguage(const QString &lang);
    QString audioLanguage() const;
    // Perfil de códec del master edge. Detectado en la primera reproducción
    // (hwdec-current per-códec) y persistido en codec_profile:
    //   Modern = AV1 + VP9 (hardware con decodificador AV1; AV1 se prefiere y
    //            VP9 es el respaldo a igual altura).
    //   Vp9    = solo VP9 (hardware sin AV1 pero con decodificador VP9).
    //   Legacy = solo h264 (sin hardware moderno; si además el h264 decodifica
    //            en software, MainWindow marca el modo "nohw": tope 1080p y
    //            espera de buffer inicial por porcentaje para 1080p).
    enum class CodecProfile { Modern, Vp9, Legacy };
    void setCodecProfile(CodecProfile profile);
    CodecProfile codecProfile() const { return m_codecProfile; }
    // Build x86 de Windows sin mpv (NEVERTUBE_NO_MPV): AV1 no decodifica ni
    // por HW ni por SW en el paquete i686. Con esto codecRankedScore NUNCA lo
    // elige: VP9 (software) siempre gana sobre un formato que falla siempre.
    void setNoAv1(bool on);

signals:
    void searchFinished(const QString &query, const QVector<VideoInfo> &videos);
    void suggestReady(const QString &query, const QVector<Suggestion> &suggestions);
    void trendingFinished(const QVector<VideoInfo> &videos);
    void channelVideosFinished(const QVector<VideoInfo> &videos);
    void channelTabVideosFinished(const QString &channelId, const QString &tab,
                                  const QVector<VideoInfo> &videos);
    void channelInfoReady(const ChannelInfo &channel);
    void detailReady(const VideoInfo &video, const QString &description,
                     qint64 likeCount, const QVector<FormatInfo> &formats);
    // getDetail no obtuvo metadatos válidos del /next (vídeo no disponible o
    // respuesta vacía).
    void detailFailed(const QString &videoId);
    void playlistDetailReady(const PlaylistInfo &info,
                             const QVector<VideoInfo> &videos);
    void streamReady(const StreamInfo &stream);
    // Alturas de vídeo disponibles del vídeo (adaptiveFormats de Innertube),
    // independientes del tope que pida el perfil: el menú de resoluciones del
    // reproductor las usa para ofrecer todas las calidades reales aunque Auto
    // haya pedido una baja. Vacío = indeterminado (el menú cae al techo del
    // stream cargado).
    void streamHeightsReady(const QString &videoId, const QVector<int> &heights);
    void error(const QString &message);
    void resolveError(const QString &message);
    void relatedReady(const QString &videoId, const QVector<VideoInfo> &videos);
    void radioReady(const QString &videoId, const QVector<VideoInfo> &videos);
    // Detalle traducido del vídeo actual (título y descripción) del /next
    // normal, en el idioma "hl" solicitado. WatchPage lo usa solo cuando el
    // idioma de audio es explícito (es/en/pt).
    void currentDetailReady(const QString &videoId, const QString &title,
                            const QString &description);
    void homeReady(const QVector<VideoInfo> &videos);
    void homeMoreReady(const QVector<VideoInfo> &videos);

private:
    void resolveViaInnertube(const QString &videoId, int maxHeight,
                             int preferredClient, bool forceRefresh = false);
    QString buildFilteredMaster(const QString &videoId,
                                const QByteArray &masterBody, int maxHeight);
    // HLS sintético local para reproducir AV1 por la vía HLS (YouTube no
    // publica AV1 en su master: solo VP9/h264). Cada segmento es una petición
    // Range corta (~2 MB) sobre la URL adaptativa AV1; el throttle por-request
    // de YouTube no aplica a ráfagas cortas, así que segmentar equivale a la
    // velocidad del HLS real. Devuelve la ruta del .m3u8 local o vacío si no
    // hay AV1 <= maxHeight con initRange/url; escribe heightOut con la altura.
    QString buildAv1Hls(const QString &videoId,
                        const QJsonArray &adaptiveFormats, int maxHeight,
                        int *heightOut);
    // Construye el modo "edge" desde los adaptiveFormats del /player:
    // elige un único vídeo (altura <= maxHeight, h264 preferido) y un único
    // audio (idioma preferido), y devuelve la URL directa del vídeo (mp4/fMP4).
    // Escribe audioUrlOut con la URL de la pista de audio directa (para
    // PlayerWidget::setAudioFile). Devuelve URL vacía si no hay combo
    // aprovechable (entonces el flujo clásico hlsManifestUrl → fetchMaster se
    // usa como fallback). Reproducir por URL directa (no HLS de un segmento)
    // da a mpv duración exacta y seek fiable — el HLS de segmento único perdía
    // ambos (barra de progreso saltando). 3 perfiles: modern (AV1), vp9-mid
    // (VP9 webm direct), legacy (h264). audioLang vacío == original.
    QString buildEdgeMaster(const QString &videoId,
                            const QJsonArray &adaptiveFormats, int maxHeight,
                            const QString &audioLang, int durationSeconds,
                            QString *audioUrlOut);
    // Puntaje de un formato de vídeo por altura + preferencia de códec según el
    // perfil de hardware detectado (Modern: AV1>VP9>h264; Vp9: VP9>h264, AV1
    // penalizado; Legacy: h264). La preferencia de códec pesa ×100000, así que
    // gana el códec preferido aunque sea más bajo, y dentro de él la mayor
    // altura.
    int codecRankedScore(int height, const QString &mimeOrCodecs) const;

    static QString qualityName(int height);
    static int closestHeight(int requested, const QVector<int> &heights);
    // Altura del vídeo que elegiría buildEdgeMaster (mayor altura <= maxHeight
    // con url directa, h264 preferido). -1 si no hay vídeo aprovechable.
    int edgeHeight(const QJsonArray &formats, int maxHeight);

    HlsServer m_hls;
    Innertube m_innertube;
    // Última consulta de búsqueda: distingue la primera página (fetchSearch)
    // de la continuación (fetchSearchMore) en Yt::search.
    QString m_searchQuery;
    SettingsRepo *m_visitorStore = nullptr;
    int m_resolveGen = 0;
    QString m_audioLang;
    CodecProfile m_codecProfile = CodecProfile::Legacy;
    bool m_noAv1 = false; // x86 (NEVERTUBE_NO_MPV): AV1 imposible, no elegirlo
    // Cliente Innertube que logró reproducir cada vídeo (índice >= 0). Se usa
    // para dar prioridad al cambiar de resolución (re-resolver el mismo vídeo).
    QHash<QString, int> m_winningClient;
};

// Selftest de la cadena real del feed en Yt: canal por Innertube + trending →
// señales channelVideosFinished/trendingFinished. Verifica que llega un
// resultado dentro de un plazo, sin cuelgues. No toca el reproductor ni SQLite.
bool feedsChainSelfTest();

// Selftest de la BÚSQUEDA por Innertube (Yt::search): lanza una búsqueda real
// (red) y exige vídeos con id no vacío.
bool searchSelfTest();

// Selftest offline del HLS sintético de AV1: valida la playlist generada
// (EXT-X-MAP/BYTERANGE, sufijo /seg.mp4, &nt distinto y consecutivo por
// segmento, elección de la mayor altura <= maxHeight y descarte correcto sin
// AV1). Sin red, sin ficheros y sin reproductor.
bool av1HlsSelfTest();