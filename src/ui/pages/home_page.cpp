#include "home_page.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include "../../debug.h"
#include "../../db/repositories.h"
#include "../../models.h"
#include "../../yt.h"
#include "../icons.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

namespace {

const int kGridW = 320;
const int kGridH = 258;

// Altura que reserva el mini player (miniatura 63 + márgenes/progreso). Es el
// espacio inferior adicional que se añade al contenido desplazable cuando el
// mini player está visible, para que la última sección quede por encima de él.
const int kMiniBarReservedPx = 96;

// Recolección del feed de suscripciones por canal: máximo de canales
// consultados por actualización y …nº de vídeos por canal (5) a partir del
// cual se deja de pedir en uno concreto. Cada canal aporta hasta 5 recientes.
const int kFeedMaxChannels = 6;
const int kFeedPerChannel = 5;
const int kFeedTargetVideos = kFeedMaxChannels * kFeedPerChannel;

// TTLs de caché por sección (ms). TTL != polling: al entrar Home se muestra lo
// cached al momento y si caducó se refresca UNA vez en segundo plano.
const qint64 kFeedTtlMs = 30 * 60 * 1000;
const qint64 kHomeTtlMs = 60 * 60 * 1000;
const qint64 kTrendTtlMs = 2 * 60 * 60 * 1000;
const qint64 kDiscoverTtlMs = 60 * 60 * 1000;
// Backoff de la generación de inicialización de Descubre (cuando no hay radios
// guardadas): tras un intento sin resultados se espera antes de reintentar,
// para no martillear la red al abrir Home repetidamente sin conexión.
const qint64 kDiscoverRetryMs = 10 * 60 * 1000;
// Radios de semilla distintas que se intentan guardar en la generación de
// inicialización (excepción: solo ocurre si no hay ninguna radio útil).
const int kDiscoverMinRadios = 3;

// Claves de las secciones de RED en el home_cache (HomeCacheRepo). Continuar
// viendo y Descubre NO se guardan ahí: derivan de historial/radios, que ya son
// persistentes, y Descubre re-selecciona siempre al arrancar.
const char kCacheSecHome[] = "recommended";
const char kCacheSecFeed[] = "feed";
const char kCacheSecTrend[] = "trending";

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

// Entradas válidas: con id y título (más robusto ante respuestas degradadas
// de Innertube: vídeos sin id, playlists rotas que se cuelan).
QVector<VideoInfo> sanitizeVideos(const QVector<VideoInfo> &in)
{
    QVector<VideoInfo> out;
    QSet<QString> seen;
    for (const VideoInfo &v : in) {
        if (v.id.isEmpty() || v.id.size() != 11 || seen.contains(v.id))
            continue;
        seen.insert(v.id);
        out.append(v);
    }
    return out;
}

} // namespace

