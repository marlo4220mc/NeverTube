#pragma once

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QObject>
#include <QVector>

#include "hls_server.h"
#include "models.h"
#include "video_variant.h"
#include "yt_innertube.h"

// "A continuación" generation mode (selected via NEVERTUBE_NEXT_MODE:
// none | suggestions | radio). Lets you choose between:
//   none         -> no seeding (the queue stays as it came in),
//   suggestions  -> static /next: ~20 recommendations of the current video,
//   radio        -> RDAMVM<videoId> watch-playlist + continuation (YouTube
//                   Music style), regenerated according to the current video.
enum class NextMode
{
    None,
    Suggestions,
    Radio
};
NextMode nextMode();
void setNextMode(NextMode m);

class SettingsRepo;

class Yt : public QObject
{
    Q_OBJECT

public:
    explicit Yt(QObject *parent = nullptr);

    // visitorData persistence (settings): if Innertube gets a new one it is
    // saved here, and on startup it is reused to avoid the sw.js_data GET
    // (one RTT less before the POST /player).
    void setVisitorStorage(SettingsRepo *settings);

    void search(const QString &query, int limit = 20, int offset = 0);
    // Search type-ahead suggestions. It does not launch a search: it only asks
    // for the prefix suggestions and delivers them through suggestReady(). It
    // is a plain GET that does not touch the Innertube session (see
    // Innertube::fetchSuggest).
    void suggest(const QString &query);
    void trending(int limit = 30);
    void channelVideos(const QString &channelId, int limit = 20);
    void channelTabVideos(const QString &channelId, const QString &tab,
                          int limit = 100, int offset = 0);
    void channelInfo(const QString &channelId);
    void getDetail(const QString &videoId);
    void playlistDetail(const QString &playlistId, int limit = 50);
    // forceRefresh=true re-resolves with the POST /player even if there is a
    // session cache for the video (fresh signed URLs; see Innertube::fetchHls).
    void resolveStream(const QString &videoId, int maxHeight,
                       bool forceRefresh = false);

    // Asks for the RAW master playlist of a video (the multivariant one)
    // and emits it as masterBodyReady. It is what the downloader uses: it
    // needs the master as it is, to pick the variant and the audio rendition
    // itself, instead of the filtered one the player is served.
    //
    // It does NOT touch the player: it is a separate request path, and the
    // signals of the playback are not emitted.
    void requestMaster(const QString &videoId);
    // Asks for the ADAPTIVE FORMATS of a video (the ones with a direct URL,
    // the initRange and the indexRange of each one) and emits them as
    // adaptiveFormatsReady. It is what the downloader uses: it downloads the
    // resources by byte range, and it needs the index of each one.
    void requestAdaptiveFormats(const QString &videoId);
    // Rotates the Innertube session (visitorData + cached signed URLs): the
    // next resolveStream will use a NEW YouTube session. PlayerController uses
    // it in recovery after a gated session (403 mid-playback), where
    // re-resolving inside the same session does not help.
    void rotateStreamSession();
    // Triggers the load of the video's related ("A continuación") through
    // Innertube /next. The results arrive through relatedReady().
    void relatedVideos(const QString &videoId);
    // Translated detail of the current video (normal /next): title and
    // description in the requested "hl" language, to show in the watch page
    // without touching the queue.
    void currentDetail(const QString &videoId);
    // Radio mode (model 3): asks for the video's RDAMVM mix queue and, if the
    // mix has more batches, radioMore() chains the next one when it runs out.
    // They arrive through radioReady().
    void radioVideos(const QString &videoId);
    void radioMore();
    // Real Home through Innertube (browse FEwhat_to_watch). The results arrive
    // through homeReady(); empty = no feed. homeMore() asks for the continuation
    // if the response brought it.
    void home();
    void homeMore();
    bool homeHasContinuation() const;

    static QString videoIdFromUrl(const QString &url);

    // A fragment (subsegment) of the fMP4 according to the sidx box of the
    // indexRange.
    struct Av1Fragment
    {
        qint64 size = 0;
        double duration = 0.0; // seconds
    };

    // Text of the synthetic HLS media playlist for an AV1 adaptiveFormat
    // (no network or files): #EXT-X-MAP + #EXT-X-BYTERANGE segments with the
    // /seg.mp4 suffix and a distinct &nt per segment. Empty if there is no AV1
    // <= maxHeight with url/initRange. heightOut receives the chosen height.
    // If `fragments` is given, each segment equals a real fragment of the fMP4
    // (exact sidx boundaries): essential for the seek to work. Without it it
    // falls back to fixed ~2 MB chunks (valid only for sequential playback from
    // the start). Exposed for the selftest.
    static QString av1HlsPlaylist(const QJsonArray &adaptiveFormats,
                                  int maxHeight, int *heightOut,
                                  const QVector<Av1Fragment> &fragments = {},
                                  qint64 fragmentsStart = -1);

