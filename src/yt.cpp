#include "yt.h"

#include <QDateTime>
#include <QtMath>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>

#include "debug.h"
#include "db/repositories.h"
#include "i18n.h"

namespace {

// Sentinel for m_winningClient (Innertube client indexes are >= 0).
constexpr int kResolveUnknown = -2;    // no winner registered yet

// Returns the value of the given EXT-X-MEDIA attribute (e.g. LANGUAGE, NAME).
QString mediaAttr(const QString &line, const QLatin1String attr)
{
    const QString needle = QLatin1String(attr.data()) + QStringLiteral("=\"");
    const int pos = line.indexOf(needle);
    if (pos < 0)
        return QString();
    const int start = pos + needle.size();
    const int end = line.indexOf(QLatin1Char('"'), start);
    if (end < 0)
        return QString();
    return line.mid(start, end - start);
}

bool isOriginalAudio(const QString &line)
{
    return line.contains(QLatin1String("acont=original"))
        || line.contains(QLatin1String("acont%3Doriginal"));
}

namespace {
NextMode initNextMode() {
    const QByteArray raw = qgetenv("NEVERTUBE_NEXT_MODE").trimmed().toLower();
    if (raw == "none")
        return NextMode::None;
    if (raw == "suggestions")
        return NextMode::Suggestions;
    return NextMode::Radio;
}
NextMode g_nextMode = initNextMode();
}
QStringList selectAudioTracks(const QStringList &audioLines,
                              const QString &prefLang)
{
    QHash<QString, QStringList> byGroup;
    QList<QString> groupOrder;
    for (const QString &line : audioLines) {
        const QString group = mediaAttr(line, QLatin1String("GROUP-ID"));
        if (group.isEmpty())
            continue;
        if (!byGroup.contains(group))
            groupOrder.append(group);
        byGroup[group].append(line);
    }

    QStringList out;
    for (const QString &group : std::as_const(groupOrder)) {
        const QStringList tracks = byGroup.value(group);
        QString original;
        QString preflight;
        QStringList dubbed;
        for (const QString &t : tracks) {
            if (isOriginalAudio(t)) {
                original = t;
            } else {
                dubbed.append(t);
                if (preflight.isEmpty() && !prefLang.isEmpty()) {
                    const QString lang = mediaAttr(t, QLatin1String("LANGUAGE"));
                    if (lang.startsWith(prefLang, Qt::CaseInsensitive))
                        preflight = t;
                }
            }
        }
        if (!prefLang.isEmpty() && !preflight.isEmpty()) {
            // Preference found: that dub first (mpv picks it with
            // alang). The rest of the dubs in the group are discarded so as not to
            // bloat the master (it slows down video startup).
            out.append(preflight);
        }
        if (!original.isEmpty() && !out.contains(original)) {
            out.append(original);
        } else if (original.isEmpty() && !tracks.isEmpty()) {
            // No recognizable original track (videos without `acont=original`):
            // keep the first of the group so as not to leave the video without audio.
            out.append(tracks.first());
        }
    }
    return out;
}

} // namespace

Yt::Yt(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<QVector<VideoInfo>>("QVector<VideoInfo>");
    qRegisterMetaType<ChannelInfo>("ChannelInfo");
    qRegisterMetaType<QVector<FormatInfo>>("QVector<FormatInfo>");
    qRegisterMetaType<StreamInfo>("StreamInfo");
    qRegisterMetaType<PlaylistInfo>("PlaylistInfo");

    // /next: direct forwarding from Innertube to the rest of the app.
    connect(&m_innertube, &Innertube::relatedReady, this,
            &Yt::relatedReady);
    connect(&m_innertube, &Innertube::radioReady, this,
            &Yt::radioReady);
    connect(&m_innertube, &Innertube::currentDetailReady, this,
            &Yt::currentDetailReady);
    connect(&m_innertube, &Innertube::homeReady, this,
            &Yt::homeReady);
    connect(&m_innertube, &Innertube::homeMoreReady, this,
            &Yt::homeMoreReady);
    connect(&m_innertube, &Innertube::channelVideosReady, this,
            [this](const QString &, const QVector<VideoInfo> &videos) {
                emit channelVideosFinished(videos);
            });
    connect(&m_innertube, &Innertube::trendingReady, this,
            &Yt::trendingFinished);
    // Search, channel info/tabs, video detail and playlist via
    // Innertube: they are forwarded to the Yt signals the UI consumes.
    connect(&m_innertube, &Innertube::searchReady, this,
            [this](const QString &query, const QVector<VideoInfo> &videos,
                   bool) { emit searchFinished(query, videos); });
    connect(&m_innertube, &Innertube::suggestReady, this, &Yt::suggestReady);
    connect(&m_innertube, &Innertube::channelInfoReady, this,
            &Yt::channelInfoReady);
    connect(&m_innertube, &Innertube::channelTabReady, this,
            [this](const QString &channelId, const QString &tab,
                   const QVector<VideoInfo> &videos, bool) {
                emit channelTabVideosFinished(channelId, tab, videos);
            });
    connect(&m_innertube, &Innertube::videoDetailReady, this,
            [this](const VideoInfo &video, const QString &description,
                   qint64 likeCount) {
                if (video.title.isEmpty()) {
                    emit detailFailed(video.id);
                    return;
                }
                emit detailReady(video, description, likeCount,
                                 QVector<FormatInfo>());
            });
    connect(&m_innertube, &Innertube::playlistReady, this,
            &Yt::playlistDetailReady);
    // Video heights available for the resolutions menu:
    // they come from the HLS master (adaptiveFormats' videoHeights) and, as a
    // fallback, from /player (formatsReady). They do not affect the resolved
    // resolution; they only inform the UI so Auto does not hide qualities the video does have.
    connect(&m_innertube, &Innertube::hlsReady, this,
            [this](const QString &videoId, const QString &,
                   const QVector<int> &heights) {
                if (!heights.isEmpty())
                    emit streamHeightsReady(videoId, heights);
            });
    connect(&m_innertube, &Innertube::formatsReady, this,
            [this](const QString &videoId, const QJsonArray &formats, int) {
                QVector<int> heights;
                for (const QJsonValue &val : formats) {
                    const QJsonObject f = val.toObject();
                    const QString mime =
                        f.value(QStringLiteral("mimeType")).toString();
                    const int h = f.value(QStringLiteral("height")).toInt(0);
                    if (mime.startsWith(QLatin1String("video/")) && h > 0
                        && !heights.contains(h))
                        heights.append(h);
                }
                std::sort(heights.begin(), heights.end());
                if (!heights.isEmpty())
                    emit streamHeightsReady(videoId, heights);
                // The VARIANTS (height, codec and real size): the menu of
                // the download offers them. They come with the player
                // response, so nothing is asked for a second time.
                const QVector<VideoVariant> variants =
                    videoVariantsOf(formats);
                if (!variants.isEmpty())
                    emit streamVariantsReady(videoId, variants);
            });
}

void Yt::setVisitorStorage(SettingsRepo *settings)
{
    m_visitorStore = settings;
    if (!settings)
        return;
    // Reuse a persisted, non-expired visitorData: it avoids the startup
    // sw.js_data GET and one RTT before the POST /player.
    const QString saved =
        settings->getString(QStringLiteral("visitorData"));
    const qlonglong savedExp =
        settings->getInt(QStringLiteral("visitorDataExpiresAt"), 0);
    if (!saved.isEmpty())
        m_innertube.seedVisitorData(saved, savedExp);
    // and save the new ones that arrive at runtime.
    connect(&m_innertube, &Innertube::visitorDataReady, this,
            [this](const QString &data, qlonglong expiresAt) {
                if (!m_visitorStore)
                    return;
                m_visitorStore->set(QStringLiteral("visitorData"), data);
                m_visitorStore->set(QStringLiteral("visitorDataExpiresAt"),
                                    QString::number(expiresAt));
            });
}