HomePage::HomePage(Yt *yt, SubscriptionsRepo *subs, HistoryRepo *history,
                   DiscoverRepo *discover, VideoCacheRepo *videoCache,
                   ThumbnailCache *thumbs, SettingsRepo *settings,
                   HomeCacheRepo *homeCache, QWidget *parent)
    : QWidget(parent)
    , m_yt(yt)
    , m_subs(subs)
    , m_history(history)
    , m_discover(discover)
    , m_videoCache(videoCache)
    , m_thumbs(thumbs)
    , m_settings(settings)
    , m_homeCache(homeCache)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_body = new QWidget;
    m_scroll->setWidget(m_body);

    auto *bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout = bodyLayout;
    bodyLayout->setContentsMargins(14, 14, 14, 20);
    bodyLayout->setSpacing(6);

    auto *topRow = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Inicio"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    auto *refresh = new QPushButton(QStringLiteral("  Actualizar"), this);
    refresh->setObjectName(QStringLiteral("ghost"));
    refresh->setIcon(nt::icon(nt::Icon::Refresh, nt::kText, 15));
    topRow->addWidget(title);
    topRow->addStretch(1);
    topRow->addWidget(refresh);
    bodyLayout->addLayout(topRow);

    m_busy = new QLabel(QStringLiteral("Cargando…"), this);
    m_busy->setObjectName(QStringLiteral("sectionSub"));
    m_busy->setAlignment(Qt::AlignCenter);
    m_busy->setContentsMargins(0, 20, 0, 20);
    bodyLayout->addWidget(m_busy);

    // ▶ Continuar viendo
    m_contTitle = new QLabel(QStringLiteral("Continuar viendo"), this);
    m_contTitle->setObjectName(QStringLiteral("sectionTitle"));
    m_contGrid = new VideoListView(m_thumbs, this);
    m_contGrid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_contGrid->setFixedHeight(0);
    bodyLayout->addWidget(m_contTitle);
    bodyLayout->addWidget(m_contGrid);

    // 🎲 Descubre
    auto *discRow = new QHBoxLayout;
    discRow->setSpacing(4);
    m_discTitle = new QLabel(QStringLiteral("Descubre"), this);
    m_discTitle->setObjectName(QStringLiteral("sectionTitle"));
    discRow->addWidget(m_discTitle);
    discRow->addStretch(1);
    m_discMoreBtn = new QPushButton(QStringLiteral("Otro"), this);
    m_discMoreBtn->setObjectName(QStringLiteral("ghost"));
    discRow->addWidget(m_discMoreBtn);
    bodyLayout->addLayout(discRow);
    m_discGrid = new VideoListView(m_thumbs, this);
    m_discGrid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_discGrid->setFixedHeight(0);
    bodyLayout->addWidget(m_discGrid);

    // 🔔 Nuevos de tus suscripciones
    m_feedTitle = new QLabel(
        QStringLiteral("Nuevos de tus suscripciones"), this);
    m_feedTitle->setObjectName(QStringLiteral("sectionTitle"));
    m_feedGrid = new VideoListView(m_thumbs, this);
    m_feedGrid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_feedGrid->setFixedHeight(0);
    bodyLayout->addWidget(m_feedTitle);
    bodyLayout->addWidget(m_feedGrid);

    // ✨ Recomendado por YouTube
    m_homeTitle = new QLabel(
        QStringLiteral("Recomendado por YouTube"), this);
    m_homeTitle->setObjectName(QStringLiteral("sectionTitle"));
    m_homeGrid = new VideoListView(m_thumbs, this);
    m_homeGrid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_homeGrid->setFixedHeight(0);
    bodyLayout->addWidget(m_homeTitle);
    bodyLayout->addWidget(m_homeGrid);

    // 🔥 Tendencias
    m_trendTitle = new QLabel(QStringLiteral("Tendencias"), this);
    m_trendTitle->setObjectName(QStringLiteral("sectionTitle"));
    m_trendGrid = new VideoListView(m_thumbs, this);
    m_trendGrid->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_trendGrid->setFixedHeight(0);
    bodyLayout->addWidget(m_trendTitle);
    bodyLayout->addWidget(m_trendGrid);

    outer->addWidget(m_scroll);

    // Las secciones del Home llenan todo el ancho disponible (celdas que se
    // estiran) con los vídeos por fila que marque el ajuste "Tamaño de las
    // miniaturas" (4/5/6, por defecto 5); su altura se deriva del ancho real
    // de cada rejilla.
    for (VideoListView *g : {m_contGrid, m_feedGrid, m_homeGrid,
                             m_discGrid, m_trendGrid}) {
        g->setGridFillWidth(kGridW);
        g->setGridColumns(m_cellColumns);
    }

    // Timer para recalcular alturas de grids después del layout inicial
    // y cuando se llenan videos asíncronamente
    m_layoutTimer = new QTimer(this);
    m_layoutTimer->setSingleShot(true);
    m_layoutTimer->setInterval(50);
    connect(m_layoutTimer, &QTimer::timeout, this,
            &HomePage::applyGridSizes);

    connect(refresh, &QPushButton::clicked, this, &HomePage::reload);
    connect(m_discMoreBtn, &QPushButton::clicked, this,
            &HomePage::onDiscoveryAgain);
    connect(m_yt, &Yt::trendingFinished, this, &HomePage::onTrending);
    connect(m_yt, &Yt::channelVideosFinished, this, &HomePage::onChannelVideos);
    connect(m_yt, &Yt::homeReady, this, &HomePage::onHomeReady);
    connect(m_yt, &Yt::radioReady, this, &HomePage::onRadioReady);
    // Cada reproducción escribe el historial (PlayerController::playIndex →
    // HistoryRepo::add). Sin esta conexión, volver al Home tras reproducir un
    // vídeo no refresca "Continuar viendo" ni oculta el empty-state.
    connect(m_history, &HistoryRepo::changed, this,
            &HomePage::refreshContinueWatching);
    connect(m_history, &HistoryRepo::changed, this, &HomePage::refreshBusy);

    connect(m_contGrid, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                if (index >= 0 && index < m_continuePositions.size())
                    emit resumeRequested(m_continueVideos, index,
                                         m_continuePositions.at(index));
                else
                    emit openRequested(m_continueVideos, index);
            });
    connect(m_feedGrid, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                emit openRequested(m_feed, index);
            });
    connect(m_homeGrid, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                emit openRequested(m_home, index);
            });
    connect(m_discGrid, &VideoListView::videoActivated, this,
            [this](int, const VideoInfo &video) {
                if (!video.id.isEmpty())
                    emit openRequested(QVector<VideoInfo>{video}, 0);
            });
    connect(m_trendGrid, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                emit openRequested(m_trending, index);
            });

    m_contTitle->hide();
    m_contGrid->hide();
    m_feedTitle->hide();
    m_feedGrid->hide();
    m_homeTitle->hide();
    m_homeGrid->hide();
    m_discTitle->hide();
    m_discMoreBtn->hide();
    m_discGrid->hide();
    m_trendTitle->hide();
    m_trendGrid->hide();
    m_busy->show();

    buildEmptyState();

    loadPersistedSections();
}

bool HomePage::cachedFresh(qint64 at, qint64 ttlMs) const
{
    return at > 0 && (nowMs() - at) < ttlMs;
}

