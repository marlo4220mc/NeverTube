#include "yt.h"

#include <QDateTime>
#include <QtMath>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QRandomGenerator>
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

namespace {

const QString kNoWarnings = QStringLiteral("--no-warnings");
// Idioma de los metadatos (títulos): sin esto YouTube auto-traduce títulos a
// inglés según el contexto, aunque el vídeo sea de un creador hispanohablante.
const QString kYtLang = QStringLiteral("--extractor-args");
const QString kYtLangArg = QStringLiteral("youtube:lang=es");

// Centinelas para m_winningClient (índices de cliente Innertube son >= 0).
constexpr int kResolveYtDlp = -1;      // yt-dlp fue quien resolvió
constexpr int kResolveUnknown = -2;    // aún sin ganador registrado

// Ruta del binario yt-dlp que lanza la app (QProcess). La app arranca el
// proceso como "yt-dlp", así que Windows solo lo encuentra si existe un
// archivo con ese nombre en PATH o junto al exe; si el instalador lo trae con
// OTRo nombre (p. ej. el antiguo `ytdlp.exe`), QProcess no lo halla y
// búsquedas/canales/fallbacks devuelven vacío (parseVideos({}) crea 1 vídeo
// falso "Sin título"). Orden de resolución:
//   1) yt-dlp.exe junto al ejecutable (lo que empaqueta el instalador);
//   2) ytdlp.exe junto al ejecutable (instalaciones antiguas);
//   3) un "yt-dlp" ya presente en el PATH del sistema.
QString ytDlpBinary()
{
    const QString dir = QCoreApplication::applicationDirPath();
    const QString packaged = dir + QStringLiteral("/yt-dlp.exe");
    if (QFileInfo::exists(packaged))
        return packaged;
    const QString legacy = dir + QStringLiteral("/ytdlp.exe");
    if (QFileInfo::exists(legacy))
        return legacy;
    const QString inPath = QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
    if (!inPath.isEmpty())
        return inPath;
    return QStringLiteral("yt-dlp");
}

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
                    if (lang.startsWith(prefLang, Qt::CaseInsensitive))
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

void Yt::setCodecProfile(CodecProfile profile)
{
    m_codecProfile = profile;
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
        NT_DEBUG("E2E channelInfo id=%s name=%s subs=%lld\n",
                 channelId.toUtf8().constData(),
                 info.name.toUtf8().constData(),
                 static_cast<long long>(info.subscriberCount));
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

void Yt::resolveStream(const QString &videoId, int maxHeight,
                       bool forceRefresh)
{
    const int winning = m_winningClient.value(videoId, kResolveUnknown);
    NT_DEBUG("E2E resolveStream id=%s maxH=%d winner=%d refresh=%d\n",
             videoId.toUtf8().constData(), maxHeight, winning,
             int(forceRefresh));
    if (winning == kResolveYtDlp)
        resolveWithYtDlp(videoId, maxHeight, /*fallbackToInnertube*/ true);
    else
        resolveViaInnertube(videoId, maxHeight, winning, forceRefresh);
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
                qWarning("yt: innertube falló para %s (%s); fallback yt-dlp",
                         vid.toUtf8().constData(), reason.toUtf8().constData());
                resolveWithYtDlp(videoId, maxHeight);
            });

