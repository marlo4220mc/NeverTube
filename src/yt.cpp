#include "yt.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRandomGenerator>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <limits>
#include <memory>

#include "debug.h"
#include "db/repositories.h"

namespace {

const QString kNoWarnings = QStringLiteral("--no-warnings");
// Idioma de los metadatos (títulos): sin esto YouTube auto-traduce títulos a
// inglés según el contexto, aunque el vídeo sea de un creador hispanohablante.
const QString kYtLang = QStringLiteral("--extractor-args");
const QString kYtLangArg = QStringLiteral("youtube:lang=es");

// Centinelas para m_winningClient (índices de cliente Innertube son >= 0).
constexpr int kResolveYtDlp = -1;      // yt-dlp fue quien resolvió
constexpr int kResolveUnknown = -2;    // aún sin ganador registrado

int boundedSafe(int value)
{
    return value < 0 ? 0 : value;
}

// Devuelve el valor del atributo EXT-X-MEDIA dado (p. ej. LANGUAGE, NAME).
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
                    if (lang == prefLang
                        || lang.startsWith(prefLang + QLatin1Char('-')))
                        preflight = t;
                }
            }
        }
        if (!prefLang.isEmpty() && !preflight.isEmpty()) {
            // Preferencia encontrada: ese doblaje primero (mpv lo elige con
            // alang). El resto de doblajes del grupo se descartan para no
            // inflar el master (ralentiza el arranque del vídeo).
            out.append(preflight);
        }
        if (!original.isEmpty() && !out.contains(original)) {
            out.append(original);
        } else if (original.isEmpty() && !tracks.isEmpty()) {
            // Sin pista original reconocible (vídeos sin `acont=original`):
            // conservar la primera del grupo para no dejar el vídeo sin audio.
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

    // /next: reenvío directo del Innertube al resto de la app.
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
            [this](const QString &channelId, const QVector<VideoInfo> &videos) {
                // Innertube primero; si no dio vídeos (fallo/500/vacío) cae al
                // fallback yt-dlp. Si ese también falla, se emite vacío y la
                // UI oculta la sección.
                if (!videos.isEmpty()) {
                    emit channelVideosFinished(videos);
                    return;
                }
                channelVideosYtDlp(channelId);
            });
    connect(&m_innertube, &Innertube::trendingReady, this,
            [this](const QVector<VideoInfo> &videos) {
                if (!videos.isEmpty()) {
                    emit trendingFinished(videos);
                    return;
                }
                trendingYtDlp();
            });
}

void Yt::setVisitorStorage(SettingsRepo *settings)
{
    m_visitorStore = settings;
    if (!settings)
        return;
    // Reutilizar un visitorData persistido no caducado: evita el GET
    // sw.js_data de arranque y una RTT antes del POST /player.
    const QString saved =
        settings->getString(QStringLiteral("visitorData"));
    const qlonglong savedExp =
        settings->getInt(QStringLiteral("visitorDataExpiresAt"), 0);
    if (!saved.isEmpty())
        m_innertube.seedVisitorData(saved, savedExp);
    // y guardar los nuevos que lleguen en ejecución.
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
    // Propagar el idioma al Innertube para que /next y /player entreguen
    // títulos y descripciones traducidos (hl). original mantiene "es" por
    // defecto (evita auto-traducción automática al inglés).
    const QString hl = m_audioLang.isEmpty() ? QStringLiteral("es")
                                             : m_audioLang;
    m_innertube.setHl(hl);
}

QString Yt::audioLanguage() const
{
    return m_audioLang;
}

void Yt::search(const QString &query, int limit, int offset)
{
    QStringList args = {
        kNoWarnings,
        kYtLang,
        kYtLangArg,
        QStringLiteral("--flat-playlist"),
        QStringLiteral("--ignore-errors"),
        QStringLiteral("--dump-single-json"),
        QStringLiteral("ytsearch%1:%2").arg(offset + limit).arg(query),
    };
    if (offset > 0)
        args << QStringLiteral("--playlist-start") << QString::number(offset + 1);
    execJson(args, [this, query](const QJsonObject &root) {
        emit searchFinished(query, parseVideos(root));
    });
}

void Yt::trending(int limit)
{
    Q_UNUSED(limit);
    m_innertube.fetchTrending();
}