// Panel "vacío": se muestra cuando NO hay contenido del usuario
// (sin historial, sin suscripciones, sin radios Descubre). En ese caso la
// app arranca sin poder ofrecer Continuar viendo / Descubre / Nuevos de tus
// suscripciones, y el panel guía al usuario en lugar de dejarlo mirando un
// Home con solo "Recomendado" y "Tendencias". Se oculta en cuanto aparece
// cualquiera de esas secciones (lo controla refreshBusy).
void HomePage::buildEmptyState()
{
    auto *emptyBox = new QFrame(m_body);
    // Estilo por QSS global (QFrame#popupDialog), igual que los otros cuatro
    // popups de la app. Antes llevaba ademas un inline que lo bajaba a kPanel
    // (#181818) sobre el kPopover (#202020) de la regla: era el unico popup
    // con ese fondo, se veia como deriva y no comodecision.
    emptyBox->setObjectName(QStringLiteral("popupDialog"));
    auto *emptyLay = new QVBoxLayout(emptyBox);
    emptyLay->setContentsMargins(28, 24, 28, 24);
    emptyLay->setSpacing(8);
    auto *emptyTitle = new QLabel(
        QStringLiteral("Aún no hay contenido personalizado"), emptyBox);
    emptyTitle->setObjectName(QStringLiteral("emptyStateTitle"));
    emptyTitle->setAlignment(Qt::AlignCenter);
    auto *emptyBody = new QLabel(
        QStringLiteral("Reproducí un vídeo o suscribite a un canal desde el "
                       "buscador para llenar tu Continuar viendo, Descubre y "
                       "Nuevos de tus suscripciones. Mientras tanto, arriba "
                       "tenés Recomendado y Tendencias."),
        emptyBox);
    emptyBody->setObjectName(QStringLiteral("emptyStateBody"));
    emptyBody->setWordWrap(true);
    emptyBody->setAlignment(Qt::AlignCenter);
    emptyLay->addWidget(emptyTitle);
    emptyLay->addWidget(emptyBody);
    auto *emptyRow = new QHBoxLayout;
    emptyRow->setSpacing(8);
    emptyRow->addStretch(1);
    auto *goSearch = new QPushButton(QStringLiteral("Ir a Buscar"), emptyBox);
    goSearch->setCursor(Qt::PointingHandCursor);
    goSearch->setObjectName(QStringLiteral("accentChip"));
    connect(goSearch, &QPushButton::clicked, this,
            [this]() { emit navigateToSearchRequested(); });
    emptyRow->addWidget(goSearch);
    emptyRow->addStretch(1);
    emptyLay->addLayout(emptyRow);
    // Se inserta en el bodyLayout JUSTO ANTES de "Continuar viendo". Si esa
    // sección está oculta, el panel ocupa su lugar y al cerrarse las
    // secciones se reagrupan naturalmente con el spacing del layout.
    if (!m_bodyLayout)
        return;
    int idx = m_bodyLayout->indexOf(m_contTitle);
    if (idx < 0)
        idx = m_bodyLayout->count();
    m_bodyLayout->insertWidget(idx, emptyBox);
    m_emptyState = emptyBox;
}


// Al arrancar se recupera el Home persistido para mostrarlo al instante:
// Recomendado/Feed/Tendencias vienen del home_cache (metadatos resueltos con el
// caché "videos"); su fetched_at cargado decide después, en loadSections(), si
// la sección necesita refresco (solo la que caducó se vuelve a pedir).
void HomePage::loadPersistedSections()
{
    if (!m_homeCache)
        return;
    auto load = [this](const char *section, QVector<VideoInfo> &out,
                       qint64 &fetchedAt) {
        const QStringList ids = m_homeCache->sectionVideoIds(
            QLatin1String(section));
        if (ids.isEmpty())
            return;
        const QVector<VideoInfo> videos = m_videoCache->getMany(ids);
        if (videos.isEmpty())
            return;
        const qint64 at = m_homeCache->sectionFetchedAt(
            QLatin1String(section));
        if (at <= 0)
            return;
        out = videos;
        fetchedAt = at;
    };
    load(kCacheSecHome, m_home, m_homeFetchedAt);
    load(kCacheSecFeed, m_feed, m_feedFetchedAt);
    load(kCacheSecTrend, m_trending, m_trendFetchedAt);
    if (!m_home.isEmpty()) {
        m_homeGrid->showVideos(m_home, VideoListView::GridMode);
        adjustGridHeight(m_homeGrid);
        m_homeTitle->setVisible(true);
        m_homeGrid->setVisible(true);
    }
    if (!m_feed.isEmpty()) {
        m_feedGrid->showVideos(m_feed, VideoListView::GridMode);
        adjustGridHeight(m_feedGrid);
        m_feedTitle->setVisible(true);
        m_feedGrid->setVisible(true);
    }
    if (!m_trending.isEmpty()) {
        m_trendGrid->showVideos(m_trending, VideoListView::GridMode);
        adjustGridHeight(m_trendGrid);
        m_trendTitle->setVisible(true);
        m_trendGrid->setVisible(true);
    }
    if (!m_home.isEmpty() || !m_feed.isEmpty() || !m_trending.isEmpty())
        m_layoutTimer->start();
}

void HomePage::saveSection(const QString &section,
                           const QVector<VideoInfo> &videos)
{
    if (!m_homeCache || videos.isEmpty())
        return;
    // Los metadatos completos van a "videos" (VideoCacheRepo): home_cache solo
    // guarda ids + timestamp, así al reabrir la app getMany(ids) reconstruye la
    // sección aunque sea una instalación nueva y nunca se haya reproducido nada.
    for (const VideoInfo &v : videos)
        m_videoCache->upsert(v);
    QStringList ids;
    ids.reserve(videos.size());
    for (const VideoInfo &v : videos)
        ids << v.id;
    m_homeCache->saveSection(section, ids, nowMs());
}

