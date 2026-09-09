#pragma once

#include <QString>
#include <QVector>

struct VideoInfo
{
    QString id;
    QString title;
    QString channel;
    QString channelId;
    QString thumbnailUrl;
    QString description;
    QString publishedAt;
    int durationSeconds = 0;
    int viewCount = 0;
};

struct ChannelInfo
{
    QString id;
    QString name;
    QString avatarUrl;
    QString description;
    long long subscriberCount = 0;
};

struct FormatInfo
{
    QString formatId;
    QString label;
    QString ext;
    int height = 0;
};

struct StreamInfo
{
    QString url;
    QString label;
    int height = 0;
    bool multipleUrls = false;
};

struct PlaylistInfo
{
    QString id;
    QString title;
    QString channel;
    QString thumbnailUrl;
    int videoCount = 0;
};