#pragma once

#include <QWidget>
#include <QVector>

#include "../../models.h"
#include "../icons.h"

class VideoCacheRepo;
class FavoritesRepo;
class WatchLaterRepo;
class PlaylistsRepo;
class StarredRepo;
class SubscriptionsRepo;
class SettingsRepo;
class PlayerController;
class PlayerWidget;
class ThumbnailCache;
class Yt;

class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTimer;
class QEvent;
class SeekSlider;
class QSlider;
class VideoListView;
class PlayerContainer;

class WatchPage : public QWidget
{
    Q_OBJECT

public:
    explicit WatchPage(Yt *yt, PlayerController *player,
                       PlayerWidget *playerWidget,
                       VideoCacheRepo *cache, FavoritesRepo *favorites,
                       StarredRepo *starred, WatchLaterRepo *watchLater,
                       SubscriptionsRepo *subs, SettingsRepo *settings,
                       ThumbnailCache *thumbs, PlaylistsRepo *playlists,
                       QWidget *parent = nullptr);

    void setWatchData(const QVector<VideoInfo> &queue, int startIndex);
    // Genera la cola del mix RDAMVM para el vídeo dado (click explícito desde
    // fuera de "A continuación": search, home, channel). No se auto-genera.
    void triggerRadio(const QString &videoId);

    // Mini player con vídeo en vivo: mueve el PlayerWidget (y su overlay) al
    // widget `host` del mini bar manteniéndolo visible/renderizando, para que
    // mpv no acumule frames ni se reconfigure el decodificador. attachPlayer()
    // lo devuelve al área de vídeo de la página. Reparentar dentro de la misma
    // ventana conserva el contexto GL y el mpv_render_context.
    void detachPlayerTo(QWidget *host);
    void attachPlayer();
    bool playerDetached() const { return m_playerDetached; }

signals:
    void backRequested();
    void channelRequested(const QString &channelId, const QString &name);
    void fullscreenToggled(bool on);

private slots:
    void onCurrentChanged();
    void onQueueChanged();
    void onPhaseChanged(const QString &phase);
    void onPositionChanged(double position, double duration);
    // El volumen/silencio cambió por cualquier vía (teclado, slider, botón de
    // silencio, ajuste): sincroniza el slider del overlay en caliente.
    void onVolumeChanged();
    void onDetailReady(const VideoInfo &video, const QString &desc,
                       qint64 likes, const QVector<FormatInfo> &formats);
    void onDetailFailed(const QString &videoId);
    void onChannelInfo(const ChannelInfo &channel);
    void onAvatarReady(const QString &key);
    void onQualityChanged(int index);
    void onVideoSizeChanged(int width, int height);
    void onPlayPause();
    void onToggleMute();
    void onNextUpActivated(int index, const VideoInfo &video);
    void onRelatedReady(const QString &videoId, const QVector<VideoInfo> &videos);
    void onRadioReady(const QString &videoId, const QVector<VideoInfo> &videos);
    void onCurrentDetailReady(const QString &videoId, const QString &title,
                              const QString &description);
    // El backend informó de la duración real del medio: se guarda en la caché
    // de vídeos (la que lee "Continuar viendo"), rellenando el hueco de los
    // vídeos que llegaron sin duración desde search/home/canal.
    void onDurationKnown(double seconds);

public slots:
    void toggleFullscreen();
    // Reaplica el rango/valor del slider del overlay desde el controlador
    // (p.ej. al cambiar el ajuste "Volumen máximo" en Ajustes).
    void refreshVolumeSlider();
    // Re-aplica los estilos/los iconos que dependen del color de acento
    // (botón Suscribirse, acciones activas, enlaces de la descripción, icono
    // de subs del overlay). Se llama desde MainWindow cuando cambia el acento
    // en Ajustes, para que todo siga el color sin reconstruir la página.
    void applyAccent();

private:
    void updateQueue();
    void updateInfo();
    void updateChannelRow();
    void updateSubscribeButton(bool subscribed);
    void updateSubscription(const VideoInfo &v, bool subscribe);
    void updateActionButtons();
    void enterFullscreen();
    void leaveFullscreen();
    int qualityIndexForHeight(int height) const;
    int idxToQuality(int idx) const;
    void setQualityIndex(int index);
    void toggleQualityMenu();
    // Conectada a PlayerWidget::qualityLimitsChanged: oculta los items del
    // menú de resoluciones por encima del techo real del archivo. Ignora
    // valores <= 0 (carga incompleta) para no romper el menú.
    void applyQualityLimits(int maxVideoHeight);
    // Conectada a Yt::streamHeightsReady: guarda la altura máxima que ofrece
    // el vídeo (adaptiveFormats) para que Auto no oculte calidades reales que
    // el stream cargado no trae (p.ej. Auto pide 360 de un vídeo 1080).
    void onStreamHeights(const QString &videoId, const QVector<int> &heights);
    // Pinta la descripción del vídeo como HTML enriquecido (enlaces + saltos
    // de línea) y guarda el texto plano. Vacía = oculta la descripción.
    void applyDescription(const QString &description);
    // True si el vídeo dado es el que está en reproducción (ignora respuestas
    // que llegan tarde, de un vídeo ya reemplazado).
    bool isCurrentVideo(const QString &videoId) const;
    void showPlaylistMenu();
    // Popup estilado "Nueva playlist" (mismo popupDialog que Biblioteca):
    // tras crearla, si hay vídeo pendiente se añade a la playlist nueva.
    void openCreatePlaylistPopup();
    void closeCreatePlaylistPopup();
    void submitCreatePlaylist();
    void showOverlayControls();
    void hideOverlayControls();
    void updateOverlayIcons();
    bool isVideoClick(const QObject *watched) const;