void Yt::channelVideos(const QString &channelId, int limit)
{
    Q_UNUSED(limit);
    m_innertube.fetchChannelVideos(channelId);
}

// Fallback degradado del feed de canales cuando Innertube no da vídeos.
void Yt::channelVideosYtDlp(const QString &channelId)
{
    NT_DEBUG("E2E channelVideos fallback yt-dlp id=%s\n",
             channelId.toUtf8().constData());
    const QString uuId = QStringLiteral("UU%1").arg(channelId.mid(2));
    const QStringList args = {
        kNoWarnings,
        kYtLang,
        kYtLangArg,
        QStringLiteral("--flat-playlist"),
        QStringLiteral("--ignore-errors"),
        QStringLiteral("--playlist-end"),
        QStringLiteral("10"),
        QStringLiteral("--dump-single-json"),
        QStringLiteral("https://www.youtube.com/playlist?list=%1").arg(uuId),
    };
    execJson(args, [this](const QJsonObject &root) {
        QVector<VideoInfo> videos = parseVideos(root);
        std::sort(videos.begin(), videos.end(), [](const VideoInfo &a,
                                                   const VideoInfo &b) {
            return a.publishedAt > b.publishedAt;
        });
        emit channelVideosFinished(videos);
    });
}

// Fallback degradado de tendencias cuando Innertube no da vídeos.
void Yt::trendingYtDlp()
{
    NT_DEBUG("E2E trending fallback yt-dlp\n");
    const QStringList args = {
        kNoWarnings,
        kYtLang,
        kYtLangArg,
        QStringLiteral("--flat-playlist"),
        QStringLiteral("--ignore-errors"),
        QStringLiteral("-I"),
        QStringLiteral("1:8"),
        QStringLiteral("--dump-single-json"),
        QStringLiteral("https://www.youtube.com/feed/trending"),
    };
    execJson(args, [this](const QJsonObject &root) {
        emit trendingFinished(parseVideos(root));
    });
}

void Yt::channelTabVideos(const QString &channelId, const QString &tab, int limit,
                          int offset)
{
    const QString path = tab == QLatin1String("shorts") ? QStringLiteral("/shorts")
                       : tab == QLatin1String("live") ? QStringLiteral("/streams")
                       : QStringLiteral("/videos");
    QStringList args = {
        kNoWarnings,
        kYtLang,
        kYtLangArg,
        QStringLiteral("--flat-playlist"),
        QStringLiteral("--ignore-errors"),
        QStringLiteral("--playlist-end"),
        QString::number(offset + limit),
        QStringLiteral("--dump-single-json"),
        QStringLiteral("https://www.youtube.com/channel/%1%2").arg(channelId, path),
    };
    if (offset > 0)
        args << QStringLiteral("--playlist-start") << QString::number(offset + 1);
    execJson(args, [this, tab, channelId](const QJsonObject &root) {
        QVector<VideoInfo> videos = parseVideos(root);
        NT_DEBUG("E2E channelTabVideos id=%s tab=%s count=%d\n",
                 channelId.toUtf8().constData(),
                 tab.toUtf8().constData(), (int)videos.size());
        emit channelTabVideosFinished(channelId, tab, videos);
    });
}

void Yt::channelInfo(const QString &channelId)
{
    const QStringList args = {
        kNoWarnings,
        QStringLiteral("--extractor-args"),
        QStringLiteral("youtube:player_client=web_embedded,lang=es"),
        QStringLiteral("--skip-download"),
        QStringLiteral("--dump-single-json"),
        QStringLiteral("https://www.youtube.com/channel/%1/about").arg(channelId),
    };
    execJson(args, [this, channelId](const QJsonObject &root) {
        const ChannelInfo info = channelFromJson(channelId, root);
        NT_DEBUG("E2E channelInfo id=%s name=%s subs=%d\n",
                 channelId.toUtf8().constData(),
                 info.name.toUtf8().constData(), info.subscriberCount);
        emit channelInfoReady(info);
    });
}

void Yt::getDetail(const QString &videoId)
{
    // web_embedded evita el bloqueo anti-bot del cliente por defecto y
    // sigue devolviendo título/descripción en el idioma de lang (es).
    // Algunos vídeos se reportan "Video unavailable" con ese cliente aunque
    // sí son reproducibles (por Innertube); cascada de clientes para obtener
    // los metadatos igualmente.
    getDetailTry(videoId, 0);
}