    // HLS primario (master de YouTube, sin throttle), edge como respaldo:
    // handlePlayer emite formatsReady y hlsReady en el mismo instante, pero el
    // flujo HLS necesita un round-trip (fetchMaster) antes de emitir streamReady.
    // El edge (URL adaptiva directa) es ~32x más lento (throttle de YouTube)
    // pero no requiere ese round-trip. Para no perder la velocidad del HLS, lo
    // dejamos ganar: formatsReady solo GUARDA el combo edge como respaldo
    // (m_edgeFallback) sin emitirlo; masterReady gana si el master queda
    // filtrable, y si no, se emite el edge guardado.
    QSharedPointer<QString> edgeUrl(new QString),
                                     edgeAudioUrl(new QString);
    QSharedPointer<int> edgeH(new int(0));
    // HLS sintético de AV1 (si el perfil lo prefiere): se construye en
    // formatsReady y gana en hlsReady, antes de pedir el master de YouTube.
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
                    return; // sin combo: solo el flujo HLS lo cubre
                *edgeUrl = videoUrl;
                *edgeAudioUrl = audioUrl;
                *edgeH = edgeHeight(formats, maxHeight);
                NT_DEBUG("E2E edge respaldo %dp %s\n", *edgeH,
                         videoUrl.toUtf8().constData());
            });

    // Emite el HLS de AV1 si formatsReady lo construyó (siempre antes que
    // hlsReady) y hay audio. Devuelve true si ya se emitió streamReady.
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
                // AV1 por HLS primero: el master de YouTube no trae AV1 (solo
                // VP9/h264), así que si hay AV1 sintético se emite ya y se
                // evita el round-trip de fetchMaster.
                if (emitAv1()) {
                    guard->deleteLater();
                    return;
                }
                // HLS primario: el edge solo se emite como respaldo si el master
                // no es filtrable (en masterReady) o si innertube falla.
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
                    [this, filterGuard, innTube, videoId, wanted, gen,
                     edgeUrl, edgeAudioUrl, edgeH](
                        const QString &fid, const QByteArray &masterBody) {
                        if (fid != videoId || gen != m_resolveGen)
                            return;
                        const QString abs = buildFilteredMaster(
                            videoId, masterBody, wanted);
                        if (abs.isEmpty()) {
                            // Master no filtrable: si hay edge guardado,
                            // entregarlo antes de caer a yt-dlp.
                            filterGuard->deleteLater();
                            if (edgeUrl->isEmpty()) {
                                // Sin edge de respaldo: fallback yt-dlp.
                                qWarning("yt: master sin variantes filtrables para %s; "
                                         "fallback yt-dlp",
                                         videoId.toUtf8().constData());
                                resolveWithYtDlp(videoId, wanted);
                                return;
                            }
                            StreamInfo se;
                            se.url = *edgeUrl;
                            se.audioUrl = *edgeAudioUrl;
                            se.label =
                                QString::number(*edgeH) + QLatin1Char('p');
                            se.height = *edgeH;
                            se.multipleUrls = false;
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
                        m_winningClient[videoId] = innTube->lastWinClient();
                        NT_DEBUG("E2E hls filtrado a %dp url=%s\n", wanted,
                                 s.url.toUtf8().constData());
                        filterGuard->deleteLater();
                        emit streamReady(s);
                    });
                // Fracaso en la fase de master: si hay edge guardado,
                // entregarlo antes de caer a yt-dlp.
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
                            m_winningClient[videoId] = innTube->lastWinClient();
                            NT_DEBUG("E2E edge respaldo %dp url=%s\n", *edgeH,
                                     edgeUrl->toUtf8().constData());
                            emit streamReady(se);
                            return;
                        }
                        qWarning("yt: innertube falló al filtrar master (%s); "
                                 "fallback yt-dlp",
                                 reason.toUtf8().constData());
                        resolveWithYtDlp(videoId, maxHeight);
                    });
                Q_UNUSED(masterFail)
                innTube->fetchMaster(vid, hlsUrl);
            });

    m_innertube.fetchHls(videoId, preferredClient, forceRefresh);
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
                    resolveViaInnertube(videoId, maxHeight, -1,
                                        /*forceRefresh*/ true);
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
                                    audioSelector(),
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

// Puntuación de un formato de vídeo: preferencia de códec (×100000) + altura.
// Reno: AV1 > VP9 > h264. La preferencia pesa más que la altura, de modo que el
// códec que la GPU decodifica por hardware gana aunque su variante sea más baja
// (p.ej. Legacy elige h264 1080 antes que VP9 2160, que iría a software).
int Yt::codecRankedScore(int height, const QString &mimeOrCodecs) const
{
    const bool isH264 = mimeOrCodecs.contains(QLatin1String("avc1"))
                        || mimeOrCodecs.contains(QLatin1String("avc3"));
    const bool isAV1 = mimeOrCodecs.contains(QLatin1String("av01"));
    // webm VP9 puede venir como codecs="vp9" o codecs="vp09.00.10.08".
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
        else if (isAV1) rank = -1; // AV1 sin hwdec = software caro, evitarlo
        break;
    case CodecProfile::Legacy:
    default:
        rank = isH264 ? 2 : 0;
        break;
    }
    return rank * 100000 + height;
}