void HomePage::setCurrentVideoId(const QString &id)
{
    m_currentVideoId = id;
}

void HomePage::reload()
{
    loadContinueWatching();
    loadSections();
}

void HomePage::refreshContinueWatching()
{
    // El historial se escribe cada kPositionFlushMs (20 s) mientras se
    // reproduce, y este slot está conectado a su changed(). Reconstruir la
    // rejilla con la página OCULTA (se reproduce en el mini player, que es el
    // caso normal) es trabajo tirado: al entrar al Home, onNavButton llama a
    // refreshDynamic() y entonces sí se reconstruye con los datos frescos.
    if (!isVisible())
        return;
    loadContinueWatching();
}

void HomePage::refreshDynamic()
{
    loadContinueWatching();
    maybeInvalidateFeed();
    buildDiscoverSelection();
}

// Re-aplica el acento de la pagina. El boton del estado vacio ya no lleva
// stylesheet propio: es QPushButton#accentChip, y nt::styleSheet() se regenera
// entero en cada cambio de acento, asi que el boton se repinta solo y aqui no
// queda nada que hacer. Se mantiene el metodo porque MainWindow lo llama en las
// cuatro paginas por igual.
void HomePage::applyAccent()
{
}

QString HomePage::currentSubsSignature() const
{
    const auto channels = m_subs->all();
    QStringList ids;
    ids.reserve(channels.size());
    for (const SubscriptionInfo &s : channels)
        ids << s.id;
    ids.sort();
    return ids.join(QLatin1Char(','));
}

void HomePage::maybeInvalidateFeed()
{
    if (!m_settings)
        return;
    const QString sig = currentSubsSignature();
    if (sig == m_settings->getString(QStringLiteral("feed_subs_sig")))
        return;
    NT_DEBUG("E2E feed subs cambiaron: invalidando caché\n");
    m_feedFetchedAt = 0;
    if (!m_feedBusy)
        loadFeed();
}

// 1) ▶ Continuar viendo: historial local, sin red. Progreso entre 0 y 100%,
//    con duración conocida, ordenados por última reproducción (máx 4).
void HomePage::loadContinueWatching()
{
    m_continueVideos.clear();
    m_continuePositions.clear();
    const QStringList recent = m_history->recentIds(50);
    QVector<VideoInfo> out;
    QVector<QPair<QString, double>> progress;
    for (const QString &id : recent) {
        if (out.size() >= 4)
            break;
        const qlonglong pos = m_history->positionOf(id);
        if (pos <= 0)
            continue;
        VideoInfo v;
        if (!m_videoCache->get(id, v))
            continue;
        const double frac = HistoryRepo::progressFraction(v, pos);
        if (frac <= 0.0)
            continue;
        out.append(v);
        m_continuePositions.append(pos);
        progress.append({id, frac});
    }
    m_continueVideos = out;
    m_contGrid->showVideos(m_continueVideos, VideoListView::GridMode);
    for (const auto &p : progress)
        m_contGrid->setProgress(p.first, p.second);
    adjustGridHeight(m_contGrid);
    m_contTitle->setVisible(!out.isEmpty());
    m_contGrid->setVisible(!out.isEmpty());
}

// Carga/refresca todas las secciones de red. Cada una: si la caché es fresca
// no pide nada; si caducó, pide UNA vez (single-flight por m_*Busy) en segundo
// plano mientras muestra lo cached.
void HomePage::loadSections()
{
    // 2) 🔔 suscripciones
    maybeInvalidateFeed();
    if (!cachedFresh(m_feedFetchedAt, kFeedTtlMs) && !m_feedBusy)
        loadFeed();

    // 3) ✨ Recomendado por YouTube
    if (!cachedFresh(m_homeFetchedAt, kHomeTtlMs) && !m_homeBusy) {
        m_homeBusy = true;
        NT_DEBUG("E2E home fetchHome\n");
        m_yt->home();
    }

    // 🎲 Descubre (solo cuando hay semilla válida)
    ensureDiscover();

    // 5) 🔥 Tendencias
    if (!cachedFresh(m_trendFetchedAt, kTrendTtlMs) && !m_trendBusy) {
        m_trendBusy = true;
        m_yt->trending(8);
    }

    refreshBusy();
}

void HomePage::loadFeed()
{
    const auto channels = m_subs->all();
    m_feedSubsSig = currentSubsSignature();
    m_pendingChannels.clear();
    // Se conserva la sección mostrada: si la actualización acaba vacía/falla,
    // onFeedDone la restaura en vez de ocultarla o reemplazarla por vacío.
    m_prevFeed = m_feed;
    m_feed.clear();
    m_feedBusy = true;
    if (channels.isEmpty()) {
        m_feedBusy = false;
        m_feedTitle->hide();
        m_feedGrid->hide();
        m_feedFetchedAt = nowMs();
        if (m_settings)
            m_settings->set(QStringLiteral("feed_subs_sig"), m_feedSubsSig);
        refreshBusy();
        return;
    }
    m_channelNames.clear();
    for (const SubscriptionInfo &s : channels) {
        m_channelNames.insert(s.id, s.title);
        m_pendingChannels << s.id;
        if (!s.avatar.isEmpty())
            m_thumbs->requestChannelAvatar(s.id, s.avatar);
    }
    m_pendingIdx = 0;
    refreshBusy();
    requestNextChannel();
}

