#pragma once

#include <QObject>
#include <QString>
#include <QVector>

class Ipv4Https;

// Resolución de streams de YouTube mediante el API Innertube, de forma
// independiente de la UI, del reproductor y de yt-dlp.
//
// Flujo: GET /sw.js_data → visitorData → POST youtubei/v1/player (cliente
// VISIONOS) → streamingData (hlsManifestUrl + adaptiveFormats, este último
// para conocer qué alturas de vídeo existen sin alimentar a mpv).
// fetchMaster() descarga el master HLS indicado (cualquier host HTTPS).
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
    void fetchHls(const QString &videoId);
    void fetchMaster(const QString &videoId, const QString &masterUrl);

signals:
    // videoHeights: alturas de vídeo (únicas, ascendentes) presentes en
    // adaptiveFormats de la respuesta player. Vacío si no se pudo extraer.
    void hlsReady(const QString &videoId, const QString &hlsUrl,
                  const QVector<int> &videoHeights);
    void masterReady(const QString &videoId, const QByteArray &masterBody);
    void failed(const QString &videoId, const QString &reason);

private:
    void fetchVisitorData();
    void requestPlayer();
    void handleVisitorData(bool ok, const QString &error, const QByteArray &body);
    void handlePlayer(bool ok, const QString &error, const QByteArray &body);
    void handleMaster(bool ok, const QString &error, const QByteArray &body);
    void fail(const QString &reason);

    Ipv4Https *m_http = nullptr;
    QString m_videoId;
    QString m_visitorData;
};

// Selftest aislado de la cadena sw.js_data → visitorData → player →
// hlsManifestUrl → alturas adaptiveFormats. No toca el reproductor.
// Devuelve true si la cadena completa responde con HLS y alturas.
bool innertubeSelfTest();