// La mayor altura <= maxHeight de los adaptiveFormats con url directa,
// según el perfil de códec preferido (misma regla que buildEdgeMaster).
// -1 si no hay vídeo.
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
// Elige un único vídeo (la mayor altura <= maxHeight, prefiriendo h264 por
// decodificación ligera en equipos modestos) y un único audio (el que mejor
// coincide con audioLang; audioLang vacío == original). Devuelve la URL
// directa del vídeo y escribe audioUrlOut con la del audio (mpv los fusiona
// vía --audio-file). mp4/fMP4 y webm/VP9 funcionan por URL directa (webm no
// soporta HLS de segmento único). Reproducir por URL directa da a mpv
// duración exacta y seek fiable: el HLS de segmento único perdía ambos y
// hacía saltar la barra de progreso. Devuelve vacío si no hay combinación
// aprovechable (sin url en los formats, o sin audio/vídeo).
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
            continue; // entradas firmadas (signatureCipher) sin descifrar
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
            // El prefijo del id es el idioma ("en.4" → "en", "zh-Hant.3").
            QString idLang = trackId;
            const int dot = idLang.indexOf(QLatin1Char('.'));
            if (dot > 0)
                idLang.truncate(dot);
            bool match = false;
            if (wantOriginal) {
                // "original": el displayName lo marca ("Inglés original"),
                // y acont=original distingue la pista base del doblaje.
                if (displayName.contains(QLatin1String("original"),
                                         Qt::CaseInsensitive))
                    match = true;
                else if (isDefault && lang.isEmpty()
                         && !displayName.isEmpty())
                    match = true; // única pista real de audio del vídeo
                else if (lang.isEmpty() && trackId.isEmpty()
                         && displayName.isEmpty()) {
                    // Sin audioTrack (player sin multiaudio): asumir original
                    // solo si es la única pista de audio del grupo.
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
                score += 10; // el original explícito manda sobre el default
            const int abr = f.value(QStringLiteral("bitrate")).toInt();
            if (abr > 0)
                score += qMin(abr / 1000, 200);
            if (score > bestAudioScore) {
                bestAudioScore = score;
                bestAudio = f;
            }
        }
    }

    // Fallback por si el combo principal no dio audio: el idioma pedido no
    // existe en este vídeo (audioLang con audioTrack distinto) o el original
    // no está marcado. Contrato del settings: "si no existe el idioma, se usa
    // el audio original" — NUNCA un doblaje cualquiera. Orden: pista marcada
    // "original" > pista isDefault > pista sin audioTrack (vídeo mono-pista)
    // > primera pista de audio (último recurso, p. ej. doblaje regional sin
    // marcador). Sin este paso el vídeo quedaría mudo.
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
            // Conservar solo UNA variante: la mejor por códec preferido y
            // altura (<= maxHeight). Reduce mpv de 22 sub-playlists (~20s) a 1
            // (~2-4s) y respeta el códec que la GPU decodifica por hardware.
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

    // Reconstruir salida: cabecera + audio + la variante elegida.
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

// HLS sintético de AV1: YouTube no incluye AV1 en su master HLS (solo
// VP9/h264), y su URL adaptativa directa se estrangula (~150 KB/s). En cambio,
// cada petición Range corta (~2 MB) no está estrangulada: una petición por
// segmento evita el throttle. Se genera una media playlist fMP4 con
// #EXT-X-MAP (initRange) y segmentos #EXT-X-BYTERANGE de ~2 MB. La URL de cada
// segmento inserta un sufijo /seg.mp4 tras /videoplayback (extensión que el
// demuxer HLS acepta; YouTube ignora ese sufijo) y un &nt=<n> distinto por
// segmento: sin ese distintivo ffmpeg fusiona los rangos contiguos del mismo
// recurso en una única petición y el throttle vuelve. El audio va aparte
// (PlayerWidget::setAudioFile), como en el modo edge.
// Baja un rango de bytes de la URL firmada (bloqueante, con timeout). Solo se
// usa para el indexRange (unos pocos KB) antes de construir la playlist AV1.
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

// Caja sidx (ISO/IEC 14496-12) del indexRange: lista de subsegmentos con su
// tamaño exacto y duración. Necesaria para que #EXT-X-BYTERANGE caiga en las
// fronteras reales del fMP4 y el seek funcione.
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

    // Tamaño y duración del recurso: la propia URL firmada los trae (clen/dur).
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

    // Sufijo de ruta ignorado por YouTube pero con extensión que el demuxer
    // HLS acepta (evita exigir protocol_whitelist/extension_picky).
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
        // Un segmento HLS = un fragmento real del fMP4: el demuxer puede
        // aterrizar en cualquier frontera (seek fiable).
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
        // Respaldo (sin sidx): trozos fijos de ~2 MB. Sirve para reproducción
        // secuencial, pero el seek puede caer a mitad de fragmento y fallar.
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