void HomePage::requestNextChannel()
{
    if (m_pendingIdx >= m_pendingChannels.size()
        || m_pendingIdx >= kFeedMaxChannels
        || m_feed.size() >= kFeedTargetVideos) {
        m_feedBusy = false;
        onFeedDone();
    } else {
        m_yt->channelVideos(m_pendingChannels.at(m_pendingIdx), 10);
    }
}

void HomePage::onChannelVideos(const QVector<VideoInfo> &videos)
{
    QVector<VideoInfo> patched = videos;
    // Los items del tab "Vídeos" del canal no llevan nombre de canal: se
    // rellena con la suscripción pedida (Innertube).
    const QString cid = m_pendingIdx < m_pendingChannels.size()
                            ? m_pendingChannels.at(m_pendingIdx)
                            : QString();
    for (VideoInfo &v : patched) {
        if (v.id.isEmpty())
            continue;
        if (v.channel.isEmpty() && !cid.isEmpty())
            v.channel = m_channelNames.value(cid);
        if (v.channelId.isEmpty())
            v.channelId = cid;
    }
    // Hasta 5 recientes por canal, ordenando ese canal por fecha.
    std::stable_sort(patched.begin(), patched.end(), [](const VideoInfo &a,
                                                        const VideoInfo &b) {
        const QDateTime ad = QDateTime::fromString(a.publishedAt, Qt::ISODate);
        const QDateTime bd = QDateTime::fromString(b.publishedAt, Qt::ISODate);
        if (ad.isValid() && bd.isValid())
            return bd < ad;
        if (ad.isValid() != bd.isValid())
            return ad.isValid();
        return false;
    });
    if (patched.size() > kFeedPerChannel)
        patched.resize(kFeedPerChannel);
    m_feed += patched;
    m_pendingIdx++;
    requestNextChannel();
}

void HomePage::onFeedDone()
{
    m_feedBusy = false;
    QVector<VideoInfo> fresh = sanitizeVideos(m_feed);
    if (fresh.isEmpty()) {
        // Actualización vacía/fallida: se conserva la sección cacheada anterior
        // (memoria y UI), no se reemplaza por vacío ni se refresca fetched_at.
        m_feed = m_prevFeed;
        m_prevFeed.clear();
        NT_DEBUG("E2E feed vacío: se conserva la sección anterior n=%d\n",
                 (int)m_feed.size());
        refreshBusy();
        ensureDiscover();
        return;
    }
    std::stable_sort(fresh.begin(), fresh.end(), [](const VideoInfo &a,
                                                    const VideoInfo &b) {
        const QDateTime ad = QDateTime::fromString(a.publishedAt, Qt::ISODate);
        const QDateTime bd = QDateTime::fromString(b.publishedAt, Qt::ISODate);
        if (ad.isValid() && bd.isValid())
            return bd < ad;
        if (ad.isValid() != bd.isValid())
            return ad.isValid();
        return false;
    });
    if (fresh.size() > kFeedTargetVideos)
        fresh.resize(kFeedTargetVideos);
    m_feed = fresh;
    m_prevFeed.clear();
    m_feedFetchedAt = nowMs();
    saveSection(QLatin1String(kCacheSecFeed), m_feed);
    if (m_settings)
        m_settings->set(QStringLiteral("feed_subs_sig"), m_feedSubsSig);
    m_feedGrid->showVideos(m_feed, VideoListView::GridMode);
    adjustGridHeight(m_feedGrid);
    const bool vis = !m_feed.isEmpty();
    m_feedTitle->setVisible(vis);
    m_feedGrid->setVisible(vis);
    NT_DEBUG("E2E feed setVisible=%d height=%d visible=%d\n", vis, m_feedGrid->height(), m_feedGrid->isVisible());
    // Recalcular alturas después de que el layout se estabilice
    m_layoutTimer->start();
    refreshBusy();
    ensureDiscover();
}

void HomePage::onHomeReady(const QVector<VideoInfo> &videos)
{
    NT_DEBUG("E2E home ready count=%d\n", (int)videos.size());
    m_homeBusy = false;
    const QVector<VideoInfo> fresh = sanitizeVideos(videos);
    if (!fresh.isEmpty()) {
        m_home = fresh;
        m_homeFetchedAt = nowMs();
        saveSection(QLatin1String(kCacheSecHome), m_home);
        m_homeGrid->showVideos(m_home, VideoListView::GridMode);
        adjustGridHeight(m_homeGrid);
        const bool vis = !m_home.isEmpty();
        m_homeTitle->setVisible(vis);
        m_homeGrid->setVisible(vis);
        NT_DEBUG("E2E home setVisible=%d height=%d visible=%d\n", vis, m_homeGrid->height(), m_homeGrid->isVisible());
        // Recalcular alturas después de que el layout se estabilice
        m_layoutTimer->start();
    } else {
        // Actualización fallida/vacía: se conserva la sección cacheada anterior
        // (memoria y UI), sin reemplazarla por vacío ni tocar fetched_at.
        NT_DEBUG("E2E home vacío: se conserva la sección anterior n=%d\n",
                 (int)m_home.size());
    }
    refreshBusy();
    ensureDiscover();
}

