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
    // Current live stream: declared by the renderers (LIVE badge /
    // "En directo", /next viewCount "watching now") and by the player response
    // (videoDetails.isLive). Persisted in the cache (videos.is_live) and the
    // thumbnails show "En directo" instead of the duration.
    bool isLive = false;
    // The REAL size of the thumbnail (the largest one): it tells whether the
    // video is VERTICAL, and a vertical video is a Short (a reel). 0 = the
    // renderer did not bring the size.
    int thumbWidth = 0;
    int thumbHeight = 0;
    // It IS a Short (a reel): a vertical video. It is persisted (videos.
    // is_short) so that the lists that come from the database ("Continuar
    // viendo", the history) can leave them out too.
    bool isShort = false;
    // Playlist / mix the row belongs to (RD…, PL…, OLAK…): a mix does NOT have
    // a video id of its own in the modern layout, it has the id of the
    // playlist, so the row is only playable through it. Empty = plain video.
    // When `id` IS the playlist (a card of Search or of the Channel's
    // Playlists tab) both fields carry it and isPlaylist() says so.
    QString playlistId;
    // Videos of the playlist, when the row IS a playlist (0 = unknown).
    int playlistVideos = 0;
    // The row is the PLAYLIST itself (Search / Channel card), not a video.
    bool isPlaylist() const { return !playlistId.isEmpty() && playlistId == id; }
    // Any row that represents a playlist or a mix (the card, or a mix entry of
    // "A continuación" that brings a video AND its mix). Those are never
    // played automatically: clicking one loads the whole playlist.
    bool isPlaylistEntry() const { return !playlistId.isEmpty(); }
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
    QString audioUrl;
    QString label;
    int height = 0;
    bool multipleUrls = false;
    // False only for streams BUILT by the app from finite media (the synthetic
    // AV1 HLS: an fMP4 with a VOD+ENDLIST playlist). Such a stream cannot be a
    // live stream by construction, so the live UI (EN VIVO, playhead at the
    // edge) is disabled for it even if the backend reports unstable durations
    // while loading. The other streams (YouTube master, filtered, edge) CAN be
    // live and keep true.
    bool canBeLive = true;
    // AUTHORITATIVE live signal (videoDetails.isLive from YouTube's player
    // response): true only if YouTube declares the media as a current live
    // stream. Lets us enter EN VIVO mode instantly, without waiting for the
    // "so far" duration to grow (and covers pure live streams with dur=0).
    bool isLive = false;
};

struct PlaylistInfo
{
    QString id;
    QString title;
    QString channel;
    QString thumbnailUrl;
    int videoCount = 0;
};

// Fallback thumbnail URL of a video, when the response/feed brings none. Only
// for valid YouTube ids (11 characters); empty otherwise.
inline QString fallbackThumbnailUrl(const QString &videoId)
{
    return videoId.size() == 11
        ? QStringLiteral("https://i.ytimg.com/vi/%1/hqdefault.jpg").arg(videoId)
        : QString();
}