NextMode nextMode()
{
    return g_nextMode;
}

void setNextMode(NextMode m)
{
    g_nextMode = m;
}

void Yt::relatedVideos(const QString &videoId)
{
    m_innertube.fetchNext(videoId);
}

void Yt::currentDetail(const QString &videoId)
{
    m_innertube.fetchCurrentDetail(videoId);
}

void Yt::radioVideos(const QString &videoId)
{
    m_innertube.fetchRadio(videoId);
}

void Yt::radioMore()
{
    m_innertube.fetchRadioMore();
}

void Yt::home()
{
    m_innertube.fetchHome();
}

void Yt::homeMore()
{
    m_innertube.fetchHomeMore();
}

bool Yt::homeHasContinuation() const
{
    return m_innertube.hasHomeContinuation();
}

QString Yt::videoIdFromUrl(const QString &url)
{
    if (url.size() == 11 && !url.contains(QStringLiteral("youtube")))
        return url;
    const QUrl parsed(url);
    const QUrlQuery query(parsed);
    const QString v = query.queryItemValue(QStringLiteral("v"));
    if (v.size() == 11)
        return v;
    const QString path = parsed.path();
    if (path.startsWith(QStringLiteral("/shorts/")))
        return path.mid(8, 11);
    if (parsed.host() == QLatin1String("youtu.be"))
        return path.mid(1, 11);
    return QString();
}

void Yt::setAudioLanguage(const QString &lang)
{
    const QString clean = lang.trimmed();
    m_audioLang = clean.compare(QLatin1String("original"),
                                Qt::CaseInsensitive) == 0
            ? QString()
            : clean;
    // Propagate the language to Innertube so /next and /player deliver
    // translated titles and descriptions (hl). original keeps "es" by
    // default (avoids automatic auto-translation to English).
    const QString hl = m_audioLang.isEmpty() ? QStringLiteral("es")
                                             : m_audioLang;
    m_innertube.setHl(hl);
}

QString Yt::audioLanguage() const
{
    return m_audioLang;
}

void Yt::setCodecProfile(CodecProfile profile)
{
    m_codecProfile = profile;
}

void Yt::setNoAv1(bool on)
{
    m_noAv1 = on;
}

void Yt::search(const QString &query, int limit, int offset)
{
    // Innertube: offset 0 = first page; offset > 0 = continuation.
    if (offset <= 0) {
        m_searchQuery = query;
        m_innertube.fetchSearch(query, limit);
    } else {
        m_innertube.fetchSearchMore(limit);
    }
}

void Yt::suggest(const QString &query)
{
    m_innertube.fetchSuggest(query);
}

void Yt::trending(int limit)
{
    Q_UNUSED(limit);
    m_innertube.fetchTrending();
}

void Yt::channelVideos(const QString &channelId, int limit)
{
    Q_UNUSED(limit);
    // Innertube: browse of the channel's "Vídeos" tab (compactVideoRenderer).
    m_innertube.fetchChannelVideos(channelId);
}

void Yt::channelTabVideos(const QString &channelId, const QString &tab, int limit,
                          int offset)
{
    if (offset <= 0)
        m_innertube.fetchChannelTab(channelId, tab, limit);
    else
        m_innertube.fetchChannelTabMore(channelId, tab, limit);
}

void Yt::channelInfo(const QString &channelId)
{
    m_innertube.fetchChannelInfo(channelId);
}

void Yt::getDetail(const QString &videoId)
{
    // Video detail (translated title, description, views, date and likes)
    // from Innertube's /next.
    m_innertube.fetchVideoDetail(videoId);
}

void Yt::playlistDetail(const QString &playlistId, int limit)
{
    // Innertube: browse VL<id> (header + playlistVideoRenderer).
    m_innertube.fetchPlaylist(playlistId, limit);
}

void Yt::resolveStream(const QString &videoId, int maxHeight,
                       bool forceRefresh)
{
    const int winning = m_winningClient.value(videoId, kResolveUnknown);
    NT_DEBUG("E2E resolveStream id=%s maxH=%d winner=%d refresh=%d\n",
             videoId.toUtf8().constData(), maxHeight, winning,
             int(forceRefresh));
    resolveViaInnertube(videoId, maxHeight, winning, forceRefresh);
}

void Yt::rotateStreamSession()
{
    m_innertube.rotateSession();
}

// The ADAPTIVE FORMATS of a video, for the downloader.
//
// The player path (resolveStream) serves a FILTERED master: it keeps one
// variant and rewrites the audio renditions, and it writes the result to a
// file for the local HLS server. The downloader needs the formats as they
// are: it picks the variant and the audio track itself, and it downloads
// both BY BYTE RANGE (the sidx of each one says where every fragment
// starts).
//
// So this is a separate request path that does NOT touch the player.
void Yt::requestAdaptiveFormats(const QString &videoId, bool forceRefresh)
{
    auto *guard = new QObject(this);
    auto *innTube = &m_innertube;

    connect(innTube, &Innertube::failed, guard,
            [this, guard, videoId](const QString &vid, const QString &reason) {
                if (vid != videoId)
                    return;
                guard->deleteLater();
                emit masterBodyFailed(videoId, reason);
            });

    connect(innTube, &Innertube::formatsReady, guard,
            [this, guard, videoId](const QString &vid, const QJsonArray &formats,
                                  int) {
                if (vid != videoId)
                    return;
                guard->deleteLater();
                emit adaptiveFormatsReady(videoId, formats);
            });
    connect(innTube, &Innertube::formatsFailed, guard,
            [this, guard, videoId](const QString &vid) {
                if (vid != videoId)
                    return;
                guard->deleteLater();
                emit masterBodyFailed(videoId, QStringLiteral("formats"));
            });

    // The player response gives the formats. It is the OWN path of the
    // downloader (not the fetchHls of the player): the player may have a
    // request in flight and the single-flight of fetchHls would then do
    // nothing, leaving the downloader waiting forever.
    innTube->fetchAdaptiveFormats(videoId, forceRefresh);
}

