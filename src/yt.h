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
//                   Harmony/YouTube Music), regenerada según el vídeo actual.
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
    // por homeReady(); vacíos = usar el fallback (el Home por yt-dlp se
    // mantiene). homeMore() pide la continuación si la respuesta la trajo.
    void home();
    void homeMore();
    bool homeHasContinuation() const;

    QString downloadVideoSelector(int maxHeight) const;
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

signals:
    void searchFinished(const QString &query, const QVector<VideoInfo> &videos);
    void trendingFinished(const QVector<VideoInfo> &videos);
    void channelVideosFinished(const QVector<VideoInfo> &videos);
    void channelTabVideosFinished(const QString &channelId, const QString &tab,
                                  const QVector<VideoInfo> &videos);
    void channelInfoReady(const ChannelInfo &channel);
    void detailReady(const VideoInfo &video, const QString &description,
                     qint64 likeCount, const QVector<FormatInfo> &formats);
    // Todos los intentos de getDetail fallaron para este vídeo (ningún
    // cliente yt-dlp devolvió metadatos válidos).
    void detailFailed(const QString &videoId);
    void playlistDetailReady(const PlaylistInfo &info,
                             const QVector<VideoInfo> &videos);
    void streamReady(const StreamInfo &stream);
    void error(const QString &message);
    void resolveError(const QString &message);
    void relatedReady(const QString &videoId, const QVector<VideoInfo> &videos);
    void radioReady(const QString &videoId, const QVector<VideoInfo> &videos);
    // Detalle traducido del vídeo actual (título y descripción) del /next
    // normal, en el idioma "hl" solicitado. WatchPage lo usa para sobrescribir
    // el título/desc en inglés que trae getDetail (yt-dlp, no traduce).
    void currentDetailReady(const QString &videoId, const QString &title,
                            const QString &description);
    void homeReady(const QVector<VideoInfo> &videos);
    void homeMoreReady(const QVector<VideoInfo> &videos);

private:
    void resolveWithYtDlp(const QString &videoId, int maxHeight,
                          bool fallbackToInnertube = false);
    void resolveViaInnertube(const QString &videoId, int maxHeight,
                             int preferredClient, bool forceRefresh = false);
    void channelVideosYtDlp(const QString &channelId);
    void trendingYtDlp();
    void execJson(QStringList args, std::function<void(const QJsonObject &)> done);
    void execLines(QStringList args, std::function<void(const QStringList &)> done);
    void getDetailTry(const QString &videoId, int clientIndex);
    void getUrls(const QString &videoId, const QString &selector,
                 std::function<void(const QStringList &)> done);
    QString buildHlsMaster(const QString &videoId, const QString &videoUrl,
                           const QString &audioUrl);
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
    // Bonus de puntaje por perfil de códec: qué códec se prefiere según el
    // perfil de hardware detectado (Modern: AV1>VP9; Vp9: solo VP9; Legacy: h264).
    int profileBonus(const QString &mime) const;

    static QString qualityName(int height);
    static int closestHeight(int requested, const QVector<int> &heights);
    // Altura del vídeo que elegiría buildEdgeMaster (mayor altura <= maxHeight
    // con url directa, h264 preferido). -1 si no hay vídeo aprovechable.
    int edgeHeight(const QJsonArray &formats, int maxHeight);
    static QString formatSelector(int maxHeight);
    QString audioSelector() const;
    static QVector<VideoInfo> parseVideos(const QJsonObject &root);
    static VideoInfo videoFromJson(const QJsonObject &j);
    static ChannelInfo channelFromJson(const QString &id, const QJsonObject &j);
    static void parsePlaylist(const QJsonObject &root, PlaylistInfo *info,
                              QVector<VideoInfo> *videos);
    static QString pickThumbnail(const QJsonArray &thumbs);
    static QString pickAvatar(const QJsonArray &thumbs);
    static QString fallbackThumb(const QString &videoId);
    static QString publishedFrom(const QJsonObject &j);

    HlsServer m_hls;
    Innertube m_innertube;
    SettingsRepo *m_visitorStore = nullptr;
    int m_resolveGen = 0;
    QString m_audioLang;
    CodecProfile m_codecProfile = CodecProfile::Legacy;
    // Método de resolución que logró reproducir cada vídeo: índice del
    // cliente Innertube (>= 0) o kResolveYtDlp. Se usa para dar prioridad
    // al cambiar de resolución (re-resolver el mismo vídeo).
    QHash<QString, int> m_winningClient;
};

// Selftest de la cadena real del feed en Yt: relay Innertube → fallback
// yt-dlp → señal channelVideosFinished/trendingFinished. Verifica que llega un
// resultado dentro de un plazo (Innertube o fallback) sin cuelgues, y registra
// qué fuente respondió. No toca el reproductor ni SQLite.
bool feedsChainSelfTest();

// Selftest offline del HLS sintético de AV1: valida la playlist generada
// (EXT-X-MAP/BYTERANGE, sufijo /seg.mp4, &nt distinto y consecutivo por
// segmento, elección de la mayor altura <= maxHeight y descarte correcto sin
// AV1). Sin red, sin ficheros y sin reproductor.
bool av1HlsSelfTest();