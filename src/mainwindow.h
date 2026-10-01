#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QMainWindow>
#include <QProgressBar>
#include <QSet>
#include <QToolButton>

#include "db/app_database.h"
#include "db/repositories.h"
#include "player_controller.h"
#include "ui/icons.h"

class HomePage;
class ChannelPage;
class SearchPage;
class WatchPage;
class LibraryPage;
class PlaylistPage;
class HistoryPage;
class SettingsPage;
class PlayerWidget;
class ThumbnailCache;
class Yt;
class HwProbe;

class QKeyEvent;
class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
class QFrame;
class QTimer;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void searchFor(const QString &query);
    void playVideo(const QString &url);
    void openChannel(const QString &channelId);
    void openLibrary();
    void openPlaylist(const QString &playlistId, const QString &title);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void openWatch(const QVector<VideoInfo> &videos, int startIndex);
    void openWatchResume(const QVector<VideoInfo> &videos, int startIndex,
                         qlonglong startPositionMs);
    void goHome();
    void goSearch();
    void goLibrary();
    void goSettings();
    void showPreviousTab();
    void openHistory();
    void resetAppData();
    void onNavButton(int index);
    void onRadioReadyDiscover(const QString &videoId,
                              const QVector<VideoInfo> &videos);
    // Recuperación de sesión gateada agotada: reinicia la app con el último
    // vídeo (reiniciar sale de la sesión gateada). Único auto-reinicio de la
    // app, acotado a este error y con guard anti-bucle.
    void onRecoveryExhausted();
    void onRecoveryHealthy();
    // Menú de opciones ("⋯") de una fila de vídeo en las superficies que no
    // tienen los repos (Búsqueda, Canal, Playlist): la ventana sí, así que el
    // menú se construye aquí, una sola vez, con las mismas acciones que
    // Biblioteca, Historial y "A continuación".
    void showVideoRowMenu(const VideoInfo &video, const QPoint &globalPos);