void Yt::resolveViaInnertube(const QString &videoId, int maxHeight,
                             int preferredClient, bool forceRefresh)
{
    auto *guard = new QObject(this);
    auto *innTube = &m_innertube;
    const int gen = ++m_resolveGen;

    QMetaObject::Connection guardFail;
    guardFail = connect(innTube, &Innertube::failed, guard,
            [this, guard, videoId, maxHeight, gen](const QString &vid,
                                                   const QString &reason) {
                if (vid != videoId || gen != m_resolveGen)
                    return;
                guard->deleteLater();
                qWarning("yt: innertube falló para %s (%s)",
                         vid.toUtf8().constData(), reason.toUtf8().constData());
                emit resolveError(
                    nt::tr("error.resolve").arg(reason));
            });

    // Primary HLS (YouTube master, no throttle), edge as fallback:
    // handlePlayer emits formatsReady and hlsReady at the same instant, but the
    // HLS flow needs a round-trip (fetchMaster) before emitting streamReady.
    // The edge (direct adaptive URL) is ~32x slower (YouTube throttle)
    // but does not require that round-trip. So as not to lose the speed of HLS, we
    // let it win: formatsReady only STORES the edge combo as a fallback
    // (m_edgeFallback) without emitting it; masterReady wins if the master is
    // filterable, and if not, the stored edge is emitted.
    QSharedPointer<QString> edgeUrl(new QString),
                                     edgeAudioUrl(new QString);
    QSharedPointer<int> edgeH(new int(0));
    // Synthetic AV1 HLS (if the profile prefers it): it is built in
    // formatsReady and wins in hlsReady, before requesting YouTube's master.
    QSharedPointer<QString> av1Hls(new QString);
    QSharedPointer<int> av1H(new int(0));
    connect(innTube, &Innertube::formatsReady, guard,
            [this, guard, innTube, videoId, maxHeight, gen, edgeUrl,
             edgeAudioUrl, edgeH, av1Hls, av1H](const QString &vid,
                                   const QJsonArray &formats, int dur) {
                if (vid != videoId || gen != m_resolveGen)
                    return;
                if (m_codecProfile == CodecProfile::Modern) {
                    int ah = 0;
                    const QString p =
                        buildAv1Hls(videoId, formats, maxHeight, &ah);
                    if (!p.isEmpty()) {
                        *av1Hls = p;
                        *av1H = ah;
                    }
                }
                QString audioUrl;
                const QString videoUrl =
                    buildEdgeMaster(videoId, formats, maxHeight, m_audioLang,
                                    dur, &audioUrl);
                if (videoUrl.isEmpty())
                    return; // no combo: only the HLS flow covers it
                *edgeUrl = videoUrl;
                *edgeAudioUrl = audioUrl;
                *edgeH = edgeHeight(formats, maxHeight);
                NT_DEBUG("E2E edge respaldo %dp %s\n", *edgeH,
                         videoUrl.toUtf8().constData());
            });

    // Emits the AV1 HLS if formatsReady built it (always before
    // hlsReady) and there is audio. Returns true if streamReady was already emitted.
    const auto emitAv1 = [this, innTube, videoId, av1Hls, av1H,
                          edgeAudioUrl]() -> bool {
        if (av1Hls->isEmpty() || edgeAudioUrl->isEmpty())
            return false;
        const QString origin = m_hls.start();
        if (origin.isEmpty())
            return false;
        StreamInfo s;
        s.url = origin + QLatin1Char('/') + videoId + QLatin1String(".m3u8");
        s.audioUrl = *edgeAudioUrl;
        s.label = QString::number(*av1H) + QLatin1Char('p');
        s.height = *av1H;
        s.multipleUrls = false;
        // Built from a finite fMP4 (VOD+ENDLIST): it is NEVER a live stream,
        // even though mpv reports unstable durations while it loads the ranges.
        s.canBeLive = false;
        m_winningClient[videoId] = innTube->lastWinClient();
        NT_DEBUG("E2E av1-hls emit %dp url=%s\n", *av1H,
                 s.url.toUtf8().constData());
        emit streamReady(s);
        return true;
    };

    connect(innTube, &Innertube::hlsReady, guard,
            [this, guard, guardFail, innTube, videoId, maxHeight, gen,
             edgeUrl, edgeAudioUrl, edgeH, emitAv1](
                const QString &vid, const QString &hlsUrl,
                const QVector<int> &heights) {
                if (vid != videoId || gen != m_resolveGen)
                    return;
                // AV1 over HLS first: YouTube's master does not carry AV1 (only
                // VP9/h264), so if there is synthetic AV1 it is emitted right away and
                // the fetchMaster round-trip is avoided.
                if (emitAv1()) {
                    guard->deleteLater();
                    return;
                }
                // Primary HLS: the edge is only emitted as fallback if the master
                // is not filterable (in masterReady) or if innertube fails.
                if (heights.isEmpty()) {
                    // No height table: deliver the master unfiltered
                    // (previous behavior).
                    guard->deleteLater();
                    m_winningClient[videoId] = innTube->lastWinClient();
                    StreamInfo s;
                    s.url = hlsUrl;
                    s.label = qualityName(maxHeight) + QLatin1Char('p');
                    s.height = maxHeight;
                    s.multipleUrls = false;
                    s.isLive = innTube->lastIsLive();
                    emit streamReady(s);
                    return;
                }
                const int wanted = closestHeight(maxHeight, heights);
                // From here on the failure is handled by filterGuard; the
                // initial guard is released to avoid a double fallback.
                disconnect(guardFail);
                guard->deleteLater();
                // A second guard for the master's filtering phase.
                auto *filterGuard = new QObject(this);
                QMetaObject::Connection masterConn;
                masterConn = connect(
                    innTube, &Innertube::masterReady, filterGuard,
                    [this, filterGuard, innTube, videoId, wanted, gen,
                     edgeUrl, edgeAudioUrl, edgeH](
                        const QString &fid, const QByteArray &masterBody) {
                        if (fid != videoId || gen != m_resolveGen)
                            return;
                        const QString abs = buildFilteredMaster(
                            videoId, masterBody, wanted);
                        if (abs.isEmpty()) {
                            // Master not filterable: if there is a stored edge,
                            // deliver it; if not, there is no stream.
                            filterGuard->deleteLater();
                            if (edgeUrl->isEmpty()) {
                                qWarning("yt: master sin variantes filtrables para %s",
                                         videoId.toUtf8().constData());
                                emit resolveError(QStringLiteral(
                                    "No se pudo preparar el stream del vídeo."));
                                return;
                            }
                            StreamInfo se;
                            se.url = *edgeUrl;
                            se.audioUrl = *edgeAudioUrl;
                            se.label =
                                QString::number(*edgeH) + QLatin1Char('p');
                            se.height = *edgeH;
                            se.multipleUrls = false;
                            se.isLive = innTube->lastIsLive();
                            m_winningClient[videoId] =
                                innTube->lastWinClient();
                            NT_DEBUG("E2E edge respaldo %dp url=%s\n",
                                     *edgeH,
                                     edgeUrl->toUtf8().constData());
                            emit streamReady(se);
                            return;
                        }
                        const QString origin = m_hls.start();
                        StreamInfo s;
                        s.url = origin + QLatin1Char('/') + videoId
                                + QLatin1String(".m3u8");
                        s.label = QString::number(wanted) + QLatin1Char('p');
                        s.height = wanted;
                        s.multipleUrls = false;
                        s.isLive = innTube->lastIsLive();
                        m_winningClient[videoId] = innTube->lastWinClient();
                        NT_DEBUG("E2E hls filtrado a %dp url=%s\n", wanted,
                                 s.url.toUtf8().constData());
                        filterGuard->deleteLater();
                        emit streamReady(s);
                    });
                // Failure in the master phase: if there is a stored edge,
                // deliver it; if not, there is no stream.
                QMetaObject::Connection masterFail;
                masterFail = connect(
                    innTube, &Innertube::failed, filterGuard,
                    [this, filterGuard, masterConn, innTube, videoId, maxHeight, gen,
                     edgeUrl, edgeAudioUrl, edgeH](
                        const QString &fid, const QString &reason) {
                        if (fid != videoId || gen != m_resolveGen)
                            return;
                        disconnect(masterConn);
                        filterGuard->deleteLater();
                        if (!edgeUrl->isEmpty()) {
                            StreamInfo se;
                            se.url = *edgeUrl;
                            se.audioUrl = *edgeAudioUrl;
                            se.label =
                                QString::number(*edgeH) + QLatin1Char('p');
                            se.height = *edgeH;
                            se.multipleUrls = false;
                            se.isLive = innTube->lastIsLive();
                            m_winningClient[videoId] = innTube->lastWinClient();
                            NT_DEBUG("E2E edge respaldo %dp url=%s\n", *edgeH,
                                     edgeUrl->toUtf8().constData());
                            emit streamReady(se);
                            return;
                        }
                        qWarning("yt: innertube falló al filtrar master (%s)",
                                 reason.toUtf8().constData());
                        emit resolveError(QStringLiteral(
                            "No se pudo preparar el stream del vídeo."));
                    });
                Q_UNUSED(masterFail)
                innTube->fetchMaster(vid, hlsUrl);
            });

    m_innertube.fetchHls(videoId, preferredClient, forceRefresh);
}

