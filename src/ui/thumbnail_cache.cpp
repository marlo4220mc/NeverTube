#include "thumbnail_cache.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>

ThumbnailCache::ThumbnailCache(QObject *parent)
    : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
    m_dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/thumbs");
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

void ThumbnailCache::request(const QString &key, const QString &url)
{
    if (key.isEmpty() || url.isEmpty())
        return;
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
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("MiniTube/1.0"));
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

void ThumbnailCache::onFinished(QNetworkReply *reply)
{
    const QString key = reply->property("nt_key").toString();
    m_inFlight.remove(key);
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError)
        return;

    const QByteArray data = reply->readAll();
    QImage image;
    if (!image.loadFromData(data))
        return;

    image = image.scaled(320, 180, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
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

    while (m_bytes > kMaxBytes && m_lru.size() > 4) {
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

int ThumbnailCache::sizeBytes() const
{
    return m_bytes;
}