private:
    void setupNav();
    void setupMiniBar();
    void updateMiniPlayer();
    // Persiste volumen y silencio del reproductor en la BD de ajustes
    // (claves "volume" / "volume_muted"), para volver a aplicarlos en el
    // próximo arranque. Lee el estado del controlador; se llama tras cada
    // cambio hecho por el usuario (teclado, mini bar u overlay).
    void persistVolume();
    void applyMiniVideoMode();
    // "Tamaño de las miniaturas" (Ajustes → Interfaz): único punto que aplica
    // el ajuste, tanto al arrancar (con el valor guardado) como en caliente
    // (SettingsPage::thumbSizeChanged). El valor se traduce a DOS efectos,
    // porque las rejillas y las listas no se dimensionan igual: las vistas de
    // lista escalan su fila con un factor, y las rejillas del Home (que son
    // dimensionadas por el ancho con columnas fijas) cambian de número de
    // columnas. Ver SettingsPage::thumbListScale/thumbGridColumns.
    void applyThumbSize(const QString &value);
    void updateMiniBar();
    void updateMiniIcons();
    void setMiniThumb();
    // Recorta el título del mini a una línea con elipsis al ancho real.
    void updateMiniTitleElide();
    // Compone la segunda línea del mini: "Canal · m:ss / m:ss".
    void updateMiniTimeText();
    void goToPlayerPage();
    bool handleShortcut(QKeyEvent *event);
    void openWatchAt(const QVector<VideoInfo> &videos, int startIndex,
                     qlonglong startPositionMs);

    // Perfil de códec de vídeo (av1-hw|vp9-hw|h264-hw|soft-all). Aplica el
    // perfil a Yt + espera de buffer (y tope 1080p en soft-all) y, si persist,
    // lo guarda en settings.
    void applyCodecProfile(const QString &profile, bool persist);
    // Lanza el sondeo offline HwProbe; el cartel bloqueante solo aparece si
    // tarda (PC básico) o si el usuario intenta reproducir mientras sondea.
    void startHwProbe();
    void showHwPopup();
    void centerHwPopup();

    AppDatabase m_db;
    Yt *m_yt = nullptr;
    ThumbnailCache *m_homeThumbs = nullptr;
    ThumbnailCache *m_globalThumbs = nullptr;
    ThumbnailCache *m_libraryThumbs = nullptr;
    PlayerWidget *m_playerWidget = nullptr;

    VideoCacheRepo m_videoCache{m_db};
    DiscoverRepo m_discoverRadios{m_db};
    HistoryRepo m_history{m_db, &m_discoverRadios};
    PlayerController m_player{m_yt, m_playerWidget, &m_history, this};

    FavoritesRepo m_fav{m_db};
    StarredRepo m_starred{m_db};
    WatchLaterRepo m_watchLater{m_db};
    SubscriptionsRepo m_subs{m_db};
    PlaylistsRepo m_playlists{m_db};
    SettingsRepo m_settings{m_db};
    HomeCacheRepo m_homeCache{m_db};

    // Auto-reinicio por sesión gateada: guard en memoria contra dobles
    // reinicios del mismo episodio, y posición (ms) a reanudar en la instancia
    // nueva (leída de settings al arrancar; ver onRecoveryExhausted).
    bool m_gateRestarting = false;
    qlonglong m_gateResumeMs = 0;

    // Semillas de vídeo reproduciéndose para las que ya se pidió su radio (y
    // aún no llegó) desde el hook de reproducción confirmada: evita re-pedir la
    // misma radio al repetirse phase "playing" (reanudar/pausar). La entrada se
    // elimina al guardar la radio o si la respuesta llega vacía (reintento).
    QSet<QString> m_radioSeedsRequested;

    HomePage *m_home = nullptr;
    SearchPage *m_search = nullptr;
    ChannelPage *m_channel = nullptr;
    WatchPage *m_watch = nullptr;
    LibraryPage *m_library = nullptr;
    PlaylistPage *m_playlist = nullptr;
    HistoryPage *m_historyPage = nullptr;
    SettingsPage *m_settingsPage = nullptr;

    QWidget *m_nav = nullptr;
    QList<QToolButton *> m_navButtons;
    QStackedWidget *m_pages = nullptr;

    QWidget *m_miniBar = nullptr;
    // Hueco del mini bar donde se incrusta el PlayerWidget en vivo (reparent),
    // con la miniatura estática como respaldo cuando no hay vídeo.
    QWidget *m_miniVideoHost = nullptr;
    bool m_miniDetached = false;
    QLabel *m_miniThumb = nullptr;
    QLabel *m_miniTitle = nullptr;
    QLabel *m_miniTime = nullptr;
    QString m_miniTitleFull;
    QString m_miniChannel;
    double m_miniPos = 0.0;
    double m_miniDur = 0.0;
    QProgressBar *m_miniProgress = nullptr;
    QPushButton *m_miniPlayBtn = nullptr;
    QPushButton *m_miniMuteBtn = nullptr;
    QPushButton *m_miniCloseBtn = nullptr;

    int m_lastTab = 0;
    int m_currentTab = 0;
    // Sin decodificación por hardware en absoluto (perfil legacy + h264
    // también en software). Dispara el tope 1080p en settings/reproductor y
    // la espera inicial de buffer por % para 1080p.
    bool m_noHw = false;
    QElapsedTimer m_elapsed;

    // Escalera de descenso de perfil por reproducción (evidencia de códec/HW,
    // nunca red): presupuesto propio, separado del de la recuperación de sesión,
    // y anti-rebote para que un mismo episodio no baje varias veces. El descenso
    // es EFÍMERO (no se persiste): un fallo puntual no debe "condenar" la
    // máquina a un perfil menor (Ajustes -> "Volver a detectar" lo restaura).
    int m_descendAttempts = 0;
    qint64 m_lastDescendMs = 0;
    // Perfil ACTIVO en memoria (puede diferir del persistido si la escalera
    // bajó de forma efímera). La escalera compara contra este, no contra
    // settings: si no, tras un descenso en memoria seguiría viendo el perfil
    // viejo y volvería a bajar en falso.
    QString m_activeProfile;

    // Sondeo de hardware de vídeo (HwProbe) y su cartel. Mientras m_hwProbing
    // está activo no se reproduce: la petición se encola y se ejecuta al acabar.
    HwProbe *m_hwProbe = nullptr;
    bool m_hwProbing = false;
    QFrame *m_hwPopup = nullptr;
    QTimer *m_hwPopupTimer = nullptr;
    bool m_hasPendingWatch = false;
    QVector<VideoInfo> m_pendingWatch;
    int m_pendingIndex = 0;
    qlonglong m_pendingPos = 0;
};