void Yt::getDetailTry(const QString &videoId, int clientIndex)
{
    static const QStringList clients = {
        QStringLiteral("web_embedded"),
        QStringLiteral("tv"),
        QStringLiteral("tv_embedded"),
        QStringLiteral("mweb"),
        QStringLiteral("android"),
    };
    if (clientIndex >= clients.size()) {
        emit detailFailed(videoId);
        return;
    }
    NT_DEBUG("E2E getDetail id=%s client=%s\n",
             videoId.toUtf8().constData(),
             clients[clientIndex].toUtf8().constData());
    const QStringList args = {
        kNoWarnings,
        QStringLiteral("--extractor-args"),
        QStringLiteral("youtube:player_client=%1,lang=es")
            .arg(clients[clientIndex]),
        QStringLiteral("--skip-download"),
        QStringLiteral("--dump-single-json"),
        QStringLiteral("https://www.youtube.com/watch?v=%1").arg(videoId),
    };
    execJson(args, [this, videoId, clientIndex](const QJsonObject &j) {
        const VideoInfo video = videoFromJson(j);
        if (video.id.isEmpty()) {
            // Fracaso de este cliente (JSON vacío o "Video unavailable");
            // probar el siguiente antes de rendirse.
            getDetailTry(videoId, clientIndex + 1);
            return;
        }
        QVector<FormatInfo> formats;
        const QJsonArray raw = j.value(QStringLiteral("formats")).toArray();
        for (const QJsonValue &val : raw) {
            const QJsonObject f = val.toObject();
            const int height = f.value(QStringLiteral("height")).toInt(0);
            if (height <= 0)
                continue;
            const QString vcodec = f.value(QStringLiteral("vcodec")).toString();
            const QString acodec = f.value(QStringLiteral("acodec")).toString();
            const bool hasVideo = vcodec.isEmpty() ? f.value(QStringLiteral("has_video")).toBool(true)
                                                   : vcodec != QLatin1String("none");
            const bool hasAudio = acodec.isEmpty() ? f.value(QStringLiteral("has_audio")).toBool(true)
                                                   : acodec != QLatin1String("none");
            if (!hasVideo || !hasAudio)
                continue;
            if (f.value(QStringLiteral("protocol")).toString() == QLatin1String("mhtml"))
                continue;
            FormatInfo info;
            info.formatId = f.value(QStringLiteral("format_id")).toString();
            if (info.formatId.startsWith(QLatin1String("hls"))
                || info.formatId.startsWith(QLatin1String("sb")))
                continue;
            info.height = height;
            const QString note = f.value(QStringLiteral("format_note")).toString();
            info.label = note.isEmpty() ? QStringLiteral("%1p").arg(height) : note;
            info.ext = f.value(QStringLiteral("ext")).toString();
            if (info.ext.isEmpty())
                info.ext = QStringLiteral("mp4");
            formats.append(info);
        }
        std::sort(formats.begin(), formats.end(),
                  [](const FormatInfo &a, const FormatInfo &b) {
                      return a.height < b.height;
                  });
        const qint64 likes = j.value(QStringLiteral("like_count")).toVariant().toLongLong();
        const QString description = j.value(QStringLiteral("description")).toString();
        emit detailReady(video, description, likes, formats);
    });
}

void Yt::playlistDetail(const QString &playlistId, int limit)
{
    const QStringList args = {
        kNoWarnings,
        kYtLang,
        kYtLangArg,
        QStringLiteral("--flat-playlist"),
        QStringLiteral("--ignore-errors"),
        QStringLiteral("-I"),
        QStringLiteral("1:%1").arg(limit),
        QStringLiteral("--dump-single-json"),
        QStringLiteral("https://www.youtube.com/playlist?list=%1").arg(playlistId),
    };
    execJson(args, [this](const QJsonObject &root) {
        PlaylistInfo info;
        QVector<VideoInfo> videos;
        parsePlaylist(root, &info, &videos);
        emit playlistDetailReady(info, videos);
    });
}

