#include "thumbnail_cache.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

#include "../debug.h"
#include "../models.h"

ThumbnailCache::ThumbnailCache(const QString &name, int maxBytes, QObject *parent)
    : QObject(parent)
    , m_maxBytes(maxBytes)
{
    m_nam = new QNetworkAccessManager(this);
    m_dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/thumbs_") + name;
    QDir().mkpath(m_dir);
}

ThumbnailCache::~ThumbnailCache()
{
}

QString ThumbnailCache::filePath(const QString &key) const
{
    return m_dir + QLatin1Char('/') + key + QStringLiteral(".jpg");
}

void ThumbnailCache::touch(const QString &key) const
{
    m_lru.removeOne(key);
    m_lru.prepend(key);
}

void ThumbnailCache::insertLocked(const QString &key, const QPixmap &pm) const
{
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        m_bytes -= it->width() * it->height() * 4;
        m_cache.erase(it);
    }
    m_cache.insert(key, pm);
    m_bytes += pm.width() * pm.height() * 4;
    touch(key);
    while (m_bytes > m_maxBytes && m_lru.size() > 4) {
        const QString victim = m_lru.takeLast();
        auto vit = m_cache.find(victim);
        if (vit != m_cache.end()) {
            m_bytes -= vit->width() * vit->height() * 4;
            m_cache.erase(vit);
        }
    }
}

QPixmap ThumbnailCache::pixmap(const QString &key) const
{
    const auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd()) {
        touch(key);
        return it.value();
    }
    // Fallback a disco: el LRU pudo desalojar la imagen de memoria (p. ej. al
    // compartir presupuesto search+canal), pero el archivo sigue en m_dir. Sin
    // esto, el delegate pinta vacío y la miniatura "desaparece" para siempre,
    // porque nadie vuelve a hacer request() de un ítem ya mostrado.
    const QString path = filePath(key);
    if (!QFile::exists(path))
        return QPixmap();
    QPixmap pm(path);
    if (pm.isNull())
        return QPixmap();
    insertLocked(key, pm);
    return pm;
}

static QString stripWebpParam(const QString &url)
{
    QUrl q{url};
    const QString sqp = QUrlQuery{q}.queryItemValue(QStringLiteral("sqp"));
    if (sqp.isEmpty())
        return url;
    QUrlQuery clean{q};
    clean.removeQueryItem(QStringLiteral("sqp"));
    q.setQuery(clean);
    return q.toString(QUrl::FullyEncoded);
}

// YouTube sirve miniaturas webp bajo i.ytimg.com/vi_webp/<id>/<nombre>.webp.
// Qt sin plugin webp (ojo: los plugins no son dependencia necesaria) no las
// decodifica, así que se reescribe a la variante .jpg equivalente (existe para
// los mismos tamaños: maxresdefault, hq720, hqdefault...). Así se hace UNA
// sola descarga y siempre jpeg: menos latencia y memoria en máquinas modestas.
static QString jpegEquivalent(const QString &url)
{
    QUrl q{url};
    if (q.host() != QLatin1String("i.ytimg.com"))
        return url;
    const QString path = q.path();
    if (!path.startsWith(QLatin1String("/vi_webp/")))
        return url;
    QString rest = path.mid(9); // "/vi_webp/" -> "…/<nombre>.webp"
    if (!rest.endsWith(QLatin1String(".webp")))
        return url;
    rest.chop(5);
    q.setPath(QStringLiteral("/vi/") + rest + QLatin1String(".jpg"));
    return q.toString(QUrl::FullyEncoded);
}

// Miniatura estándar por id de vídeo. Se usa como fallback cuando la URL que
// trae el panel/radio (p. ej. hq720_custom_1.jpg) responde 404.
static QString standardThumbFor(const QString &key)
{
    if (!key.startsWith(QLatin1String("v_")))
        return QString();
    return fallbackThumbnailUrl(key.mid(2));
}