QString Yt::qualityName(int height)
{
    if (height <= 144)
        return QStringLiteral("144");
    if (height <= 240)
        return QStringLiteral("240");
    if (height <= 360)
        return QStringLiteral("360");
    if (height <= 480)
        return QStringLiteral("480");
    if (height <= 720)
        return QStringLiteral("720");
    return QStringLiteral("1080");
}

// Picks the height to request: exactly the requested one if it exists; if not,
// the immediately lower available one; it never goes up automatically unless
// no lower one exists (last resort).
int Yt::closestHeight(int requested, const QVector<int> &heights)
{
    if (heights.isEmpty())
        return requested;
    if (std::binary_search(heights.begin(), heights.end(), requested))
        return requested;
    int lower = -1;
    for (int h : heights) {
        if (h < requested && h > lower)
            lower = h;
    }
    if (lower > 0)
        return lower;
    int upper = std::numeric_limits<int>::max();
    for (int h : heights) {
        if (h > requested && h < upper)
            upper = h;
    }
    return upper == std::numeric_limits<int>::max() ? requested : upper;
}

// Score of a video format: codec preference (×100000) + height.
// Reno: AV1 > VP9 > h264. The preference weighs more than the height, so the
// codec the GPU decodes in hardware wins even if its variant is lower
// (e.g. Legacy picks h264 1080 before VP9 2160, which would go to software).
int Yt::codecRankedScore(int height, const QString &mimeOrCodecs) const
{
    const bool isH264 = mimeOrCodecs.contains(QLatin1String("avc1"))
                        || mimeOrCodecs.contains(QLatin1String("avc3"));
    const bool isAV1 = mimeOrCodecs.contains(QLatin1String("av01"));
    // webm VP9 may come as codecs="vp9" or codecs="vp09.00.10.08".
    const bool isVP9 = mimeOrCodecs.contains(QLatin1String("vp09"))
                       || mimeOrCodecs.contains(QLatin1String("\"vp9\""));
    int rank = 0;
    switch (m_codecProfile) {
    case CodecProfile::Modern:
        if (isAV1) rank = 2;
        else if (isVP9) rank = 1;
        break;
    case CodecProfile::Vp9:
        if (isVP9) rank = 2;
        else if (isAV1) rank = -1; // AV1 without hwdec = expensive software, avoid it
        break;
    case CodecProfile::Legacy:
    default:
        rank = isH264 ? 2 : 0;
        break;
    }
    // x86 build without mpv (NEVERTUBE_NO_MPV): AV1 decodes neither by HW nor by
    // SW in the i686 package — FFmpeg 9's native decoder does not support native
    // decode ('the av1 decoder doesn't support native decode'; the package does
    // not ship libdav1d/libaom). Never pick it even if it is the highest height:
    // VP9 (software) and h264 are always better than a format that always fails.
    // The flag is set by MainWindow only in that build (Linux without mpv; VAAPI can
    // give AV1 HW there, so it is not enabled there).
    if (m_noAv1 && isAV1)
        rank = -1;
    return rank * 100000 + height;
}

// The highest height <= maxHeight among the adaptiveFormats with a direct URL,
// according to the preferred codec profile (same rule as buildEdgeMaster).
// -1 if there is no video.
int Yt::edgeHeight(const QJsonArray &formats, int maxHeight)
{
    int best = -1;
    int bestScore = std::numeric_limits<int>::min();
    for (const QJsonValue &val : formats) {
        const QJsonObject f = val.toObject();
        if (f.value(QStringLiteral("url")).toString().isEmpty())
            continue;
        const QString mime = f.value(QStringLiteral("mimeType")).toString();
        const int h = f.value(QStringLiteral("height")).toInt();
        if (!mime.startsWith(QLatin1String("video/")) || h <= 0)
            continue;
        if (h > maxHeight)
            continue;
        const int score = codecRankedScore(h, mime);
        if (score > bestScore) {
            bestScore = score;
            best = h;
        }
    }
    return best;
}
// Picks a single video (the highest height <= maxHeight, preferring h264 for
// light decoding on modest machines) and a single audio (the one that best
// matches audioLang; empty audioLang == original). Returns the direct video
// URL and writes the audio one to audioUrlOut (mpv merges them
// via --audio-file). mp4/fMP4 and webm/VP9 work by direct URL (webm does not
// support single-segment HLS). Playing by direct URL gives mpv
// exact duration and reliable seek: single-segment HLS lost both and
// made the progress bar jump. Returns empty if there is no usable
// combination (no URL in the formats, or no audio/video).
QString Yt::buildEdgeMaster(const QString &videoId,
                            const QJsonArray &adaptiveFormats, int maxHeight,
                            const QString &audioLang, int durationSeconds,
                            QString *audioUrlOut)
{
    Q_UNUSED(videoId);
    Q_UNUSED(durationSeconds);
    if (audioUrlOut)
        audioUrlOut->clear();
    if (adaptiveFormats.isEmpty())
        return QString();

    QJsonObject bestVideo, bestAudio;
    int bestVideoH = 0;
    int bestScore = std::numeric_limits<int>::min();
    int bestAudioScore = -1;
    const bool wantOriginal = audioLang.isEmpty()
                              || audioLang == QLatin1String("original");

    for (const QJsonValue &val : adaptiveFormats) {
        const QJsonObject f = val.toObject();
        if (f.value(QStringLiteral("url")).toString().isEmpty())
            continue; // signed entries (signatureCipher) without decrypting
        const QString mime = f.value(QStringLiteral("mimeType")).toString();
        const int h = f.value(QStringLiteral("height")).toInt();
        if (mime.startsWith(QLatin1String("video/")) && h > 0) {
            if (h > maxHeight)
                continue;
            const int score = codecRankedScore(h, mime);
            if (score > bestScore) {
                bestScore = score;
                bestVideo = f;
                bestVideoH = h;
            }
        } else if (mime.startsWith(QLatin1String("audio/"))) {
            const QJsonObject track =
                f.value(QStringLiteral("audioTrack")).toObject();
            const QString lang =
                track.value(QStringLiteral("languageCode")).toString();
            const QString trackId =
                track.value(QStringLiteral("id")).toString();
            const QString displayName =
                track.value(QStringLiteral("displayName")).toString();
            const bool isDefault =
                track.value(QStringLiteral("audioIsDefault")).toBool();
            // The id prefix is the language ("en.4" → "en", "zh-Hant.3").
            QString idLang = trackId;
            const int dot = idLang.indexOf(QLatin1Char('.'));
            if (dot > 0)
                idLang.truncate(dot);
            bool match = false;
            if (wantOriginal) {
                // "original": the displayName marks it ("Inglés original"),
                // and acont=original distinguishes the base track from the dub.
                if (displayName.contains(QLatin1String("original"),
                                         Qt::CaseInsensitive))
                    match = true;
                else if (isDefault && lang.isEmpty()
                         && !displayName.isEmpty())
                    match = true; // single real audio track of the video
                else if (lang.isEmpty() && trackId.isEmpty()
                         && displayName.isEmpty()) {
                    // Without audioTrack (player without multiaudio): assume original
                    // only if it is the only audio track in the group.
                    match = true;
                }
            } else {
                const QString pref = audioLang.trimmed().toLower();
                match = (!lang.isEmpty()
                         && lang.startsWith(pref, Qt::CaseInsensitive))
                        || (!idLang.isEmpty()
                            && idLang.startsWith(pref, Qt::CaseInsensitive));
            }
            if (!match)
                continue;
            int score = 100;
            if (wantOriginal && displayName.contains(
                                    QLatin1String("original"),
                                    Qt::CaseInsensitive))
                score += 10; // the explicit original takes precedence over the default
            const int abr = f.value(QStringLiteral("bitrate")).toInt();
            if (abr > 0)
                score += qMin(abr / 1000, 200);
            if (score > bestAudioScore) {
                bestAudioScore = score;
                bestAudio = f;
            }
        }
    }

    // Fallback in case the main combo gave no audio: the requested language does not
    // exist in this video (audioLang with a different audioTrack) or the original
    // is not marked. Contract of the settings: "if the language does not exist, the
    // original audio is used" — NEVER any dub. Order: track marked
    // "original" > track isDefault > track without audioTrack (single-track video)
    // > first audio track (last resort, e.g. a regional dub without a
    // marker). Without this step the video would be muted.
    if (bestAudio.isEmpty()) {
        for (int pass = 0; pass < 4 && bestAudio.isEmpty(); ++pass) {
            for (const QJsonValue &val : adaptiveFormats) {
                const QJsonObject f = val.toObject();
                if (f.value(QStringLiteral("url")).toString().isEmpty())
                    continue;
                if (!f.value(QStringLiteral("mimeType"))
                          .toString()
                          .startsWith(QLatin1String("audio/")))
                    continue;
                const QJsonObject track =
                    f.value(QStringLiteral("audioTrack")).toObject();
                const QString displayName =
                    track.value(QStringLiteral("displayName")).toString();
                const bool isDefault =
                    track.value(QStringLiteral("audioIsDefault")).toBool();
                const bool noTrack = displayName.isEmpty()
                                     && track.value(QStringLiteral("languageCode"))
                                                  .toString()
                                                  .isEmpty()
                                     && track.value(QStringLiteral("id"))
                                                  .toString()
                                                  .isEmpty();
                const bool take = pass == 0
                        ? displayName.contains(QLatin1String("original"),
                                               Qt::CaseInsensitive)
                        : pass == 1 ? isDefault : pass == 2 ? noTrack : true;
                if (take) {
                    bestAudio = f;
                    break;
                }
            }
        }
    }

    if (bestVideo.isEmpty() || bestAudio.isEmpty())
        return QString();

    const QString videoUrl = bestVideo.value(QStringLiteral("url")).toString();
    QString audioUrl = bestAudio.value(QStringLiteral("url")).toString();
    if (videoUrl.isEmpty() || audioUrl.isEmpty())
        return QString();

    if (audioUrlOut)
        *audioUrlOut = audioUrl;
    return videoUrl;
}