void Yt::resolveStream(const QString &videoId, int maxHeight)
{
    const int winning = m_winningClient.value(videoId, kResolveUnknown);
    NT_DEBUG("E2E resolveStream id=%s maxH=%d winner=%d\n",
             videoId.toUtf8().constData(), maxHeight, winning);
    if (winning == kResolveYtDlp)
        resolveWithYtDlp(videoId, maxHeight, /*fallbackToInnertube*/ true);
    else
        resolveViaInnertube(videoId, maxHeight, winning);
}

void Yt::resolveViaInnertube(const QString &videoId, int maxHeight,
                             int preferredClient)
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
                qWarning("yt: innertube falló para %s (%s); fallback yt-dlp",
                         vid.toUtf8().constData(), reason.toUtf8().constData());
                resolveWithYtDlp(videoId, maxHeight);
            });

    connect(innTube, &Innertube::hlsReady, guard,
            [this, guard, guardFail, innTube, videoId, maxHeight, gen](
                const QString &vid, const QString &hlsUrl,
                const QVector<int> &heights) {
                if (vid != videoId || gen != m_resolveGen)
                    return;
                if (heights.isEmpty()) {
                    // Sin tabla de alturas: entregar el master sin filtrar
                    // (comportamiento previo).
                    guard->deleteLater();
                    m_winningClient[videoId] = innTube->lastWinClient();
                    StreamInfo s;
                    s.url = hlsUrl;
                    s.label = qualityName(maxHeight) + QLatin1Char('p');
                    s.height = maxHeight;
                    s.multipleUrls = false;
                    emit streamReady(s);
                    return;
                }
                const int wanted = closestHeight(maxHeight, heights);
                // A partir de aquí el fallo lo gestiona filterGuard; el
                // guard inicial queda liberado para evitar doble fallback.
                disconnect(guardFail);
                guard->deleteLater();
                // Un segundo guard para la fase de filtrado del master.
                auto *filterGuard = new QObject(this);
                QMetaObject::Connection masterConn;
                masterConn = connect(
                    innTube, &Innertube::masterReady, filterGuard,
                    [this, filterGuard, innTube, videoId, wanted, gen](
                        const QString &fid, const QByteArray &masterBody) {
                        if (fid != videoId || gen != m_resolveGen)
                            return;
                        const QString abs = buildFilteredMaster(
                            videoId, masterBody, wanted);
                        if (abs.isEmpty()) {
                            // Master no filtrable: fallback yt-dlp.
                            filterGuard->deleteLater();
                            qWarning("yt: master sin variantes filtrables para %s; "
                                     "fallback yt-dlp",
                                     videoId.toUtf8().constData());
                            resolveWithYtDlp(videoId, wanted);
                            return;
                        }
                        const QString origin = m_hls.start();
                        StreamInfo s;
                        s.url = origin + QLatin1Char('/') + videoId
                                + QLatin1String(".m3u8");
                        s.label = QString::number(wanted) + QLatin1Char('p');
                        s.height = wanted;
                        s.multipleUrls = false;
                        m_winningClient[videoId] = innTube->lastWinClient();
                        NT_DEBUG("E2E hls filtrado a %dp url=%s\n", wanted,
                                 s.url.toUtf8().constData());
                        filterGuard->deleteLater();
                        emit streamReady(s);
                    });
                // Fracaso en la fase de master: fallback yt-dlp.
                QMetaObject::Connection masterFail;
                masterFail = connect(
                    innTube, &Innertube::failed, filterGuard,
                    [this, filterGuard, masterConn, videoId, maxHeight, gen](
                        const QString &fid, const QString &reason) {
                        if (fid != videoId || gen != m_resolveGen)
                            return;
                        disconnect(masterConn);
                        filterGuard->deleteLater();
                        qWarning("yt: innertube falló al filtrar master (%s); "
                                 "fallback yt-dlp",
                                 reason.toUtf8().constData());
                        resolveWithYtDlp(videoId, maxHeight);
                    });
                Q_UNUSED(masterFail)
                innTube->fetchMaster(vid, hlsUrl);
            });

    m_innertube.fetchHls(videoId, preferredClient);
}

