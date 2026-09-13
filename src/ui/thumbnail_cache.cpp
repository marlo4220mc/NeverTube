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

QPixmap ThumbnailCache::pixmap(const QString &key) const
{
    const auto it = m_cache.constFind(key);
    if (it != m_cache.constEnd()) {
        m_lru.removeOne(key);
        m_lru.prepend(key);
        return it.value();
    }
    return QPixmap();
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

// Miniatura estándar por id de vídeo. Se usa como fallback cuando la URL que
// trae el panel/radio (p. ej. hq720_custom_1.jpg) responde 404.
static QString standardThumbFor(const QString &key)
{
    if (!key.startsWith(QLatin1String("v_")))
        return QString();
    const QString id = key.mid(2);
    if (id.size() != 11)
        return QString();
    return QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(id);
}

void ThumbnailCache::request(const QString &key, const QString &url)
{
    if (key.isEmpty() || url.isEmpty())
        return;
    const QString jpegUrl = stripWebpParam(url);
    NT_DEBUG("thumbnail request key=%s url=%s\n", key.toUtf8().constData(),
             jpegUrl.toUtf8().constData());
    if (m_cache.contains(key) || m_inFlight.contains(key))
        return;

    const QString path = filePath(key);
    if (QFile::exists(path)) {
        QPixmap pm(path);
        if (!pm.isNull()) {
            m_cache.insert(key, pm);
            m_bytes += pm.width() * pm.height() * 4;
            m_lru.prepend(key);
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
    if (key.isEmpty() || pm.isNull() || m_cache.contains(key))
        return;
    m_cache.insert(key, pm);
    m_lru.prepend(key);
    m_bytes += pm.width() * pm.height() * 4;
    while (m_bytes > m_maxBytes && m_lru.size() > 4) {
        const QString victim = m_lru.takeLast();
        auto it = m_cache.find(victim);
        if (it != m_cache.end()) {
            m_bytes -= it->width() * it->height() * 4;
            m_cache.erase(it);
        }
    }
    pm.save(filePath(key), "JPG", 85);
    emit ready(key);
}

void ThumbnailCache::onFinished(QNetworkReply *reply)
{
    const QString key = reply->property("nt_key").toString();
    m_inFlight.remove(key);
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        NT_DEBUG("thumbnail failed key=%s error=%s url=%s\n",
                 key.toUtf8().constData(), reply->errorString().toUtf8().constData(),
                 reply->url().toString().toUtf8().constData());
        // Fallback: si la URL del panel/radio (variante custom_1, etc.) ya no
        // existe, pedir la miniatura estándar por id y reintentar una vez.
        const QString stdUrl = standardThumbFor(key);
        if (!stdUrl.isEmpty()
            && QUrl(stdUrl) != reply->request().url()) {
            NT_DEBUG("thumbnail fallback standard key=%s url=%s\n",
                     key.toUtf8().constData(), stdUrl.toUtf8().constData());
            request(key, stdUrl);
        }
        return;
    }

    const QByteArray data = reply->readAll();
    NT_DEBUG("thumbnail response key=%s bytes=%d\n", key.toUtf8().constData(),
             data.size());
    QImage image;
    if (!image.loadFromData(data))
        return;

    image = image.scaled(kMaxThumbW, kMaxThumbW * 9 / 16,
                         Qt::KeepAspectRatio, Qt::SmoothTransformation);
    store(key, image);
    emit ready(key);
}

void ThumbnailCache::store(const QString &key, const QImage &image)
{
    if (image.isNull())
        return;
    const QPixmap pm = QPixmap::fromImage(image);
    m_cache.insert(key, pm);
    m_lru.prepend(key);
    m_bytes += pm.width() * pm.height() * 4;

    while (m_bytes > m_maxBytes && m_lru.size() > 4) {
        const QString victim = m_lru.takeLast();
        auto it = m_cache.find(victim);
        if (it != m_cache.end()) {
            m_bytes -= it->width() * it->height() * 4;
            m_cache.erase(it);
        }
    }
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