// Filters YouTube's HLS master leaving only the variants with
// RESOLUTION= height <= maxHeight. The abbreviated one is served from the local
// HlsServer so the player only sees the allowed qualities.
// The audio tracks are chosen with selectAudioTracks: with empty m_audioLang
// (by default) only the original; the publishing order defines which one mpv
// picks (it picks the first of each group).
QString Yt::buildFilteredMaster(const QString &videoId,
                                const QByteArray &masterBody, int maxHeight)
{
    if (masterBody.isEmpty())
        return QString();

    QStringList audioLines;
    QStringList bestInf, bestUrl;
    int bestHeight = 0;
    int bestScore = std::numeric_limits<int>::min();
    const QStringList lines = QString::fromUtf8(masterBody).split(QLatin1Char('\n'));
    bool sawHeader = false;
    bool nextIsUrl = false;
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("#EXTM3U"))) {
            sawHeader = true;
            nextIsUrl = false;
        } else if (line.startsWith(QLatin1String("#EXT-X-MEDIA:"))) {
            if (line.contains(QLatin1String("TYPE=AUDIO")))
                audioLines.append(line);
            nextIsUrl = false;
        } else if (line.startsWith(QLatin1String("#EXT-X-STREAM-INF:"))) {
            int height = 0;
            const int resPos = line.indexOf(QLatin1String("RESOLUTION="));
            if (resPos >= 0) {
                const QString rest =
                    line.mid(resPos + int(QLatin1String("RESOLUTION=").size()));
                const int comma = rest.indexOf(QLatin1Char(','));
                const QString dim = comma >= 0 ? rest.left(comma) : rest;
                const int x = dim.indexOf(QLatin1Char('x'));
                if (x >= 0)
                    height = dim.mid(x + 1).toInt();
            }
            // Keep only ONE variant: the best by preferred codec and
            // height (<= maxHeight). Reduces mpv from 22 sub-playlists (~20s) to 1
            // (~2-4s) and respects the codec the GPU decodes in hardware.
            const int score = codecRankedScore(height, line);
            if (height > 0 && height <= maxHeight && score > bestScore) {
                bestInf.clear();
                bestUrl.clear();
                bestInf.append(line);
                bestScore = score;
                bestHeight = height;
                nextIsUrl = true;
            } else {
                nextIsUrl = false;
            }
        } else if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) {
            if (nextIsUrl) {
                bestUrl.append(line);
                nextIsUrl = false;
            }
        } else {
            nextIsUrl = false;
        }
    }

    if (!sawHeader || bestInf.isEmpty() || bestUrl.isEmpty())
        return QString();

    // Rebuild the output: header + audio + the chosen variant.
    QStringList out;
    out.append(QStringLiteral("#EXTM3U"));
    const QStringList selected = selectAudioTracks(audioLines, m_audioLang);
    for (const QString &line : selected)
        out.append(line);
    for (const QString &line : bestInf)
        out.append(line);
    for (const QString &line : bestUrl)
        out.append(line);

    const QString dir = HlsServer::masterDirPath();
    const QString filePath = dir + QLatin1Char('/') + videoId + QLatin1String(".m3u8");
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    QTextStream ts(&file);
    for (const QString &line : out)
        ts << line << QLatin1Char('\n');
    file.close();
    NT_DEBUG("E2E filtered master 1 variante %dp (de %dp) para %s\n",
             bestHeight, maxHeight, videoId.toUtf8().constData());
    return filePath;
}

