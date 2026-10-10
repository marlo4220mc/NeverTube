#pragma once

#include <QString>
#include <QVector>

struct VideoInfo
{
    QString id;
    QString title;
    QString channel;
    QString channelId;
    QString thumbnailUrl;
    QString description;
    QString publishedAt;
    int durationSeconds = 0;
    int viewCount = 0;
    // Directo actual: lo declaran los renderers (badge LIVE / "En directo",
    // viewCount "viéndolo ahora" del /next) y el player response
    // (videoDetails.isLive). Se persiste en la caché (videos.is_live) y las
    // miniaturas muestran "En directo" en vez de la duración.
    bool isLive = false;
};

struct ChannelInfo
{
    QString id;
    QString name;
    QString avatarUrl;
    QString description;
    long long subscriberCount = 0;
};

struct FormatInfo
{
    QString formatId;
    QString label;
    QString ext;
    int height = 0;
};

struct StreamInfo
{
    QString url;
    QString audioUrl;
    QString label;
    int height = 0;
    bool multipleUrls = false;
    // Falso solo para streams CONSTRUIDOS por la app desde un medio finito
    // (el HLS sintético de AV1: un fMP4 con playlist VOD+ENDLIST). Un stream
    // así no puede ser un directo por construcción, así que la UI de directo
    // (EN VIVO, playhead al borde) se desactiva para él aunque el backend
    // reporte duraciones inestables mientras carga. El resto de flujos (master
    // de YouTube, filtrado, edge) SÍ pueden ser un directo y quedan con true.
    bool canBeLive = true;
    // Señal AUTORITATIVA de directo (videoDetails.isLive del player response
    // de YouTube): true solo si YouTube declara el medio como directo actual.
    // Permite entrar en modo EN VIVO al instante, sin esperar a que la
    // duración "hasta ahora" crezca (y cubre los directos puros con dur=0).
    bool isLive = false;
};

struct PlaylistInfo
{
    QString id;
    QString title;
    QString channel;
    QString thumbnailUrl;
    int videoCount = 0;
};

// URL de la miniatura de reserva de un vídeo, cuando la respuesta/feed no trae
// ninguna. Solo para ids de YouTube válidos (11 caracteres); si no, vacío.
inline QString fallbackThumbnailUrl(const QString &videoId)
{
    return videoId.size() == 11
        ? QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(videoId)
        : QString();
}