    // Parses the sidx box (bytes of the indexRange) and returns the fragments.
    // firstOffsetOut receives the offset (relative to the end of the indexRange)
    // of the first subsegment. Empty if there is no valid sidx.
    static QVector<Av1Fragment> av1ParseSidx(const QByteArray &indexBox,
                                             qint64 *firstOffsetOut);

    // Audio language preference for the filtered master. Empty (or "original")
    // = original track. A language code (e.g. "en-US" or "pt") prioritizes the
    // matching AI dubbing, with the original as fallback. Settings does not
    // configure it yet; it is left ready.
    void setAudioLanguage(const QString &lang);
    QString audioLanguage() const;
    // Codec profile of the edge master. Detected on the first playback
    // (hwdec-current per codec) and persisted in codec_profile:
    //   Modern = AV1 + VP9 (hardware with an AV1 decoder; AV1 is preferred and
    //            VP9 is the fallback at equal height).
    //   Vp9    = VP9 only (hardware without AV1 but with a VP9 decoder).
    //   Legacy = h264 only (no modern hardware; if in addition h264 decodes in
    //            software, MainWindow sets the "nohw" mode: 1080p cap and
    //            percentage-based initial buffer wait for 1080p).
    enum class CodecProfile { Modern, Vp9, Legacy };
    void setCodecProfile(CodecProfile profile);
    CodecProfile codecProfile() const { return m_codecProfile; }
    // Windows x86 build without mpv (NEVERTUBE_NO_MPV): AV1 decodes neither by
    // HW nor by SW in the i686 package. With this codecRankedScore NEVER picks
    // it: VP9 (software) always wins over a format that always fails.
    void setNoAv1(bool on);

signals:
    void searchFinished(const QString &query, const QVector<VideoInfo> &videos);
    void suggestReady(const QString &query, const QVector<Suggestion> &suggestions);
    void trendingFinished(const QVector<VideoInfo> &videos);
    void channelVideosFinished(const QVector<VideoInfo> &videos);
    void channelTabVideosFinished(const QString &channelId, const QString &tab,
                                  const QVector<VideoInfo> &videos);
    void channelInfoReady(const ChannelInfo &channel);
    void detailReady(const VideoInfo &video, const QString &description,
                     qint64 likeCount, const QVector<FormatInfo> &formats);
    // getDetail did not get valid metadata from the /next (video unavailable or
    // empty response).
    void detailFailed(const QString &videoId);
    void playlistDetailReady(const PlaylistInfo &info,
                             const QVector<VideoInfo> &videos);
    void streamReady(const StreamInfo &stream);
    // The RAW master playlist of a video, as YouTube serves it (the
    // multivariant one, with the variants and the #EXT-X-MEDIA renditions).
    // The downloader uses it to build its plan.
    void masterBodyReady(const QString &videoId, const QByteArray &masterBody);
    // The ADAPTIVE FORMATS of a video (the ones with a direct URL), as the
    // player response gives them. The downloader takes the AUDIO from here
    // and not from the master: the audio of the master is AAC in LATM (its
    // segments carry a PRIV box of Apple that says so) and the FFmpeg of
    // this system cannot mux that ("Error applying bitstream filters to an
    // output packet for stream #1: Not yet implemented in FFmpeg"). The
    // adaptive gives the same track as a clean m4a.
    void adaptiveFormatsReady(const QString &videoId, const QJsonArray &formats);
    // The master could not be fetched.
    void masterBodyFailed(const QString &videoId, const QString &reason);
    // Available video heights of the video (Innertube adaptiveFormats),
    // independent of the cap the profile asks for: the player's resolution menu
    // uses them to offer all the real qualities even if Auto asked for a low
    // one. Empty = indeterminate (the menu falls back to the cap of the loaded
    // stream).
    void streamHeightsReady(const QString &videoId, const QVector<int> &heights);
    // The VARIANTS of the video (height, codec and real size): what the
    // menu of the download offers. The Player knows them because the player
    // response carries the adaptiveFormats, so the menu does not have to
    // ask YouTube for anything.
    void streamVariantsReady(const QString &videoId,
                             const QVector<VideoVariant> &variants);
    void error(const QString &message);
    void resolveError(const QString &message);
    void relatedReady(const QString &videoId, const QVector<VideoInfo> &videos);
    void radioReady(const QString &videoId, const QVector<VideoInfo> &videos);
    // Translated detail of the current video (title and description) from the
    // normal /next, in the requested "hl" language. WatchPage uses it only when
    // the audio language is explicit (es/en/pt).
    void currentDetailReady(const QString &videoId, const QString &title,
                            const QString &description);
    void homeReady(const QVector<VideoInfo> &videos);
    void homeMoreReady(const QVector<VideoInfo> &videos);

private:
    void resolveViaInnertube(const QString &videoId, int maxHeight,
                             int preferredClient, bool forceRefresh = false);
    QString buildFilteredMaster(const QString &videoId,
                                const QByteArray &masterBody, int maxHeight);
    // Local synthetic HLS to play AV1 through the HLS path (YouTube does not
    // publish AV1 in its master: only VP9/h264). Each segment is a short Range
    // request (~2 MB) over the AV1 adaptive URL; YouTube's per-request throttle
    // does not apply to short bursts, so segmenting equals the speed of the real
    // HLS. Returns the local .m3u8 path or empty if there is no
    // AV1 <= maxHeight with initRange/url; writes heightOut with the height.
    QString buildAv1Hls(const QString &videoId,
                        const QJsonArray &adaptiveFormats, int maxHeight,
                        int *heightOut);
    // Builds the "edge" mode from the /player adaptiveFormats: picks a single
    // video (height <= maxHeight, h264 preferred) and a single audio (preferred
    // language), and returns the video's direct URL (mp4/fMP4). Writes
    // audioUrlOut with the direct audio track URL (for
    // PlayerWidget::setAudioFile). Returns an empty URL if there is no usable
    // combo (then the classic hlsManifestUrl → fetchMaster flow is used as a
    // fallback). Playing from a direct URL (not segment HLS) gives mpv exact
    // duration and reliable seek — single-segment HLS lost both (progress bar
    // jumping). 3 profiles: modern (AV1), vp9-mid (VP9 webm direct), legacy
    // (h264). Empty audioLang == original.
    QString buildEdgeMaster(const QString &videoId,
                            const QJsonArray &adaptiveFormats, int maxHeight,
                            const QString &audioLang, int durationSeconds,
                            QString *audioUrlOut);
    // Score of a video format by height + codec preference according to the
    // detected hardware profile (Modern: AV1>VP9>h264; Vp9: VP9>h264, AV1
    // penalized; Legacy: h264). The codec preference weighs ×100000, so the
    // preferred codec wins even if it is lower, and within it the tallest
    // height.
    int codecRankedScore(int height, const QString &mimeOrCodecs) const;