void HomePage::onTrending(const QVector<VideoInfo> &videos)
{
    NT_DEBUG("E2E trending count=%d\n", (int)videos.size());
    if (!videos.isEmpty()) {
        NT_DEBUG("E2E trending first id='%s' size=%d title='%s'\n", 
                 videos[0].id.toUtf8().constData(), videos[0].id.size(),
                 videos[0].title.left(40).toUtf8().constData());
    }
    m_trendBusy = false;
    QVector<VideoInfo> fresh = sanitizeVideos(videos);
    NT_DEBUG("E2E trending after sanitize=%d\n", (int)fresh.size());
    if (!fresh.isEmpty()) {
        if (fresh.size() > 8)
            fresh.resize(8);
        m_trending = fresh;
        m_trendFetchedAt = nowMs();
        saveSection(QLatin1String(kCacheSecTrend), m_trending);
        m_trendGrid->showVideos(m_trending, VideoListView::GridMode);
        adjustGridHeight(m_trendGrid);
        const bool vis = !m_trending.isEmpty();
        m_trendTitle->setVisible(vis);
        m_trendGrid->setVisible(vis);
        NT_DEBUG("E2E trending setVisible=%d height=%d visible=%d\n", vis, m_trendGrid->height(), m_trendGrid->isVisible());
        m_layoutTimer->start();
    } else {
        // Actualización fallida/vacía: se conserva la sección cacheada anterior
        // (memoria y UI), sin reemplazarla por vacío ni tocar fetched_at.
        NT_DEBUG("E2E trending vacío: se conserva la sección anterior n=%d\n",
                 (int)m_trending.size());
    }
    refreshBusy();
    ensureDiscover();
}

// 4) 🎲 Descubre: fuente PERSISTENTE (SQLite). Abrir Home NUNCA genera radios
// por sí solo: si la caché en memoria caducó, se re-seleccionan hasta 5 vídeos
// de las radios YA GUARDADAS (pickDiscoverVideos, sin red). Las radios se
// guardan al reproducir vídeos (MainWindow.onRadioReadyDiscover) y, como
// EXCEPCIÓN de inicialización, cuando no existe ninguna radio útil la propia
// sección las genera con semillas disponibles (historial/recomendado/
// suscripciones/tendencias) y las guarda antes de consumirlas.
void HomePage::ensureDiscover()
{
    if (m_discoverBusy)
        return;
    if (!m_discoverVideos.isEmpty()
        && cachedFresh(m_discoverFetchedAt, kDiscoverTtlMs)) {
        showDiscover();
        return;
    }
    buildDiscoverSelection();
}

// Selección de Descubre consumiendo SOLO las radios guardadas. Las radios sin
// vídeo útil se descartan en el propio pick. Si no queda ninguna, se dispara la
// generación de inicialización (con backoff), única vía en la que Home toca
// radioVideos() y solo la primera vez que la app se siembra (ver
// discoverSeeded).
void HomePage::buildDiscoverSelection()
{
    m_discoverVideos = pickDiscoverVideos(*m_discover, *m_videoCache,
                                          m_currentVideoId);
    m_discoverFetchedAt = nowMs();
    m_discoverEmptyAt = 0;
    showDiscover();
    if (m_discoverVideos.isEmpty() && m_discover->count() == 0 && m_yt
        && !discoverSeeded()
        && !cachedFresh(m_discoverEmptyAt, kDiscoverRetryMs)) {
        startDiscoverSeedGen();
    } else if (m_discoverVideos.isEmpty()) {
        m_discoverEmptyAt = nowMs();
    }
}

bool HomePage::discoverSeeded() const
{
    return m_settings
        && m_settings->getInt(QStringLiteral("discover_seeded"), 0) == 1;
}

void HomePage::markDiscoverSeeded()
{
    if (m_settings)
        m_settings->set(QStringLiteral("discover_seeded"),
                        QStringLiteral("1"));
}

// Semillas para la generación de radios de la excepción de inicialización,
// variadas y barajadas: historial reciente (reusa el caché de metadatos local),
// recomendado, suscripciones y tendencias. Se excluyen el vídeo en reproducción
// y los duplicados.
void HomePage::buildDiscoverSeeds()
{
    m_discoverSeeds.clear();
    QSet<QString> seen;
    auto addVideos = [&](const QVector<VideoInfo> &list) {
        for (const VideoInfo &v : list) {
            if (v.id.size() != 11 || v.id == m_currentVideoId
                || seen.contains(v.id))
                continue;
            seen.insert(v.id);
            m_discoverSeeds << v.id;
        }
    };
    addVideos(m_home);
    addVideos(m_feed);
    addVideos(m_trending);
    const QStringList recent = m_history->recentIds(20);
    for (const QString &id : recent) {
        if (seen.contains(id) || id == m_currentVideoId)
            continue;
        VideoInfo v;
        if (!m_videoCache->get(id, v) || v.id.size() != 11)
            continue;
        seen.insert(id);
        m_discoverSeeds << v.id;
    }
    for (int i = m_discoverSeeds.size() - 1; i > 0; --i) {
        const int j = QRandomGenerator::global()->bounded(i + 1);
        m_discoverSeeds.swapItemsAt(i, j);
    }
    NT_DEBUG("E2E discover gen seeds=%d\n", (int)m_discoverSeeds.size());
}

