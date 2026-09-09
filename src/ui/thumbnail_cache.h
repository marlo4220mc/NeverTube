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
    explicit ThumbnailCache(QObject *parent = nullptr);
    ~ThumbnailCache() override;

    QPixmap pixmap(const QString &key) const;
    void request(const QString &key, const QString &url);
    void requestChannelAvatar(const QString &channelId, const QString &url);
    int clear();
    int sizeBytes() const;

signals:
    void ready(const QString &key);

private slots:
    void onFinished(QNetworkReply *reply);

private:
    QString filePath(const QString &key) const;
    void store(const QString &key, const QImage &image);

    QNetworkAccessManager *m_nam = nullptr;
    QTimer *m_sweep = nullptr;
    mutable QHash<QString, QPixmap> m_cache;
    mutable QStringList m_lru;
    QSet<QString> m_inFlight;
    QString m_dir;
    int m_bytes = 0;

    static constexpr int kMaxBytes = 32 * 1024 * 1024;
};