// Synthetic AV1 HLS: YouTube does not include AV1 in its HLS master (only
// VP9/h264), and its direct adaptive URL gets throttled (~150 KB/s). Instead,
// each short Range request (~2 MB) is not throttled: one request per
// segment avoids the throttle. A fMP4 media playlist is generated with
// #EXT-X-MAP (initRange) and #EXT-X-BYTERANGE segments of ~2 MB. The URL of each
// segment inserts a /seg.mp4 suffix after /videoplayback (an extension the
// HLS demuxer accepts; YouTube ignores that suffix) and a different &nt=<n> per
// segment: without that marker ffmpeg merges the contiguous ranges of the same
// resource into a single request and the throttle returns. The audio goes
// separately (PlayerWidget::setAudioFile), as in edge mode.
// Downloads a byte range of the signed URL (blocking, with timeout). It is only
// used for the indexRange (a few KB) before building the AV1 playlist.
static QByteArray fetchRangeBlocking(const QString &url, qint64 start, qint64 end)
{
    if (url.isEmpty() || end < start)
        return QByteArray();
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("Range", QByteArrayLiteral("bytes=")
                                  + QByteArray::number(start) + '-'
                                  + QByteArray::number(end));
    req.setRawHeader("User-Agent",
                     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                     "AppleWebKit/537.36 (KHTML, like Gecko) "
                     "Chrome/120.0.0.0 Safari/537.36");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = nam.get(req);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(3000);
    loop.exec();
    QByteArray data;
    if (reply->isFinished() && reply->error() == QNetworkReply::NoError)
        data = reply->readAll();
    reply->deleteLater();
    return data;
}

// sidx box (ISO/IEC 14496-12) of the indexRange: list of subsegments with their
// exact size and duration. Needed so #EXT-X-BYTERANGE falls on the real
// fMP4 boundaries and seek works.
QVector<Yt::Av1Fragment> Yt::av1ParseSidx(const QByteArray &indexBox,
                                          qint64 *firstOffsetOut)
{
    if (firstOffsetOut)
        *firstOffsetOut = 0;
    QVector<Av1Fragment> frags;
    const int at = indexBox.indexOf("sidx");
    if (at < 4)
        return frags;
    const int boxStart = at - 4;
    auto rd32 = [&indexBox](int o) -> quint32 {
        return (quint32(quint8(indexBox[o])) << 24)
               | (quint32(quint8(indexBox[o + 1])) << 16)
               | (quint32(quint8(indexBox[o + 2])) << 8)
               | quint32(quint8(indexBox[o + 3]));
    };
    const quint32 boxSize = rd32(boxStart);
    if (boxSize < 32 || boxStart + int(boxSize) > indexBox.size())
        return frags;
    int p = boxStart + 8;
    const quint32 verFlags = rd32(p);
    p += 4; // version + flags
    p += 4; // reference_ID
    const quint32 timescale = rd32(p);
    p += 4;
    qint64 firstOffset = 0;
    if ((verFlags >> 24) == 0) {
        p += 4; // earliest_presentation_time
        firstOffset = rd32(p);
        p += 4;
    } else {
        p += 8;
        firstOffset = (qint64(rd32(p)) << 32) | rd32(p + 4);
        p += 8;
    }
    if (timescale == 0)
        return frags;
    p += 2; // reserved
    const int count = int((quint32(quint8(indexBox[p])) << 8)
                          | quint32(quint8(indexBox[p + 1])));
    p += 2;
    if (p + count * 12 > boxStart + int(boxSize))
        return frags;
    frags.reserve(count);
    for (int i = 0; i < count; ++i) {
        Av1Fragment f;
        f.size = qint64(rd32(p) & 0x7fffffffu);
        f.duration = double(rd32(p + 4)) / double(timescale);
        p += 12;
        if (f.size <= 0)
            return {};
        frags.append(f);
    }
    if (firstOffsetOut)
        *firstOffsetOut = firstOffset;
    return frags;
}

