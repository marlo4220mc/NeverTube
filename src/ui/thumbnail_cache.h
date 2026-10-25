#pragma once

#include <QObject>
#include <QPixmap>
#include <QSet>

class QNetworkAccessManager;
class QNetworkReply;

class ThumbnailCache : public QObject
{
    Q_OBJECT

public:
    explicit ThumbnailCache(const QString &name, int maxBytes,
                            QObject *parent = nullptr);
    ~ThumbnailCache() override;
    static constexpr int kMaxThumbW = 640;

    QPixmap pixmap(const QString &key) const;
    void request(const QString &key, const QString &url);
    void requestChannelAvatar(const QString &channelId, const QString &url);
    void put(const QString &key, const QPixmap &pm);
    int clear();
    int sizeBytes() const;

signals:
    void ready(const QString &key);

private slots:
    void onFinished(QNetworkReply *reply);

private:
    QString filePath(const QString &key) const;
    // Inserts/updates in memory respecting the LRU budget. Const because it
    // only touches mutable members: it is also used from pixmap() (paint).
    void touch(const QString &key) const;
    void insertLocked(const QString &key, const QPixmap &pm) const;
public:
    void store(const QString &key, const QImage &image);
private:

    QNetworkAccessManager *m_nam = nullptr;
    mutable QHash<QString, QPixmap> m_cache;
    mutable QStringList m_lru;
    QSet<QString> m_inFlight;
    QString m_dir;
    mutable int m_bytes = 0;
    int m_maxBytes;
};