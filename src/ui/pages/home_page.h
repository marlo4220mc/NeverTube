#pragma once

#include <QHash>
#include <QScrollArea>
#include <QSet>
#include <QWidget>
#include <QVector>

#include "../../models.h"

class QLabel;
class QPushButton;
class QTimer;
class QTimer;
class QVBoxLayout;
class DiscoverRepo;
class HistoryRepo;
class HomeCacheRepo;
class SettingsRepo;
class SubscriptionsRepo;
class ThumbnailCache;
class VideoCacheRepo;
class VideoListView;
class Yt;

// Home de NeverTube. Secciones (orden fijo; las vacías se ocultan):
//   1. ▶ Continuar viendo      : historial local (≤4, progreso en miniatura)
//   2. 🔔 Nuevos suscripciones : ≤5 recientes por cada canal suscrito (TTL 30 min)
//   3. ✨ Recomendado YouTube   : browse FEwhat_to_watch VISIONOS (≤3×8, TTL 60 min)
//   4. 🎲 Descubre             : ≤5 vídeos de radios GUARDADAS (1-2 por radio,
//                                fuente persistente SQLite, TTL 60 min)
//   5. 🔥 Tendencias           : ≤8, reutiliza el trending yt-dlp (TTL 2 h)
// Caché en memoria con TTL: al entrar Home se muestra lo cached al momento y si
// caducó se refresca UNA vez en segundo plano (single-flight, sin polling). En
// fallo/500/vacío se oculta la sección y el resto (el fallback yt-dlp: feed de
// suscripciones + tendencias) nunca desaparece.
class HomePage : public QWidget
{
    Q_OBJECT

public:
    explicit HomePage(Yt *yt, SubscriptionsRepo *subs, HistoryRepo *history,
                      DiscoverRepo *discover, VideoCacheRepo *videoCache,
                      ThumbnailCache *thumbs, SettingsRepo *settings,
                      HomeCacheRepo *homeCache, QWidget *parent = nullptr);

    void reload();
    void refreshContinueWatching();
    void refreshDynamic();
    void setYtAvailable(bool ok);
    // Id del vídeo en reproducción: el Descubre no lo muestra (para no
    // repetir en Home el vídeo que se está viendo en la página de reproducción).
    void setCurrentVideoId(const QString &id);
    // Margen inferior adicional del contenido desplazable mientras el mini
    // player está visible, para poder llevar la última sección por encima de
    // él y verla completa. Se elimina al ocultarlo.
    void setMiniPlayerVisible(bool visible);
    // Aborta la generación de radios de Descubre en curso (solo ocurre como
    // excepción de inicialización, cuando no hay radios guardadas). Se llama al
    // abrir un vídeo para no competir con la radio de "A continuación" del
    // reproductor (Innertube.fetchRadio es single-flight).
    void interruptDiscover();
    // Invalida la caché de Descubre (la re-selecciona o la oculta si ya no hay
    // radios). Se llama cuando cambia el historial (p. ej. al borrar vídeos):
    // Descubre no debe seguir mostrando selecciones de radios eliminadas.
    void invalidateDiscover();

signals:
    void openRequested(const QVector<VideoInfo> &videos, int startIndex);
    void resumeRequested(const QVector<VideoInfo> &videos, int startIndex,
                         qlonglong startPositionMs);

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onTrending(const QVector<VideoInfo> &videos);
    void onChannelVideos(const QVector<VideoInfo> &videos);
    void onFeedDone();
    void onHomeReady(const QVector<VideoInfo> &videos);
    void onRadioReady(const QString &videoId, const QVector<VideoInfo> &videos);
    void onDiscoveryAgain();

private:
    bool cachedFresh(qint64 at, qint64 ttlMs) const;
    void loadContinueWatching();
    void loadSections();
    void loadFeed();
    void requestNextChannel();
    // Carga del home_cache (HomeCacheRepo) las secciones de RED persistidas en
    // el último arranque: Home muestra contenido al instante y cada sección se
    // refresca sola cuando caduca su TTL (lo persiste todo el Home menos lo que
    // ya es local: Continuar viendo deriva del historial y Descubre de las
    // radios guardadas).
    void loadPersistedSections();
    // Guarda (reemplaza, no agrega) una sección de red en el home_cache tras
    // recibir datos frescos. Los ids se deducen de la lista en orden mostrado.
    void saveSection(const QString &section, const QVector<VideoInfo> &videos);
    void ensureDiscover();
    // Construye la selección de Descubre SOLO desde radios guardadas (sin red)
    // y la muestra. Si no hay radios útiles, dispara la generación de
    // inicialización (única excepción que toca Yt::radioVideos).
    void buildDiscoverSelection();
    void startDiscoverSeedGen();
    void fetchNextDiscoverSeed();
    void finishDiscoverSeedGen();
    void buildDiscoverSeeds();
    void showDiscover();
    void refreshBusy();
    void adjustGridHeight(VideoListView *grid);
    // Recalcula la altura de los grids de sección (Continuar/feed/Recomendado/
    // Tendencias). La altura se deriva del ancho real del grid, de modo que las
    // secciones se ajustan al contenido y no dejan vacío vertical entre un
    // apartado y los siguientes.
    void applyGridSizes();
    void scheduleGridSizes();

