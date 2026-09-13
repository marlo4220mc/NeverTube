#pragma once

#include <QObject>
#include <QPixmap>
#include <QSet>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

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
    int maxWidth() const;

signals:
    void ready(const QString &key);

private slots:
    void onFinished(QNetworkReply *reply);

private:
    QString filePath(const QString &key) const;
public:
    void store(const QString &key, const QImage &image);
private:

    QNetworkAccessManager *m_nam = nullptr;
    QTimer *m_sweep = nullptr;
    mutable QHash<QString, QPixmap> m_cache;
    mutable QStringList m_lru;
    QSet<QString> m_inFlight;
    QString m_dir;
    int m_bytes = 0;
    int m_maxBytes;
};