void Yt::resolveWithYtDlp(const QString &videoId, int maxHeight,
                           bool fallbackToInnertube)
{
    // Helper interno: si hay fallback habilitado, al fallar reintentar
    // Innertube con la cascada completa (solo la primera vez).
    const auto onFail =
            [this, videoId, maxHeight, fallbackToInnertube](const QString &msg) {
                if (fallbackToInnertube) {
                    qWarning("yt: yt-dlp preferido falló; reiniciando cascada "
                             "innertube para %s",
                             videoId.toUtf8().constData());
                    resolveViaInnertube(videoId, maxHeight, -1);
                    return;
                }
                emit resolveError(msg);
            };

    getUrls(videoId, formatSelector(maxHeight),
            [this, videoId, maxHeight, onFail](const QStringList &combinedUrls) {
                if (combinedUrls.size() == 1) {
                    m_winningClient[videoId] = kResolveYtDlp;
                    StreamInfo s;
                    s.url = combinedUrls.first();
                    s.label = qualityName(maxHeight) + QLatin1Char('p');
                    s.height = maxHeight;
                    s.multipleUrls = false;
                    emit streamReady(s);
                    return;
                }

                const QString videoSel =
                    QStringLiteral("bestvideo[height<=%1][ext=mp4][vcodec^=avc1]"
                                   "[protocol=m3u8_native]/bestvideo[height<=%1]"
                                   "[protocol=m3u8_native]/bestvideo[height<=%1]")
                        .arg(maxHeight);
                getUrls(videoId, videoSel,
                        [this, videoId, maxHeight, onFail](
                            const QStringList &videoUrls) {
                            if (videoUrls.isEmpty()) {
                                onFail(QStringLiteral(
                                    "yt-dlp no devolvió una URL reproducible; "
                                    "el video puede requerir iniciar sesión."));
                                return;
                            }
                            const QString videoUrl = videoUrls.first();
                            if (!videoUrl.contains(QLatin1String(".m3u8"))) {
                                onFail(QStringLiteral(
                                    "yt-dlp solo devolvió video sin audio."));
                                return;
                            }
                            getUrls(videoId,
                                    QStringLiteral("bestaudio[protocol=m3u8_native]/bestaudio"),
                                    [this, videoId, maxHeight, videoUrl, onFail](
                                        const QStringList &audioUrls) {
                                        if (audioUrls.isEmpty()) {
                                            onFail(QStringLiteral(
                                                "yt-dlp no devolvió una pista de audio; "
                                                "el video puede requerir iniciar sesión."));
                                            return;
                                        }
                                        const QString master = buildHlsMaster(
                                            videoId, videoUrl, audioUrls.first());
                                        const QString origin = m_hls.start();
                                        if (master.isEmpty() || origin.isEmpty()) {
                                            onFail(QStringLiteral(
                                                "No se pudo preparar el stream de respaldo."));
                                            return;
                                        }
                                        m_winningClient[videoId] = kResolveYtDlp;
                                        StreamInfo s;
                                        s.url = origin + QLatin1Char('/')
                                                + videoId + QLatin1String(".m3u8");
                                        s.label = qualityName(maxHeight) + QLatin1Char('p');
                                        s.height = maxHeight;
                                        s.multipleUrls = false;
                                        emit streamReady(s);
                                    });
                        });
            });
}

QString Yt::downloadVideoSelector(int maxHeight) const
{
    return QStringLiteral("best[height<=%1][ext=mp4][vcodec!*=vp9]/"
                          "best[height<=%1]/best")
        .arg(maxHeight);
}

QString Yt::qualityName(int height)
{
    if (height <= 360)
        return QStringLiteral("360");
    if (height <= 480)
        return QStringLiteral("480");
    if (height <= 720)
        return QStringLiteral("720");
    return QStringLiteral("1080");
}

// Elige la altura a pedir: exactamente la solicitada si existe; si no, la
// inmediatamente inferior disponible; nunca sube de forma automática salvo
// que no exista ninguna inferior (último recurso).
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