void ThumbnailCache::request(const QString &key, const QString &url)
{
    if (key.isEmpty() || url.isEmpty())
        return;
    const QString jpegUrl = jpegEquivalent(stripWebpParam(url));
    NT_DEBUG("thumbnail request key=%s url=%s\n", key.toUtf8().constData(),
             jpegUrl.toUtf8().constData());
    if (m_cache.contains(key) || m_inFlight.contains(key))
        return;

    const QString path = filePath(key);
    if (QFile::exists(path)) {
        QPixmap pm(path);
        if (!pm.isNull()) {
            insertLocked(key, pm);
            emit ready(key);
            return;
        }
    }

    m_inFlight.insert(key);
    QNetworkRequest req{QUrl(jpegUrl)};
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("Mozilla/5.0 MiniTube/1.0"));
    req.setRawHeader("Accept", "image/avif,image/webp,image/apng,image/*,*/*;q=0.8");
    req.setRawHeader("Referer", "https://www.youtube.com/");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(15000);
    QNetworkReply *reply = m_nam->get(req);
    reply->setProperty("nt_key", key);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply]() { onFinished(reply); });
}

void ThumbnailCache::requestChannelAvatar(const QString &channelId,
                                          const QString &url)
{
    request(QStringLiteral("ch_%1").arg(channelId), url);
}

void ThumbnailCache::put(const QString &key, const QPixmap &pm)
{
    if (key.isEmpty() || pm.isNull())
        return;
    insertLocked(key, pm);
    pm.save(filePath(key), "JPG", 85);
    emit ready(key);
}

void ThumbnailCache::onFinished(QNetworkReply *reply)
{
    const QString key = reply->property("nt_key").toString();
    m_inFlight.remove(key);
    const QNetworkReply::NetworkError err = reply->error();
    const QString requestedUrl = reply->url().toString();
    const QByteArray data = reply->readAll();
    QString decodeInfo;
    QImage image;
    if (err == QNetworkReply::NoError && !data.isEmpty()) {
        image.loadFromData(data);
        if (image.isNull())
            decodeInfo = QStringLiteral(" (no se pudo decodificar)");
    }
    reply->deleteLater();

    // Descarga rota o que no puede decodificarse (p. ej. webp en un Qt sin
    // plugin webp): reintentar UNA vez con la miniatura estándar por id
    // (hqdefault.jpg). Sin bucle: si la estándar tampoco sirve, no se vuelve
    // a llamar (QUrl(stdUrl) == url pedida).
    const bool bad = err != QNetworkReply::NoError || data.isEmpty()
        || image.isNull();
    if (bad) {
        const QString stdUrl = standardThumbFor(key);
        if (!stdUrl.isEmpty() && QUrl(stdUrl) != reply->request().url()) {
            NT_DEBUG("thumbnail fallback standard key=%s url=%s%s\n",
                     key.toUtf8().constData(), requestedUrl.toUtf8().constData(),
                     decodeInfo.toUtf8().constData());
            if (err != QNetworkReply::NoError)
                NT_DEBUG("thumbnail failed key=%s error=%s url=%s\n",
                         key.toUtf8().constData(),
                         reply->errorString().toUtf8().constData(),
                         requestedUrl.toUtf8().constData());
            request(key, stdUrl);
        }
        return;
    }

    NT_DEBUG("thumbnail response key=%s bytes=%d\n", key.toUtf8().constData(),
             data.size());
    image = image.scaled(kMaxThumbW, kMaxThumbW * 9 / 16,
                         Qt::KeepAspectRatio, Qt::SmoothTransformation);
    store(key, image);
    emit ready(key);
}

void ThumbnailCache::store(const QString &key, const QImage &image)
{
    if (image.isNull())
        return;
    insertLocked(key, QPixmap::fromImage(image));
    image.save(filePath(key), "JPG", 85);
}

int ThumbnailCache::clear()
{
    const int n = QDir(m_dir).entryList(QDir::Files).size();
    QDir(m_dir).removeRecursively();
    QDir().mkpath(m_dir);
    m_cache.clear();
    m_lru.clear();
    m_inFlight.clear();
    m_bytes = 0;
    return n;
}

int ThumbnailCache::maxWidth() const
{
    return kMaxThumbW;
}

int ThumbnailCache::sizeBytes() const
{
    return m_bytes;
}