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
    void resolveStream(const QString &videoId, int maxHeight);
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

    // Preferencia de idioma de audio para el master filtrado. Vacío (o
    // "original") = pista original. Un código de idioma (p. ej. "en-US" o
    // "pt") prioriza el doblaje de IA que coincida, con la original de
    // respaldo. Ajustes aún no lo configura; queda preparado.
    void setAudioLanguage(const QString &lang);
    QString audioLanguage() const;

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
                             int preferredClient);
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
    // Construye el master "edge" desde los adaptiveFormats del /player:
    // elige un único vídeo (altura <= maxHeight, h264 preferido) y un único
    // audio (idioma preferido), y escribe dos media playlists de segmento
    // único (vídeo y audio) + el master que las referencia. Devuelve la ruta
    // del master o vacío si no hay combo aprovechable (entonces el flujo
    // clásico hlsManifestUrl → fetchMaster se usa como fallback).
    QString buildEdgeMaster(const QString &videoId,
                            const QJsonArray &adaptiveFormats, int maxHeight,
                            const QString &audioLang, int durationSeconds);

    static QString qualityName(int height);
    static int closestHeight(int requested, const QVector<int> &heights);
    // Altura del vídeo que elegiría buildEdgeMaster (mayor altura <= maxHeight
    // con url directa, h264 preferido). -1 si no hay vídeo aprovechable.
    static int edgeHeight(const QJsonArray &formats, int maxHeight);
    static QString formatSelector(int maxHeight);
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
    // Método de resolución que logró reproducir cada vídeo: índice del
    // cliente Innertube (>= 0) o kResolveYtDlp. Se usa para dar prioridad
    // al cambiar de resolución (re-resolver el mismo vídeo).
    QHash<QString, int> m_winningClient;
    // Registro de vídeoIds para los que el master edge ya emitió streamReady;
    // el handler de hlsReady lo consulta para no duplicar la resolución
    // (emit streamReady doble). Se limpia al inicio de cada resolveViaInnertube.
    QSet<QString> m_edgeServed;
};

// Selftest de la cadena real del feed en Yt: relay Innertube → fallback
// yt-dlp → señal channelVideosFinished/trendingFinished. Verifica que llega un
// resultado dentro de un plazo (Innertube o fallback) sin cuelgues, y registra
// qué fuente respondió. No toca el reproductor ni SQLite.
bool feedsChainSelfTest();