// Filtra el master HLS de YouTube dejando únicamente las variantes con
// RESOLUTION= altura <= maxHeight. El abreviado se sirve desde el HlsServer
// local para que el reproductor solo vea las calidades permitidas.
// Las pistas de audio se eligen con selectAudioTracks: con m_audioLang vacío
// (por defecto) solo la original; el orden de publicación define cuál elige
// mpv (elige la primera de cada grupo).
QString Yt::buildFilteredMaster(const QString &videoId,
                                const QByteArray &masterBody, int maxHeight)
{
    if (masterBody.isEmpty())
        return QString();

    QStringList out;
    QStringList audioLines;
    const QStringList lines = QString::fromUtf8(masterBody).split(QLatin1Char('\n'));
    bool sawHeader = false;
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("#EXTM3U"))) {
            out.append(line);
            sawHeader = true;
        } else if (line.startsWith(QLatin1String("#EXT-X-MEDIA:"))) {
            if (line.contains(QLatin1String("TYPE=AUDIO")))
                audioLines.append(line);
            else
                out.append(line);
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
            if (height > 0 && height <= maxHeight)
                out.append(line);
            // La URL de la variante va en la línea siguiente; se conserva
            // solo si su #EXT-X-STREAM-INF fue conservado.
        } else if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) {
            // Línea de URL de variante: solo se mantiene si la línea
            // previa es un #EXT-X-STREAM-INF filtrado.
            if (out.size() > 0
                && out.last().startsWith(QLatin1String("#EXT-X-STREAM-INF:")))
                out.append(line);
        }
    }

    // Insertar las pistas de audio seleccionadas justo tras la cabecera.
    if (sawHeader) {
        const QStringList selected = selectAudioTracks(audioLines, m_audioLang);
        int ins = 1;
        for (const QString &line : selected) {
            if (ins <= out.size())
                out.insert(ins++, line);
            else
                out.append(line);
        }
    }

    if (!sawHeader || out.size() < 2)
        return QString();

    const QString dir = HlsServer::masterDirPath();
    const QString filePath = dir + QLatin1Char('/') + videoId + QLatin1String(".m3u8");
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    QTextStream ts(&file);
    for (const QString &line : out)
        ts << line << QLatin1Char('\n');
    file.close();
    return filePath;
}

QString Yt::formatSelector(int maxHeight)
{
    return QStringLiteral("best[height<=%1][ext=mp4][protocol=https][acodec^=mp4a]/"
                          "best[height<=%1][ext=mp4][protocol^=http][acodec^=mp4a]")
        .arg(maxHeight);
}

QString Yt::buildHlsMaster(const QString &videoId, const QString &videoUrl,
                           const QString &audioUrl)
{
    const QString dir = HlsServer::masterDirPath();
    const QString filePath = dir + QLatin1Char('/') + videoId + QLatin1String(".m3u8");
    const QString bw = QString::number(2500000);
    const QString resolution = QStringLiteral("1280x720");
    const QString content =
        QStringLiteral("#EXTM3U\n")
        + QStringLiteral("#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"Audio\","
                         "DEFAULT=YES,AUTOSELECT=YES,URI=\"%1\"\n")
              .arg(audioUrl)
        + QStringLiteral("#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=%1,"
                         "RESOLUTION=%2,AUDIO=\"aud\"\n")
              .arg(bw, resolution)
        + videoUrl + QStringLiteral("\n\n");
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    file.write(content.toUtf8());
    file.close();
    return filePath;
}

QVector<VideoInfo> Yt::parseVideos(const QJsonObject &root)
{
    QVector<VideoInfo> list;
    const QJsonArray entries = root.value(QStringLiteral("entries")).toArray();
    if (entries.isEmpty()) {
        if (root.value(QStringLiteral("_type")).toString() == QLatin1String("playlist"))
            return list;
        list.append(videoFromJson(root));
        return list;
    }
    for (const QJsonValue &entry : entries)
        list.append(videoFromJson(entry.toObject()));
    return list;
}

VideoInfo Yt::videoFromJson(const QJsonObject &j)
{
    VideoInfo v;
    v.id = j.value(QStringLiteral("id")).toString();
    if (v.id.startsWith(QLatin1String("ytsearch")))
        return v;
    v.title = j.value(QStringLiteral("title")).toString();
    if (v.title.isEmpty())
        v.title = QStringLiteral("Sin título");
    const QString channel = j.value(QStringLiteral("uploader")).toString();
    v.channel = channel.isEmpty() ? j.value(QStringLiteral("channel")).toString()
                                  : channel;
    const QString channelId = j.value(QStringLiteral("channel_id")).toString();
    v.channelId = channelId.isEmpty() ? j.value(QStringLiteral("uploader_id")).toString()
                                      : channelId;
    v.durationSeconds = boundedSafe(j.value(QStringLiteral("duration")).toInt());
    v.viewCount = boundedSafe(j.value(QStringLiteral("view_count")).toInt());
    v.publishedAt = publishedFrom(j);
    v.thumbnailUrl = pickThumbnail(j.value(QStringLiteral("thumbnails")).toArray());
    if (v.thumbnailUrl.isEmpty())
        v.thumbnailUrl = fallbackThumb(v.id);
    const QJsonValue desc = j.value(QStringLiteral("description"));
    if (desc.isString())
        v.description = desc.toString();
    return v;
}