void HomePage::startDiscoverSeedGen()
{
    m_discoverTried.clear();
    m_discoverSeedId.clear();
    buildDiscoverSeeds();
    if (m_discoverSeeds.isEmpty()) {
        m_discoverEmptyAt = nowMs();
        m_discoverBusy = false;
        refreshBusy();
        return;
    }
    m_discoverBusy = true;
    refreshBusy();
    fetchNextDiscoverSeed();
}

// Pide la radio de la siguiente semilla. UNA a UNA: Innertube.fetchRadio() es
// single-flight. Las semillas que ya tienen radio guardada se saltan (no se
// regenera). Se detiene al reunir kDiscoverMinRadios.
void HomePage::fetchNextDiscoverSeed()
{
    if (m_discover->count() >= kDiscoverMinRadios) {
        finishDiscoverSeedGen();
        return;
    }
    while (!m_discoverSeeds.isEmpty()) {
        const QString seed = m_discoverSeeds.takeFirst();
        if (m_discoverTried.contains(seed))
            continue;
        if (m_discover->hasRadio(seed))
            continue;
        m_discoverTried.insert(seed);
        m_discoverSeedId = seed;
        NT_DEBUG("E2E discover gen fetch seed=%s\n",
                 seed.toUtf8().constData());
        m_yt->radioVideos(seed);
        return;
    }
    finishDiscoverSeedGen();
}

// En el radioReady solo interesan las radios pedidas por el propio Descubre
// (m_discoverSeedId). Las radios del reproductor ("A continuación") llegan con
// otra semilla y se ignoran (MainWindow las guarda aparte). La radio se guarda
// y se sigue con la siguiente semilla hasta reunir kDiscoverMinRadios.
void HomePage::onRadioReady(const QString &videoId,
                            const QVector<VideoInfo> &videos)
{
    if (videoId != m_discoverSeedId)
        return;
    m_discoverSeedId.clear();
    const QVector<VideoInfo> valid = sanitizeVideos(videos);
    NT_DEBUG("E2E discover gen radio seed=%s n=%d\n",
             videoId.toUtf8().constData(), (int)valid.size());
    if (!valid.isEmpty()) {
        for (const VideoInfo &v : valid)
            m_videoCache->upsert(v);
        m_discover->saveRadio(videoId, valid);
    }
    if (m_discover->count() >= kDiscoverMinRadios)
        finishDiscoverSeedGen();
    else
        fetchNextDiscoverSeed();
}

void HomePage::finishDiscoverSeedGen()
{
    m_discoverSeedId.clear();
    m_discoverSeeds.clear();
    m_discoverBusy = false;
    if (m_discover->count() == 0)
        m_discoverEmptyAt = nowMs(); // sin resultados: reintentar con backoff
    else
        markDiscoverSeeded(); // ya hubo siembra: no volver a auto-rellenar
    refreshBusy();
    buildDiscoverSelection();
}

// "↻ Otro": re-selecciona usando las radios YA GUARDADAS (no regenera, no pide
// nada por red). Solo si no queda ninguna radio útil cae en la excepción de
// inicialización.
void HomePage::onDiscoveryAgain()
{
    interruptDiscover();
    m_discoverVideos.clear();
    m_discoverFetchedAt = 0;
    ensureDiscover();
}

void HomePage::interruptDiscover()
{
    m_discoverSeedId.clear();
    m_discoverSeeds.clear();
    m_discoverBusy = false;
}

void HomePage::invalidateDiscover()
{
    m_discoverVideos.clear();
    m_discoverFetchedAt = 0;
    m_discoverEmptyAt = 0;
    if (m_discoverBusy)
        return;
    buildDiscoverSelection();
}

void HomePage::showDiscover()
{
    const bool vis = !m_discoverVideos.isEmpty();
    m_discGrid->showVideos(m_discoverVideos, VideoListView::GridMode);
    adjustGridHeight(m_discGrid);
    m_discTitle->setVisible(vis);
    m_discMoreBtn->setVisible(vis);
    m_discGrid->setVisible(vis);
    NT_DEBUG("E2E discover setVisible=%d count=%d height=%d\n", vis ? 1 : 0,
             (int)m_discoverVideos.size(), m_discGrid->height());
    m_layoutTimer->start();
}

void HomePage::setMiniPlayerVisible(bool visible)
{
    if (m_miniVisible == visible)
        return;
    m_miniVisible = visible;
    if (!m_bodyLayout)
        return;
    const int extra = visible ? kMiniBarReservedPx : 0;
    NT_DEBUG("E2E miniVisible=%d bottomMargin=%d\n", visible ? 1 : 0,
             20 + extra);
    m_bodyLayout->setContentsMargins(14, 14, 14, 20 + extra);
}