QString Yt::av1HlsPlaylist(const QJsonArray &adaptiveFormats, int maxHeight,
                           int *heightOut, const QVector<Av1Fragment> &fragments,
                           qint64 fragmentsStart)
{
    if (heightOut)
        *heightOut = 0;
    QJsonObject best;
    int bestH = 0;
    for (const QJsonValue &val : adaptiveFormats) {
        const QJsonObject f = val.toObject();
        const QString mime = f.value(QStringLiteral("mimeType")).toString();
        if (!mime.contains(QLatin1String("av01")))
            continue;
        if (f.value(QStringLiteral("url")).toString().isEmpty())
            continue;
        const int h = f.value(QStringLiteral("height")).toInt();
        if (h <= 0 || h > maxHeight)
            continue;
        if (f.value(QStringLiteral("initRange")).toObject().isEmpty())
            continue;
        if (h > bestH) {
            bestH = h;
            best = f;
        }
    }
    if (best.isEmpty())
        return QString();

    const QString url = best.value(QStringLiteral("url")).toString();
    const QJsonObject init = best.value(QStringLiteral("initRange")).toObject();
    const int initStart = init.value(QStringLiteral("start")).toString().toInt();
    const int initEnd = init.value(QStringLiteral("end")).toString().toInt();
    if (initEnd <= initStart)
        return QString();

    // Size and duration of the resource: the signed URL itself carries them (clen/dur).
    qint64 contentLength = 0;
    double durationSec = 0.0;
    const QStringList params =
        url.section(QLatin1Char('?'), 1).split(QLatin1Char('&'));
    for (const QString &kv : params) {
        if (kv.startsWith(QLatin1String("clen=")))
            contentLength = kv.mid(5).toLongLong();
        else if (kv.startsWith(QLatin1String("dur=")))
            durationSec = kv.mid(4).toDouble();
    }
    if (contentLength <= initEnd + 1)
        return QString();

    // Path suffix ignored by YouTube but with an extension the HLS demuxer
    // accepts (avoids requiring protocol_whitelist/extension_picky).
    QString segUrl = url;
    const int vp = segUrl.indexOf(QLatin1String("/videoplayback"));
    if (vp >= 0) {
        const int insertAt = vp + int(QLatin1String("/videoplayback").size());
        segUrl.insert(insertAt, QLatin1String("/seg.mp4"));
    }

    constexpr qint64 kChunk = 2 * 1024 * 1024;
    QStringList out;
    out.append(QStringLiteral("#EXTM3U"));
    out.append(QStringLiteral("#EXT-X-VERSION:7"));
    out.append(QStringLiteral("#EXT-X-PLAYLIST-TYPE:VOD"));
    out.append(QStringLiteral("#EXT-X-TARGETDURATION:120"));
    out.append(QStringLiteral("#EXT-X-MAP:URI=\"") + segUrl
               + QStringLiteral("&nt=map\",BYTERANGE=\"")
               + QString::number(initEnd - initStart + 1) + QLatin1Char('@')
               + QString::number(initStart) + QLatin1Char('"'));
    int index = 0;
    if (!fragments.isEmpty()) {
        // One HLS segment = one real fMP4 fragment: the demuxer can
        // land on any boundary (reliable seek).
        qint64 pos = fragmentsStart > 0 ? fragmentsStart : (initEnd + 1);
        for (const Av1Fragment &f : fragments) {
            if (pos >= contentLength)
                break;
            const qint64 len = qMin(f.size, contentLength - pos);
            out.append(QStringLiteral("#EXTINF:")
                       + QString::number(f.duration, 'f', 3) + QLatin1Char(','));
            out.append(QStringLiteral("#EXT-X-BYTERANGE:")
                       + QString::number(len) + QLatin1Char('@')
                       + QString::number(pos));
            out.append(segUrl + QStringLiteral("&nt=") + QString::number(index));
            pos += len;
            ++index;
        }
    } else {
        // Fallback (without sidx): fixed chunks of ~2 MB. It works for sequential
        // playback, but seek may land in the middle of a fragment and fail.
        qint64 pos = initEnd + 1;
        while (pos < contentLength) {
            const qint64 len = qMin(kChunk, contentLength - pos);
            const double segDur =
                durationSec > 0.0
                    ? durationSec * double(len) / double(contentLength)
                    : 0.0;
            out.append(QStringLiteral("#EXTINF:")
                       + QString::number(segDur, 'f', 3) + QLatin1Char(','));
            out.append(QStringLiteral("#EXT-X-BYTERANGE:")
                       + QString::number(len) + QLatin1Char('@')
                       + QString::number(pos));
            out.append(segUrl + QStringLiteral("&nt=") + QString::number(index));
            pos += len;
            ++index;
        }
    }
    out.append(QStringLiteral("#EXT-X-ENDLIST"));
    if (heightOut)
        *heightOut = bestH;
    NT_DEBUG("E2E av1-hls media %dp clen=%lld segs=%d\n", bestH,
             static_cast<long long>(contentLength), index);
    return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

// Writes av1HlsPlaylist() to disk to be served by HlsServer.
QString Yt::buildAv1Hls(const QString &videoId,
                        const QJsonArray &adaptiveFormats, int maxHeight,
                        int *heightOut)
{
    // Selects the same AV1 <= maxHeight as av1HlsPlaylist to download its
    // indexRange (sidx) and align each segment to a real fMP4 fragment
    // (without this, seek lands in the middle of a fragment and mpv does not load video).
    QJsonObject best;
    int bestH = 0;
    for (const QJsonValue &val : adaptiveFormats) {
        const QJsonObject f = val.toObject();
        const QString mime = f.value(QStringLiteral("mimeType")).toString();
        if (!mime.contains(QLatin1String("av01")))
            continue;
        if (f.value(QStringLiteral("url")).toString().isEmpty())
            continue;
        const int h = f.value(QStringLiteral("height")).toInt();
        if (h <= 0 || h > maxHeight)
            continue;
        if (f.value(QStringLiteral("initRange")).toObject().isEmpty())
            continue;
        if (h > bestH) {
            bestH = h;
            best = f;
        }
    }
    QVector<Av1Fragment> frags;
    qint64 fragsStart = -1;
    if (!best.isEmpty()) {
        const QJsonObject idx =
            best.value(QStringLiteral("indexRange")).toObject();
        const qint64 is =
            idx.value(QStringLiteral("start")).toString().toLongLong();
        const qint64 ie =
            idx.value(QStringLiteral("end")).toString().toLongLong();
        if (ie > is && ie - is < 1024 * 1024) {
            const QByteArray box = fetchRangeBlocking(
                best.value(QStringLiteral("url")).toString(), is, ie);
            if (!box.isEmpty()) {
                qint64 firstOff = 0;
                frags = av1ParseSidx(box, &firstOff);
                if (!frags.isEmpty())
                    fragsStart = ie + 1 + firstOff;
            }
        }
        NT_DEBUG("E2E av1-hls sidx frags=%d start=%lld\n", frags.size(),
                 static_cast<long long>(fragsStart));
    }

    const QString playlist =
        av1HlsPlaylist(adaptiveFormats, maxHeight, heightOut, frags, fragsStart);
    if (playlist.isEmpty())
        return QString();

    const QString dir = HlsServer::masterDirPath();
    const QString filePath =
        dir + QLatin1Char('/') + videoId + QLatin1String(".m3u8");
    // Atomic write (tmp + rename): recovery rewrites the same
    // path while mpv may be reading it; without this it would read a truncated
    // playlist.
    const QString tmpPath = filePath + QLatin1String(".tmp");
    {
        QFile file(tmpPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return QString();
        if (file.write(playlist.toUtf8()) < 0) {
            file.close();
            QFile::remove(tmpPath);
            return QString();
        }
        file.close();
    }
    if (std::rename(QFile::encodeName(tmpPath).constData(),
                    QFile::encodeName(filePath).constData()) != 0) {
        QFile::remove(tmpPath);
        return QString();
    }
    NT_DEBUG("E2E av1-hls built %dp %s\n", heightOut ? *heightOut : 0,
             filePath.toUtf8().constData());
    return filePath;
}

bool feedsChainSelfTest()
{
    std::fprintf(stderr, "FEEDS-CHAIN SELFTEST start\n");
    Yt yt;
    const QByteArray envChannel = qgetenv("NEVERTUBE_TEST_CHANNEL").trimmed();
    const QString channel = envChannel.isEmpty()
        ? QStringLiteral("UC36xmz34q02JYaZYKrMwXng")
        : QString::fromUtf8(envChannel);
    std::fprintf(stderr, "FEEDS-CHAIN canal=%s\n", channel.toUtf8().constData());

    bool channelDone = false;
    bool trendDone = false;
    bool channelOk = false;
    bool trendOk = false;
    int channelVideos = 0;
    int trendVideos = 0;

    // Global watchdog: network requests can be slow; do not hang the selftest.
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, []() {
        std::fprintf(stderr, "SELFTEST FAIL: feeds cadena (timeout global)\n");
        QCoreApplication::exit(1);
    });
    watchdog.start(90000);

    const auto maybeExit = [&]() {
        if (channelDone && trendDone)
            QCoreApplication::exit(0);
    };
    QObject::connect(&yt, &Yt::channelVideosFinished,
                     [&](const QVector<VideoInfo> &videos) {
                         if (channelDone)
                             return;
                         channelDone = true;
                         channelVideos = videos.size();
                         channelOk = !videos.isEmpty();
                         std::fprintf(stderr,
                                      "FEEDS-CHAIN channelVideos videos=%d "
                                      "(fuente=%s)\n",
                                      videos.size(),
                                      videos.isEmpty()
                                          ? "vacío"
                                          : "innertube");
                         maybeExit();
                     });
    QObject::connect(&yt, &Yt::trendingFinished,
                     [&](const QVector<VideoInfo> &videos) {
                         if (trendDone)
                             return;
                         trendDone = true;
                         trendVideos = videos.size();
                         trendOk = !videos.isEmpty();
                         std::fprintf(stderr,
                                      "FEEDS-CHAIN trending videos=%d "
                                      "(fuente=%s)\n",
                                      videos.size(),
                                      videos.isEmpty()
                                          ? "vacío"
                                          : "innertube");
                         maybeExit();
                     });

    yt.channelVideos(channel, 10);
    yt.trending(8);

    QCoreApplication::exec();
    std::fprintf(stderr,
                 "FEEDS-CHAIN RESULT canal=%s videos=%d trend=%s videos=%d\n",
                 channelOk ? "OK" : "vacío", channelVideos,
                 trendOk ? "OK" : "vacío", trendVideos);
    return channelDone && trendDone;
}

// Selftest of the SEARCH via Innertube (used by search): it launches Yt::search
// with 3 results and requires videos with a real id and no duplicates. It fails if the
// /search endpoint does not respond or returns renderers without an id.
bool searchSelfTest()
{
    std::fprintf(stderr, "SEARCH SELFTEST start\n");
    Yt yt;
    bool done = false;
    // Global watchdog: the search does real network; do not hang the selftest.
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, []() {
        std::fprintf(stderr, "SELFTEST FAIL: search (timeout global)\n");
        QCoreApplication::exit(1);
    });
    watchdog.start(90000);
    QObject::connect(&yt, &Yt::searchFinished,
                     [&](const QString &, const QVector<VideoInfo> &videos) {
                         if (done)
                             return;
                         done = true;
                         int emptyIds = 0;
                         int emptyChannels = 0;
                         for (const VideoInfo &v : videos) {
                             if (v.id.isEmpty())
                                 ++emptyIds;
                             if (v.channelId.isEmpty())
                                 ++emptyChannels;
                         }
                         // non-empty channelId: the search page builds
                         // the "Canales" section from it.
                         const bool ok = !videos.isEmpty() && emptyIds == 0
                                         && emptyChannels == 0;
                         std::fprintf(stderr,
                                      "SEARCH videos=%d idVacios=%d "
                                      "canalVacio=%d -> %s\n",
                                      videos.size(), emptyIds, emptyChannels,
                                      ok ? "OK" : "FAIL");
                         QCoreApplication::exit(ok ? 0 : 2);
                     });
    QObject::connect(&yt, &Yt::error, [&](const QString &msg) {
        if (done)
            return;
        done = true;
        std::fprintf(stderr, "SEARCH error=%s\n", msg.toUtf8().constData());
        QCoreApplication::exit(3);
    });
    yt.search(QStringLiteral("musica"), 3, 0);
    return QCoreApplication::exec() == 0;
}