int boundedSafe(int value) { return value < 0 ? 0 : value; }

ChannelInfo Yt::channelFromJson(const QString &id, const QJsonObject &j)
{
    ChannelInfo c;
    c.id = id;
    const QString name = j.value(QStringLiteral("channel")).toString();
    c.name = name.isEmpty() ? j.value(QStringLiteral("uploader")).toString() : name;
    if (c.name.isEmpty())
        c.name = id;
    const QJsonValue subs = j.value(QStringLiteral("channel_follower_count"));
    if (subs.isDouble())
        c.subscriberCount = subs.toVariant().toLongLong();
    else
        c.subscriberCount = j.value(QStringLiteral("subscriber_count")).toVariant().toLongLong();
    c.avatarUrl = pickAvatar(j.value(QStringLiteral("thumbnails")).toArray());
    c.description = j.value(QStringLiteral("description")).toString();
    return c;
}

void Yt::parsePlaylist(const QJsonObject &root, PlaylistInfo *info,
                       QVector<VideoInfo> *videos)
{
    info->title = root.value(QStringLiteral("title")).toString();
    if (info->title.isEmpty())
        info->title = QStringLiteral("Playlist");
    info->channel = root.value(QStringLiteral("channel")).toString();
    info->videoCount = root.value(QStringLiteral("playlist_count")).toInt();
    const QString thumb = root.value(QStringLiteral("thumbnail")).toString();
    if (!thumb.isEmpty())
        info->thumbnailUrl = thumb;
    const QJsonArray entries = root.value(QStringLiteral("entries")).toArray();
    for (const QJsonValue &entry : entries)
        videos->append(videoFromJson(entry.toObject()));
}

QString Yt::pickThumbnail(const QJsonArray &thumbs)
{
    QString best;
    int bestScore = -1;
    for (const QJsonValue &val : thumbs) {
        if (!val.isObject())
            continue;
        const QJsonObject t = val.toObject();
        const QString url = t.value(QStringLiteral("url")).toString();
        if (url.isEmpty())
            continue;
        const int w = t.value(QStringLiteral("width")).toInt(0);
        const int h = t.value(QStringLiteral("height")).toInt(0);
        const int pref = t.value(QStringLiteral("preference")).toInt(0);
        const int score = qMax(w * h, 1) + pref * 1000;
        if (score > bestScore) {
            bestScore = score;
            best = url;
        }
    }
    return best;
}

QString Yt::pickAvatar(const QJsonArray &thumbs)
{
    QString best;
    int bestScore = -1;
    for (const QJsonValue &val : thumbs) {
        if (!val.isObject())
            continue;
        const QJsonObject t = val.toObject();
        const QString url = t.value(QStringLiteral("url")).toString();
        if (url.isEmpty())
            continue;
        const int w = t.value(QStringLiteral("width")).toInt(0);
        const int h = t.value(QStringLiteral("height")).toInt(0);
        if (w == 0 || h == 0)
            continue;
        const int ratio = qMax(w, h) * 100 / qMin(w, h);
        const int sqPenalty = qAbs(ratio - 100);
        const int score = w * h - sqPenalty * w * h / 4;
        if (score > bestScore) {
            bestScore = score;
            best = url;
        }
    }
    return best;
}

QString Yt::fallbackThumb(const QString &videoId)
{
    if (videoId.isEmpty())
        return QString();
    const QStringList keys{
        QStringLiteral("mqdefault"),
        QStringLiteral("hqdefault"),
        QStringLiteral("maxresdefault"),
    };
    const int idx = QRandomGenerator::global()->bounded(keys.size());
    return QStringLiteral("https://i.ytimg.com/vi/%1/%2.jpg")
        .arg(videoId, keys[idx]);
}

