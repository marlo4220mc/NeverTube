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

public slots:
    void fetchHls(const QString &videoId, int preferredClient = -1);
    void fetchMaster(const QString &videoId, const QString &masterUrl);

    // Descarga los relacionados ("A continuación" / recommendations) del vídeo
    // mediante POST youtubei/v1/next con el cliente primario (VISIONOS). No
    // toca el flujo player/master; reusa el visitorData vigente.
    void fetchNext(const QString &videoId);

    // Watch-playlist "radio" estilo Harmony/YouTube Music: POST /next con
    // playlistId RDAMVM<videoId> (+ params de radio) que devuelve la cola del
    // mix generado para el vídeo. fetchRadioMore() pide el siguiente batch con
    // el token de continuación. Cada vídeo responde por radioReady().
    void fetchRadio(const QString &videoId);
    void fetchRadioMore();

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

private:
    void fetchVisitorData();
    void requestPlayer();
    void requestNext();
    void requestRadio();
    void handleNext(const QString &videoId, bool ok, const QString &error,
                    const QByteArray &body);
    void handleRadio(const QString &videoId, bool ok, const QString &error,
                     const QByteArray &body);
    void useFallbackVisitorData();
    void dispatchPending();
    void handlePlayer(bool ok, const QString &error, const QByteArray &body);
    void handleMaster(bool ok, const QString &error, const QByteArray &body);
    void fail(const QString &reason);
    QVector<VideoInfo> parseRelated(const QJsonObject &root) const;
    // Cola del mix de reproducción (playlistPanelVideoRenderer) + token de
    // continuación para pedir más batches. out vacío si el cliente no trae
    // panel de reproducción en el /next.
    void parseWatchQueue(const QJsonObject &root, QVector<VideoInfo> *out,
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
};

// Selftest aislado de la cadena sw.js_data → visitorData → player →
// hlsManifestUrl → alturas adaptiveFormats. No toca el reproductor.
// Devuelve true si la cadena completa responde con HLS y alturas.
bool innertubeSelfTest();