bool av1HlsSelfTest()
{
    std::fprintf(stderr, "AV1-HLS SELFTEST start\n");
    bool ok = true;
    const auto check = [&ok](bool cond, const char *what) {
        std::fprintf(stderr, "AV1-HLS %s: %s\n", cond ? "OK" : "FAIL", what);
        if (!cond)
            ok = false;
    };
    const auto fmt = [](const char *mime, int height, int initStart,
                        int initEnd) {
        QJsonObject o;
        o.insert(QStringLiteral("mimeType"), QString::fromLatin1(mime));
        o.insert(QStringLiteral("height"), height);
        o.insert(QStringLiteral("url"),
                 QStringLiteral("https://rr1---sn-test.googlevideo.com/"
                                "videoplayback?expire=1&itag=399&clen=10485760"
                                "&dur=100.0&sig=x"));
        QJsonObject init;
        init.insert(QStringLiteral("start"), QString::number(initStart));
        init.insert(QStringLiteral("end"), QString::number(initEnd));
        o.insert(QStringLiteral("initRange"), init);
        return o;
    };

    QJsonArray formats;
    formats.append(fmt("video/webm; codecs=\"vp9\"", 1080, 0, 100));
    formats.append(fmt("video/mp4; codecs=\"av01.0.08M.08\"", 720, 0, 699));
    formats.append(fmt("video/mp4; codecs=\"av01.0.09M.08\"", 1080, 0, 699));

    int h = 0;
    const QString p = Yt::av1HlsPlaylist(formats, 1080, &h);
    check(!p.isEmpty(), "genera playlist con AV1");
    check(h == 1080, "elige la mayor altura AV1 <= maxHeight");
    check(p.contains(QLatin1String("#EXT-X-MAP:")), "incluye EXT-X-MAP");
    check(p.contains(QLatin1String("BYTERANGE=\"700@0\"")),
          "initRange del MAP correcto");
    check(p.contains(QLatin1String("/videoplayback/seg.mp4?")),
          "inserta el sufijo /seg.mp4");
    check(p.contains(QLatin1String("&nt=map\"")), "el MAP lleva nt=map");
    check(p.contains(QLatin1String("#EXT-X-BYTERANGE:2097152@700")),
          "primer segmento de 2 MB desde initEnd+1");
    check(p.endsWith(QLatin1String("#EXT-X-ENDLIST\n")), "cierra con ENDLIST");

    int segs = 0;
    QSet<int> nt;
    const QStringList lines =
        p.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &ln : lines) {
        if (!ln.startsWith(QLatin1String("https://")))
            continue;
        const int i = ln.indexOf(QLatin1String("&nt="));
        if (i < 0)
            continue;
        const QString v = ln.mid(i + 4);
        if (v == QLatin1String("map"))
            continue;
        nt.insert(v.toInt());
        ++segs;
    }
    check(segs == 5, "5 segmentos para 10 MB con init de 700 B");
    check(nt.size() == segs, "&nt distinto por segmento (evita fusión)");
    bool dense = true;
    for (int i = 0; i < segs; ++i)
        if (!nt.contains(i))
            dense = false;
    check(dense, "&nt consecutivo 0..n-1");

    QJsonArray onlyVp9;
    onlyVp9.append(fmt("video/webm; codecs=\"vp9\"", 1080, 0, 100));
    check(Yt::av1HlsPlaylist(onlyVp9, 1080, &h).isEmpty(),
          "sin AV1 devuelve vacío (fallback al HLS de YouTube)");
    check(Yt::av1HlsPlaylist(formats, 480, &h).isEmpty(),
          "maxHeight sin AV1 disponible devuelve vacío");

    // --- sidx: one HLS segment per real fragment (reliable seek) ---
    const auto be32 = [](QByteArray &b, quint32 v) {
        b.append(char((v >> 24) & 0xff));
        b.append(char((v >> 16) & 0xff));
        b.append(char((v >> 8) & 0xff));
        b.append(char(v & 0xff));
    };
    const auto be16 = [](QByteArray &b, quint16 v) {
        b.append(char((v >> 8) & 0xff));
        b.append(char(v & 0xff));
    };
    const QVector<quint32> fsz{1000, 2500, 1750};
    const QVector<double> fsec{2.5, 2.5, 1.0};
    QByteArray box;
    be32(box, 0);                                   // box size (below)
    box.append("sidx");
    be32(box, 0);                                   // version 0 + flags
    be32(box, 1);                                   // reference_ID
    be32(box, 30000);                               // timescale
    be32(box, 0);                                   // earliest_presentation
    be32(box, 1234);                                // first_offset
    be16(box, 0);                                   // reserved
    be16(box, quint16(fsz.size()));                 // reference_count
    for (int i = 0; i < fsz.size(); ++i) {
        be32(box, fsz[i]);
        be32(box, quint32(fsec[i] * 30000.0));
        be32(box, 0x90000000);                      // SAP
    }
    const quint32 boxSize = quint32(box.size());
    box[0] = char((boxSize >> 24) & 0xff);
    box[1] = char((boxSize >> 16) & 0xff);
    box[2] = char((boxSize >> 8) & 0xff);
    box[3] = char(boxSize & 0xff);

    qint64 firstOff = -1;
    const QVector<Yt::Av1Fragment> parsed = Yt::av1ParseSidx(box, &firstOff);
    check(parsed.size() == 3, "sidx: parsea los 3 fragmentos");
    check(firstOff == 1234, "sidx: first_offset");
    check(parsed.size() == 3 && parsed[0].size == 1000
              && qAbs(parsed[1].duration - 2.5) < 1e-6,
          "sidx: tamaño y duración por fragmento");

    QJsonArray av1only;
    av1only.append(fmt("video/mp4; codecs=\"av01.0.08M.08\"", 720, 0, 699));
    QVector<Yt::Av1Fragment> frags{{1000, 2.5}, {2500, 2.5}, {1750, 1.0}};
    const QString pf = Yt::av1HlsPlaylist(av1only, 720, &h, frags, 5000);
    check(pf.contains(QLatin1String("#EXT-X-BYTERANGE:1000@5000")),
          "sidx: primer segmento en la frontera real");
    check(pf.contains(QLatin1String("#EXT-X-BYTERANGE:2500@6000")),
          "sidx: segundo segmento contiguo");
    check(pf.contains(QLatin1String("#EXT-X-BYTERANGE:1750@8500")),
          "sidx: tercer segmento contiguo");
    check(pf.contains(QLatin1String("#EXTINF:2.500,")),
          "sidx: EXTINF real del fragmento");

    std::fprintf(stderr, "AV1-HLS SELFTEST %s\n", ok ? "OK" : "FAIL");
    return ok;
}