QString Yt::publishedFrom(const QJsonObject &j)
{
    const double ts = j.value(QStringLiteral("timestamp")).toDouble(0.0);
    if (ts > 0.0)
        return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ts * 1000.0))
            .toUTC()
            .toString(Qt::ISODate);
    const QString d = j.value(QStringLiteral("upload_date")).toString();
    if (d.size() == 8) {
        const QDate date = QDate::fromString(d, QStringLiteral("yyyyMMdd"));
        if (date.isValid())
            return date.toString(Qt::ISODate);
    }
    return QString();
}

void Yt::execJson(QStringList args, std::function<void(const QJsonObject &)> done)
{
    auto *process = new QProcess(this);
    auto fired = std::make_shared<bool>(false);
    QObject::connect(process,
                     static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
                         &QProcess::finished),
                     process, [this, process, done, fired]() {
                         if (*fired)
                             return;
                         *fired = true;
                         const int code = process->exitCode();
                         if (code != 0) {
                             const QString err =
                                 QString::fromUtf8(process->readAllStandardError()).trimmed();
                             Q_EMIT error(err.isEmpty()
                                               ? QStringLiteral("yt-dlp falló (código %1).")
                                                     .arg(code)
                                               : err);
                             if (done)
                                 done(QJsonObject());
                             process->deleteLater();
                             return;
                         }
                         const QByteArray out = process->readAllStandardOutput();
                         QJsonParseError parseErr;
                         const QJsonDocument doc = QJsonDocument::fromJson(out, &parseErr);
                         if (parseErr.error != QJsonParseError::NoError) {
                             Q_EMIT error(QStringLiteral("Respuesta JSON inválida de yt-dlp."));
                             if (done)
                                 done(QJsonObject());
                             process->deleteLater();
                             return;
                         }
                         if (done)
                             done(doc.object());
                         process->deleteLater();
                     });
    QTimer::singleShot(120000, process, [process, fired]() {
        if (!*fired) {
            *fired = true;
            process->kill();
        }
    });
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->start(QStringLiteral("yt-dlp"), args);
    if (!process->waitForStarted(3000)) {
        if (!*fired) {
            *fired = true;
            Q_EMIT error(QStringLiteral("No se pudo iniciar yt-dlp."));
            if (done)
                done(QJsonObject());
            process->deleteLater();
        }
    }
}

void Yt::execLines(QStringList args, std::function<void(const QStringList &)> done)
{
    auto *process = new QProcess(this);
    auto fired = std::make_shared<bool>(false);
    QObject::connect(process,
                     static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
                         &QProcess::finished),
                     process, [process, done, fired]() {
                         if (*fired)
                             return;
                         *fired = true;
                         QStringList lines;
                         if (process->exitCode() == 0) {
                             const QByteArray out = process->readAllStandardOutput();
                             const auto split = QString::fromUtf8(out).split(QLatin1Char('\n'));
                             for (const QString &raw : split) {
                                 const QString line = raw.trimmed();
                                 if (!line.isEmpty() && line.startsWith(QLatin1String("http")))
                                     lines.append(line);
                             }
                         }
                         if (done)
                             done(lines);
                         process->deleteLater();
                     });
    QTimer::singleShot(120000, process, [process, fired]() {
        if (!*fired) {
            *fired = true;
            process->kill();
        }
    });
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->start(QStringLiteral("yt-dlp"), args);
    if (!process->waitForStarted(3000)) {
        if (!*fired) {
            *fired = true;
            if (done)
                done(QStringList());
            process->deleteLater();
        }
    }
}

void Yt::getUrls(const QString &videoId, const QString &selector,
                 std::function<void(const QStringList &)> done)
{
    const QStringList args = {
        kNoWarnings,
        QStringLiteral("--no-playlist"),
        QStringLiteral("--extractor-args"),
        QStringLiteral("youtube:player_client=visionos"),
        QStringLiteral("--get-url"),
        QStringLiteral("-f"),
        selector,
        QStringLiteral("https://www.youtube.com/watch?v=%1").arg(videoId),
    };
    execLines(args, done);
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

    // Watchdog global: los fallbacks yt-dlp pueden tardar; no colgar el selftest.
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
                                          ? "yt-dlp vacío/degradado"
                                          : "innertube (o fallback con datos)");
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
                                          ? "yt-dlp vacío/degradado"
                                          : "innertube (o fallback con datos)");
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