    bool eventFilter(QObject *watched, QEvent *event) override;

    Yt *m_yt;
    PlayerController *m_player;
    PlayerWidget *m_playerWidget = nullptr;
    PlayerContainer *m_playerContainer = nullptr;
    bool m_playerDetached = false;
    VideoCacheRepo *m_cache;
    FavoritesRepo *m_favs;
    StarredRepo *m_starred;
    WatchLaterRepo *m_watchLater;
    SubscriptionsRepo *m_subs;
    SettingsRepo *m_settings;
    ThumbnailCache *m_thumbs;
    PlaylistsRepo *m_playlists;
    bool m_liked = false;
    bool m_disliked = false;
    bool m_isStarred = false;
    bool m_watchLaterMarked = false;

    QPushButton *m_back;
    QStackedWidget *m_overlay;
    QLabel *m_errorLabel;
    // Scroll ÚNICO de toda la página: vídeo arriba + fila con descripción y
    // "A continuación" (derecha, sin límite vertical), movidos por el mismo
    // scroll (estilo YouTube).
    QScrollArea *m_topScroll = nullptr;
    QWidget *m_bottomWidget = nullptr;
    // Bloque de descripción (título/meta/canal/acciones/descripción) dentro
    // del scroll. Se oculta en fullscreen para dejar solo el vídeo.
    QWidget *m_watchBody = nullptr;
    bool m_inFullscreen = false;
    QLabel *m_title;
    QLabel *m_meta;
    QPushButton *m_channelBtn;
    QString m_channelAvatarKey;
    QString m_channelAvatarUrl;
    QPushButton *m_subscribeBtn;
    QPushButton *m_likeBtn;
    QPushButton *m_dislikeBtn;
    QPushButton *m_shareBtn;
    QPushButton *m_starBtn = nullptr;
    QPushButton *m_watchLaterBtn = nullptr;
    QPushButton *m_playlistBtn = nullptr;
    nt::Icon m_likeIcon = nt::Icon::Thumb;
    nt::Icon m_dislikeIcon = nt::Icon::ThumbDown;
    nt::Icon m_shareIcon = nt::Icon::Share;
    nt::Icon m_starIcon = nt::Icon::Star;
    nt::Icon m_starFilledIcon = nt::Icon::StarFilled;
    nt::Icon m_watchLaterIcon = nt::Icon::WatchLater;
    nt::Icon m_playlistIcon = nt::Icon::ListAdd;
    QFrame *m_plPopup = nullptr;
    QLabel *m_plPopupTitle = nullptr;
    QLineEdit *m_plName = nullptr;
    QPushButton *m_plOk = nullptr;
    QString m_plPendingVideoId;
    QPushButton *m_quality;
    QFrame *m_qualityMenu;
    QWidget *m_qualityMenuAnchor;
    // Items del menú de resoluciones, alineados con kQualities[]/{360,480,
    // 720,1080,1440,2160}. Se ocultan individualmente (no se quitan) cuando
    // el archivo cargado no tiene esa pista: ver applyQualityLimits.
    QVector<QPushButton *> m_qualityItems;
    // Altura REAL del vídeo en reproducción (mpv video-params/h). Permite
    // mostrar honestamente la altura en el botón para clips sub-360p (donde
    // kQualities[] no tiene entrada), sin engañar con la 360p más cercana.
    int m_videoHeight = 0;
    int m_qualityIndex = 2;
    bool m_qualityMenuActive = false;
    bool m_autoQuality = false;
    int m_autoHeight = 360;
    // Altura máxima que ofrece el vídeo según Innertube (Yt::streamHeightsReady).
    // El menú ofrece todas las calidades reales aunque Auto haya pedido una
    // baja. 0 = indeterminado (cae al techo del stream cargado).
    int m_availableMaxH = 0;
    QLabel *m_descLabel;
    VideoListView *m_nextUpList;

    // Controles flotantes sobre el video (overlay).
    QWidget *m_ctrlBar = nullptr;
    QPushButton *m_ovPlayBtn = nullptr;
    QPushButton *m_ovNextBtn = nullptr;
    QPushButton *m_ovPrevBtn = nullptr;
    SeekSlider *m_ovSeek = nullptr;
    QLabel *m_ovTime = nullptr;
    QPushButton *m_ovMuteBtn = nullptr;
    QSlider *m_ovVolume = nullptr;
    QPushButton *m_ovSubsBtn = nullptr;
    QPushButton *m_ovFsBtn = nullptr;
    QTimer *m_overlayTimer = nullptr;
    QPoint m_lastMousePos = QPoint(-100000, -100000);
    double m_volume = 100.0;
    bool m_subsVisible = false;
    qint64 m_lastVideoPressMs = -100000;
    QPoint m_lastVideoPressPos = QPoint(-100000, -100000);

    QVector<VideoInfo> m_queue;
    int m_queueStart = -1;
    QVector<VideoInfo> m_nextUp;
    QString m_description;
    qint64 m_likeCount = 0;
    bool m_fetchingDetail = false;
    // Siembra de "A continuación": id del vídeo cuyo /next ya se pidió (un
    // solo pedido por vídeo; los nuevos vídeos que llegan de los relacionados
    // disparan su propio /next cuando la cola vuelve a acercarse al final).
    QString m_relatedVideoId;
};