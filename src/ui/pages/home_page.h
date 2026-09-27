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
//   5. 🔥 Tendencias           : ≤8, trending por Innertube (TTL 2 h)
// Caché en memoria con TTL: al entrar Home se muestra lo cached al momento y si
// caducó se refresca UNA vez en segundo plano (single-flight, sin polling). Si
// la actualización falla o devuelve vacío se CONSERVA la sección cacheada
// anterior (memoria y UI; no se oculta ni se reemplaza por vacío ni se refresca
// fetched_at). El feed de suscripciones y las tendencias (Innertube) nunca
// desaparece: si esas secciones se quedan vacías tras fallar todo, se mantiene
// lo persistido en home_cache. Lo persistido en BD se reemplaza solo cuando
// llega una respuesta de red VÁLIDA (nunca por un fallo).
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
    // Vídeos por fila de las rejillas del Home (ajuste "Tamaño de las
    // miniaturas": 4/5/6). Es la única palanca de tamaño de una rejilla, que
    // es dimensionada por el ancho con un número de columnas fijo; el ancho
    // manda un tope para que la celda no baje de kMinCellW (si no, con 6
    // columnas en una ventana estrecha el título de 2 líneas se recorta).
    void setCellColumns(int columns);
    int cellColumns() const { return m_cellColumns; }
    // Re-aplica el botón del empty state con el acento vigente (MainWindow lo
    // llama al cambiar el color de acento en Ajustes, sin reconstruir).
    void applyAccent();

signals:
    void openRequested(const QVector<VideoInfo> &videos, int startIndex);
    void resumeRequested(const QVector<VideoInfo> &videos, int startIndex,
                         qlonglong startPositionMs);
    // Empty-state del home: el usuario pidió ir a Buscar.
    void navigateToSearchRequested();

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
    // Firma del conjunto de suscripciones (ids ordenados). Se persiste tras
    // construir el feed; si al entrar a Home difiere, el feed cacheado quedó
    // obsoleto (se suscribió/dejó de seguir) y se fuerza su reconstrucción
    // aunque su TTL siga vigente.
    QString currentSubsSignature() const;
    void maybeInvalidateFeed();
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
    // Construye e inserta en el bodyLayout el panel "Tu inicio aún está
    // vacío". Se muestra hasta que cualquier sección tenga contenido (lo
    // controla refreshBusy). Construido una sola vez (en el ctor).
    void buildEmptyState();
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
    // Columnas efectivas tras aplicar el tope por ancho (kMinCellW).
    int effectiveCellColumns() const;

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
    // Panel "Aún no hay nada para mostrar": se muestra cuando TODAS las
    // secciones están vacías (sin historial, sin suscripciones, sin radios
    // Descubre, sin caché previa). Cierra el hueco gigante que dejaban las
    // secciones hide() y le dice al usuario qué hacer para empezar.
    QWidget *m_emptyState = nullptr;
    // Botón "Ir a Buscar" del empty state: su fondo usa el acento, así que se
    // guarda el puntero para re-aplicarlo al cambiar el color de acento.
    QPushButton *m_goSearchBtn = nullptr;

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
    // Vídeos por fila elegidos por el usuario (4/5/6). applyGridSizes() les
    // aplica el tope por ancho antes de fijar el tamaño de las rejillas.
    int m_cellColumns = 5;

    QVector<VideoInfo> m_continueVideos;
    QVector<qlonglong> m_continuePositions; // alineado con m_continueVideos
    QVector<VideoInfo> m_feed;
    // Copia del feed mostrado tomada al iniciar una actualización (loadFeed).
    // Si la actualización acaba vacía/falla, onFeedDone la restaura en vez de
    // ocultar la sección.
    QVector<VideoInfo> m_prevFeed;
    qint64 m_feedFetchedAt = 0;
    QString m_feedSubsSig;
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
    QString m_currentVideoId;
    bool m_miniVisible = false;
    QVBoxLayout *m_bodyLayout = nullptr;
    QTimer *m_layoutTimer = nullptr;
};