    static QString qualityName(int height);
    static int closestHeight(int requested, const QVector<int> &heights);
    // Height of the video buildEdgeMaster would pick (tallest height <= maxHeight
    // with a direct url, h264 preferred). -1 if there is no usable video.
    int edgeHeight(const QJsonArray &formats, int maxHeight);

    HlsServer m_hls;
    Innertube m_innertube;
    // Last search query: distinguishes the first page (fetchSearch) from the
    // continuation (fetchSearchMore) in Yt::search.
    QString m_searchQuery;
    SettingsRepo *m_visitorStore = nullptr;
    int m_resolveGen = 0;
    QString m_audioLang;
    CodecProfile m_codecProfile = CodecProfile::Legacy;
    bool m_noAv1 = false; // x86 (NEVERTUBE_NO_MPV): AV1 impossible, do not pick it
    // Innertube client that managed to play each video (index >= 0). Used to
    // give it priority when changing resolution (re-resolving the same video).
    QHash<QString, int> m_winningClient;
};

// Self-test of the real feed chain in Yt: channel through Innertube + trending →
// channelVideosFinished/trendingFinished signals. Checks that a result arrives
// within a deadline, without hangs. It does not touch the player or SQLite.
bool feedsChainSelfTest();

// Self-test of the Innertube SEARCH (Yt::search): launches a real search
// (network) and requires videos with a non-empty id.
bool searchSelfTest();

// Offline self-test of the synthetic AV1 HLS: validates the generated playlist
// (EXT-X-MAP/BYTERANGE, /seg.mp4 suffix, distinct and consecutive &nt per
// segment, choice of the tallest height <= maxHeight and correct discard
// without AV1). No network, no files and no player.
bool av1HlsSelfTest();