// Escribe en disco la playlist de av1HlsPlaylist() para servida por HlsServer.
QString Yt::buildAv1Hls(const QString &videoId,
                        const QJsonArray &adaptiveFormats, int maxHeight,
                        int *heightOut)
{
    // Selecciona el mismo AV1 <= maxHeight que av1HlsPlaylist para bajar su
    // indexRange (sidx) y alinear cada segmento a un fragmento real del fMP4
    // (sin esto, el seek cae a mitad de fragmento y mpv no carga vídeo).
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
    // Escritura atómica (tmp + rename): la recuperación reescribe la misma
    // ruta mientras mpv puede estar leyéndola; sin esto leería una playlist
    // truncada.
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

QString Yt::formatSelector(int maxHeight)
{
    return QStringLiteral("best[height<=%1][ext=mp4][protocol=https][acodec^=mp4a]/"
                          "best[height<=%1][ext=mp4][protocol^=http][acodec^=mp4a]")
        .arg(maxHeight);
}

// Selector de audio para la vía de respaldo yt-dlp: si hay idioma pedido,
// que yt-dlp elija una pista con ese idioma (cq "es", "es-ES") antes de caer
// al mejor audio a secas. Consistente con el resto de la app (audioLang).
QString Yt::audioSelector() const
{
    if (m_audioLang.isEmpty())
        return QStringLiteral("bestaudio[protocol=m3u8_native]/bestaudio");
    return QStringLiteral("bestaudio[language^=%1]/"
                          "bestaudio[protocol=m3u8_native]/bestaudio")
        .arg(m_audioLang);
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
    auto timedOut = std::make_shared<bool>(false);
    QObject::connect(process,
                     static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
                         &QProcess::finished),
                     process, [this, process, done, fired, timedOut]() {
                         if (*fired)
                             return;
                         *fired = true;
                         if (*timedOut) {
                             // kill() por timeout: responder igualmente y
                             // liberar el proceso. Sin esto el llamante nunca
                             // recibe done() y el objeto QProcess queda vivo.
                             Q_EMIT error(QStringLiteral(
                                 "yt-dlp tardó demasiado (timeout)."));
                             if (done)
                                 done(QJsonObject());
                             process->deleteLater();
                             return;
                         }
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
    QTimer::singleShot(120000, process, [process, timedOut]() {
        *timedOut = true;
        process->kill();
    });
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->start(ytDlpBinary(), args);
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
    auto timedOut = std::make_shared<bool>(false);
    QObject::connect(process,
                     static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(
                         &QProcess::finished),
                     process, [this, process, done, fired, timedOut]() {
                         if (*fired)
                             return;
                         *fired = true;
                         if (*timedOut) {
                             Q_EMIT error(QStringLiteral(
                                 "yt-dlp tardó demasiado (timeout)."));
                             if (done)
                                 done(QStringList());
                             process->deleteLater();
                             return;
                         }
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
    QTimer::singleShot(120000, process, [process, timedOut]() {
        *timedOut = true;
        process->kill();
    });
    process->setProcessChannelMode(QProcess::SeparateChannels);
    process->start(ytDlpBinary(), args);
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

// Selftest de la ruta de BÚSQUEDA yt-dlp de la app (usada por search, el
// fallback de canales y el de trending): lanza Yt::search con 3 resultados y
// exige vídeos con id real. Detecta el fallo "yt-dlp no inicia" (binario mal
// nombrado o no encontrado): en ese caso execJson llama done({}) y
// parseVideos({}) devuelve 1 vídeo falso con id='' ("Sin título") → aquí
// falla con idVacios>0.
bool searchSelfTest()
{
    std::fprintf(stderr, "SEARCH SELFTEST start\n");
    Yt yt;
    bool done = false;
    // Watchdog global: la búsqueda hace red real; no colgar el selftest.
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
                         for (const VideoInfo &v : videos) {
                             if (v.id.isEmpty())
                                 ++emptyIds;
                         }
                         const bool ok = !videos.isEmpty() && emptyIds == 0;
                         std::fprintf(stderr,
                                      "SEARCH videos=%d idVacios=%d -> %s\n",
                                      videos.size(), emptyIds,
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

    // --- sidx: un segmento HLS por fragmento real (seek fiable) ---
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
    be32(box, 0);                                   // box size (abajo)
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