    Yt *m_yt;
    SubscriptionsRepo *m_subs;
    HistoryRepo *m_history;
    VideoCacheRepo *m_videoCache;
    DiscoverRepo *m_discover;
    ThumbnailCache *m_thumbs;
    SettingsRepo *m_settings;
    HomeCacheRepo *m_homeCache;

    // Auto-siembra de radios de inicialización: solo la primera vez. Si el
    // sistema ya generó alguna radio alguna vez (discover_seeded), un Descubre
    // vacío (p. ej. tras borrar todo el historial) NO se rellena solo: las
    // radios solo vuelven a generarse al reproducir vídeos.
    bool discoverSeeded() const;
    void markDiscoverSeeded();

    QScrollArea *m_scroll;
    QWidget *m_body;
    QLabel *m_busy;
    QLabel *m_banner;

    QLabel *m_contTitle;
    VideoListView *m_contGrid;
    QLabel *m_feedTitle;
    VideoListView *m_feedGrid;
    QLabel *m_homeTitle;
    VideoListView *m_homeGrid;
    QLabel *m_discTitle;
    QPushButton *m_discMoreBtn;
    VideoListView *m_discGrid;
    QLabel *m_trendTitle;
    VideoListView *m_trendGrid;

    QVector<VideoInfo> m_continueVideos;
    QVector<qlonglong> m_continuePositions; // alineado con m_continueVideos
    QVector<VideoInfo> m_feed;
    qint64 m_feedFetchedAt = 0;
    QVector<VideoInfo> m_home;
    qint64 m_homeFetchedAt = 0;
    // 4) 🎲 Descubre. Fuente PERSISTENTE (SQLite): las radios se generan al
    // reproducir vídeos (y se guardan) o por la excepción de inicialización si
    // no hay radios. Abrir Home NUNCA pide radios por sí solo: si la caché en
    // memoria caducó se re-seleccionan vídeos de las radios guardadas
    // (pickDiscoverVideos, sin red). "↻ Otro" repica la selección con las
    // radios ya guardadas (no regenera).
    QVector<VideoInfo> m_discoverVideos;
    qint64 m_discoverFetchedAt = 0;
    qint64 m_discoverEmptyAt = 0;
    bool m_discoverBusy = false;
    QStringList m_discoverSeeds;
    QSet<QString> m_discoverTried;
    QString m_discoverSeedId;
    QVector<VideoInfo> m_trending;
    qint64 m_trendFetchedAt = 0;

    QStringList m_pendingChannels;
    int m_pendingIdx = 0;
    QHash<QString, QString> m_channelNames;
    bool m_feedBusy = false;
    bool m_homeBusy = false;
    bool m_trendBusy = false;
    bool m_ytAvailable = true;
    QString m_currentVideoId;
    bool m_miniVisible = false;
    QVBoxLayout *m_bodyLayout = nullptr;
    QTimer *m_layoutTimer = nullptr;
};