void HomePage::refreshBusy()
{
    // Contenido PERSONALIZADO: el historial cuenta aunque el vídeo no figure
    // en "Continuar viendo" (p. ej. reproducción completa o dejada al inicio,
    // que m_continueVideos no incluye). Un refreshBusy es barato; recientIds(1)
    // es una consulta local, por eso se pregunta directamente a la BD y no se
    // depende de listas ya renderizadas.
    const bool hasHistory = m_history && !m_history->recentIds(1).isEmpty();
    const bool anyUserContent = hasHistory || !m_feed.isEmpty()
        || !m_discoverVideos.isEmpty();
    const bool anyContent = anyUserContent || !m_home.isEmpty()
        || !m_trending.isEmpty();
    const bool loading = m_feedBusy || m_homeBusy || m_trendBusy
        || m_discoverBusy;
    m_busy->setVisible(loading && !anyContent);
    // Empty-state: se muestra cuando NO hay contenido personalizado del
    // usuario (historial, suscripciones, radios Descubre) — el home solo
    // tiene Recomendado y/o Tendencias, que son de red. Cubre el hueco
    // visual que dejaban "Continuar viendo", "Descubre" y "Nuevos de tus
    // suscripciones" ocultos, y guía al usuario.
    if (m_emptyState)
        m_emptyState->setVisible(!anyUserContent);
    NT_DEBUG("E2E refreshBusy: history=%d feed=%d disc=%d cont=%d empty=%d\n",
             hasHistory ? 1 : 0, m_feed.size(), m_discoverVideos.size(),
             m_continueVideos.size(),
             m_emptyState ? (m_emptyState->isVisible() ? 1 : 0) : -1);
}

void HomePage::adjustGridHeight(VideoListView *grid)
{
    const int count = grid->videos().size();
    if (count == 0) {
        // 0 vídeos: el grid se va a ocultar, lo dejamos con altura 0 para
        // que no consuma el hueco enorme en el scroll (sino, queda un
        // vacío visual antes de la siguiente sección).
        grid->setFixedHeight(0);
        return;
    }
    const int w = grid->width();
    const int cols = qMax(1, grid->gridColumns());
    const int rows = (count + cols - 1) / cols;
    // Altura de una celda efectiva: si el grid llena el ancho, la celda escaló
    // (16:9 según el ancho real); si no, la base.
    const int cellH = grid->gridSize().height() > 0
                          ? grid->gridSize().height()
                          : kGridH;
    // En IconMode las filas van exactamente una detrás de otra, sin separaciones
    // (medido: celda 177x195 -> rect 177x195 y paso 195), así que el alto del
    // contenido es rows*cellH clavado. Lo que importa es que ese cellH sea el
    // tamaño REAL de la celda: ver VideoListView::applyGridGeometry.
    const int newH = rows * cellH;
    NT_DEBUG("E2E adjustGridHeight count=%d width=%d cols=%d rows=%d cellH=%d height=%d\n",
             count, w, cols, rows, cellH, newH);
    grid->setFixedHeight(newH);
}

// Columnas efectivas: las que pidió el usuario, recortadas si la ventana es
// demasiado estrecha para que la celda conserve el bloque de texto completo.
// Sin tope, "Pequeñas" (6) en una ventana de 700 px daría celdas de 113 px con
// el título y el canal recortados. La regla vive en
// VideoListView::clampGridColumns (función pura, testeada en --selftest-grid).
//
// El ancho NO se toma del grid de una sección: los grids se ocultan cuando su
// sección está vacía y un widget oculto tiene su viewport estrechado (98 px en
// vez de 760 con la ventana a 1280). Con la base vacía eso forzaba siempre el
// mínimo de 3 columnas y el ajuste "Tamaño de las miniaturas" se ignoraba. Se
// mide el scroll, que está siempre visible y comparte ancho con todos los grids;
// si aún no está visible (arranque), se recurre al ancho de la página menos los
// márgenes de m_bodyLayout.
int HomePage::effectiveCellColumns() const
{
    int vw = 0;
    if (m_scroll && m_scroll->isVisible())
        vw = m_scroll->viewport()->width();
    if (vw <= 0)
        vw = qMax(0, width() - 2 * 14);
    return VideoListView::clampGridColumns(m_cellColumns, vw);
}

void HomePage::setCellColumns(int columns)
{
    if (columns <= 0 || columns == m_cellColumns)
        return;
    m_cellColumns = columns;
    applyGridSizes();
}

void HomePage::applyGridSizes()
{
    NT_DEBUG("E2E applyGridSizes called\n");
    // Primero las columnas efectivas (con el tope por ancho) y el tamaño de
    // celda que de ahí se deriva; después las alturas de cada sección, que usan
    // ese tamaño. Aplicarlo al revés dejaría las alturas de un paso atrás.
    const int cols = effectiveCellColumns();
    for (VideoListView *g : {m_contGrid, m_feedGrid, m_homeGrid,
                             m_discGrid, m_trendGrid})
        if (g)
            g->setGridColumns(cols);
    adjustGridHeight(m_contGrid);
    adjustGridHeight(m_feedGrid);
    adjustGridHeight(m_homeGrid);
    adjustGridHeight(m_discGrid);
    adjustGridHeight(m_trendGrid);
    NT_DEBUG("E2E applyGridSizes done: cols=%d cont=%d feed=%d home=%d disc=%d trend=%d\n",
             cols, m_contGrid->height(), m_feedGrid->height(),
             m_homeGrid->height(), m_discGrid->height(), m_trendGrid->height());
}

void HomePage::scheduleGridSizes()
{
    QTimer::singleShot(0, this, &HomePage::applyGridSizes);
}

void HomePage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    applyGridSizes();
}

