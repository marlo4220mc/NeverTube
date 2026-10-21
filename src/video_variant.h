#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

// Una variante de video que un video ofrece: la altura, el códec y el
// tamaño REAL de la pista.
//
// La usan dos cosas que hoy no se hablan: el reproductor (que sabe lo que
// el video ofrece, para armar el menú de la descarga) y el downloader (que
// tiene que elegir una pista). El tipo vive fuera de los dos para que
// ninguno dependa del otro.
//
// Por qué el códec es parte de la variante: un video tiene VARIAS variantes
// por altura (AV1, VP9 y H264), y no son la misma imagen: el AV1 es más
// chico a igual calidad, pero el H264 es el que abre cualquier cosa. El
// usuario tiene que poder elegir (el submenú del botón de descarga).

struct VideoVariant
{
    int height = 0;
    // La familia del códec: "av1", "vp9" o "h264" (vacío = no se pudo
    // identificar).
    QString codec;
    // El tamaño real de la pista (el contentLength), no una estimación: es
    // lo que se muestra al lado del códec y lo que se descarga.
    qint64 sizeBytes = 0;
    // El formato del que viene, tal cual lo dio el player response.
    QJsonObject format;
};

// El tamaño real de un formato: el contentLength del player response, o el
// "clen" de su URL (el master lo trae en la URL; el adaptive, no).
qint64 formatBytes(const QJsonObject &format);

// La familia del códec a partir del mimeType o del codecs= del formato:
// "av01.0.09M.08" -> "av1", "vp09.00.40.08" -> "vp9", "avc1.640028" ->
// "h264".
//
// Se comparan los PREFIJOS, no el contenido: "vp9" y "vp09" son el mismo
// códec, y "avc1" y "avc3" también.
QString videoCodecFamily(const QString &mimeOrCodecs);

// Las variantes que ofrecen los formatos adaptativos: UNA por (altura,
// códec), con su tamaño real.
//
// Solo cuentan las que se pueden DESCARGAR (las que traen URL directa: las
// que vienen cifradas con signatureCipher no se pueden pedir por rango).
// Cuando hay más de una variante de la misma (altura, códec) —el mismo
// códec con distinto bitrate— gana la más chica.
//
// `requireInitRange` filtra las que no son un fMP4 completo (sin el init no
// se puede reproducir ni ensamblar): lo necesita el downloader, no el
// menú del player.
//
// Sin cap de altura: el cap es del reproductor, no del archivo (el usuario
// puede descargar un 4K que el player no reproduzca).
QVector<VideoVariant> videoVariantsOf(const QJsonArray &adaptiveFormats,
                                      bool requireInitRange = false);

// La pista de audio que se usaría: la ORIGINAL del video, o la del idioma
// preferido (prefLang vacío = la original).
//
// El mp4 (con sidx) le gana al WebM (sin él): el mp4 se puede pedir por
// trozos y el WebM no. Entre los del mismo tipo gana el más CHICO (para una
// descarga el tamaño es lo que importa).
//
// Es la MISMA regla para el reproductor y para el downloader: una sola
// copia, para que el tamaño que muestra el menú sea el que se descarga.
QJsonObject preferredAudioFormat(const QJsonArray &adaptiveFormats,
                                  const QString &prefLang = QString());

// El tamaño real de la pista de audio: el mismo para todos los códecs de
// video, y el menú lo muestra al lado de cada uno.
qint64 audioBytesOf(const QJsonArray &adaptiveFormats);


