#pragma once

#include <QJsonObject>
#include <QObject>

#include "hls_server.h"
#include "models.h"

class Yt : public QObject
{
    Q_OBJECT

public:
    explicit Yt(QObject *parent = nullptr);

    void search(const QString &query, int limit = 20);
    void trending(int limit = 30);
    void channelVideos(const QString &channelId, int limit = 20);
    void channelInfo(const QString &channelId);
    void getDetail(const QString &videoId);
    void playlistDetail(const QString &playlistId, int limit = 50);
    void resolveStream(const QString &videoId, int maxHeight);

    QString downloadVideoSelector(int maxHeight) const;
    static QString videoIdFromUrl(const QString &url);

signals:
    void searchFinished(const QVector<VideoInfo> &videos);
    void trendingFinished(const QVector<VideoInfo> &videos);
    void channelVideosFinished(const QVector<VideoInfo> &videos);
    void channelInfoReady(const ChannelInfo &channel);
    void detailReady(const VideoInfo &video, const QString &description,
                     qint64 likeCount, const QVector<FormatInfo> &formats);
    void playlistDetailReady(const PlaylistInfo &info,
                             const QVector<VideoInfo> &videos);
    void streamReady(const StreamInfo &stream);
    void error(const QString &message);

private:
    void execJson(QStringList args, std::function<void(const QJsonObject &)> done);
    void execLines(QStringList args, std::function<void(const QStringList &)> done);
    void getUrls(const QString &videoId, const QString &selector,
                 std::function<void(const QStringList &)> done);
    QString buildHlsMaster(const QString &videoId, const QString &videoUrl,
                           const QString &audioUrl);

    static QString qualityName(int height);
    static QString formatSelector(int maxHeight);
    static QVector<VideoInfo> parseVideos(const QJsonObject &root);
    static VideoInfo videoFromJson(const QJsonObject &j);
    static ChannelInfo channelFromJson(const QString &id, const QJsonObject &j);
    static void parsePlaylist(const QJsonObject &root, PlaylistInfo *info,
                              QVector<VideoInfo> *videos);
    static QString pickThumbnail(const QJsonArray &thumbs);
    static QString fallbackThumb(const QString &videoId);
    static QString publishedFrom(const QJsonObject &j);

    HlsServer m_hls;
};