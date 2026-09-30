#include "watch_page.h"

#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QEvent>
#include <QFontMetrics>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include <functional>

#include "../../db/repositories.h"
#include "../../debug.h"
#include "../../models.h"
#include "../../player_controller.h"
#include "../../playerwidget.h"
#include "../../yt.h"
#include "../format.h"
#include "../icons.h"
#include "../flow_layout.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

// Líneas visibles de la descripción cuando está colapsada (estilo YouTube).
static const int kDescLines = 3;

// Resoluciones del menú del reproductor, de menor a mayor. Fuente ÚNICA de
// verdad: los índices del menú (m_qualityItems), idxToQuality,
// qualityIndexForHeight y la altura pedida a Yt salen de aquí. Debe coincidir
// con las opciones de calidad de Ajustes (settings_page.cpp).
namespace {
const int kQualityHeights[] = {144, 240, 360, 480, 720, 1080, 1440, 2160};
constexpr int kQualityCount =
    static_cast<int>(sizeof(kQualityHeights) / sizeof(kQualityHeights[0]));
// Altura máxima del área de vídeo (PlayerContainer). En la página con scroll
// único, un vídeo sin tope ocuparía width/aspect (768+ px en ventanas grandes
// y maximizadas) y descripción + "A continuación" quedarían empujadas fuera de
// la vista. El tope muerde cuando el cap por aspecto supera los 560 px: en
// ventanas pequeñas manda width/aspect y no cambia nada. Layout puro: no toca
// render ni el redibujado.
constexpr int kMaxPlayerHeight = 560;

// Ancho de vídeo por debajo del cual la fila completa de controles no cabe y se
// esconde el slider de volumen. La suma sale del código, no es a ojo:
//   6 botones iconBtn (icono + 12 de padding)  188
//   chip de calidad por contenido                ~60
//   slider de volumen                            80
//   reloj "12:04 / 32:11" a 12 px               ~85
//   8 huecos de btnRow x 6                       48
//   márgenes laterales de barLay                 24
//                                              -----
//                                               ~485
// 500 deja ~15 px de holgura y es deliberadamente conservador: en la banda
// 485-500 el volumen se esconde aunque habría cabido. Preferimos esconderlo de
// más que recortar, porque los setFixedWidth no ceden y el recorte caería en los
// botones y el reloj. El mute sigue estando, así que no se pierde la capacidad
// de silenciar.
constexpr int kOverlayFullRowMinW = 500;

// Área de vídeo dentro del overlay. gl1 (el layout que lleva la caja) iba con
// contentsMargins 0, así que la caja ocupaba el ancho entero sin separador. Esta
// clase no pinta nada: solo avisa de ese ancho, que es lo que decide si la fila
// cabe. Sin Q_OBJECT a propósito para no meter una clase con señales en el
// namespace anónimo: un std::function que se asigna al vuelo.
class OverlayHost : public QWidget
{
public:
    explicit OverlayHost(QWidget *parent = nullptr) : QWidget(parent) {}
    std::function<void(int)> onWidth;

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        if (onWidth)
            onWidth(width());
    }
};
} // namespace

class PlayerContainer : public QWidget
{
public:
    PlayerContainer(PlayerWidget *player, QWidget *overlay, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_overlay(overlay)
        , m_player(player)
    {
        // El fondo va por QSS global (QWidget#playerArea): negro puro, no kBg,
        // porque es el fondo de las bandas letterbox y tiene que ser negro de
        // verdad para que no se note el marco. Antes era un setStyleSheet inline.
        player->setParent(this);
        overlay->setParent(player);
        overlay->setGeometry(player->rect());
        overlay->raise();
        overlay->show();

        m_lay = new QVBoxLayout(this);
        m_lay->setContentsMargins(0, 0, 0, 0);
        m_lay->setSpacing(0);
        m_lay->addWidget(player);

        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumHeight(0);
        updateMaxHeight();
    }

    // Devuelve el PlayerWidget a esta área tras el mini player (lo reparenta y
    // lo reintegra en el layout; el contexto GL se conserva por ser la misma
    // ventana). El overlay permanece hijo del player.
    void givePlayerBack()
    {
        m_player->setParent(this);
        if (m_lay->indexOf(m_player) < 0)
            m_lay->addWidget(m_player);
        m_player->show();
        m_overlay->setGeometry(m_player->rect());
        m_overlay->raise();
        updateMaxHeight();
    }

    QSize sizeHint() const override
    {
        if (m_aspect > 0.0)
            return QSize(320, qRound(320.0 / m_aspect));
        return QSize(320, 180);
    }

    void setVideoAspect(double aspect)
    {
        if (aspect <= 0.0)
            return;
        m_aspect = aspect;
        updateMaxHeight();
    }

    // Pantalla completa de solo-vídeo: el tope absoluto de altura NO aplica (o
    // el vídeo se quedaría en 560 px con la pantalla llena de negro); manda solo
    // el cap por aspecto, que era el comportamiento original.
    void setFullscreen(bool fullscreen)
    {
        if (m_fullscreen == fullscreen)
            return;
        m_fullscreen = fullscreen;
        updateMaxHeight();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        m_overlay->setGeometry(m_player->rect());
        m_overlay->raise();
        updateMaxHeight();
        NT_DEBUG("E2E pc resize %dx%d\n", width(), height());
    }

private:
    void updateMaxHeight()
    {
        if (m_aspect <= 0.0)
            return;
        // El vídeo mantiene su proporción y además nunca excede el tope
        // absoluto: en pantallas grandes/maximizadas el stretch (3:1) haría
        // crecer el área 3/4 de la ventana y descripción + "A continuación"
        // se quedarían sin espacio. En ventanas pequeñas el cap por aspecto
        // (width/aspect) es el que manda y este tope no cambia nada.
        // En PANTALLA COMPLETA no aplica el tope: el vídeo debe llenar la
        // pantalla (solo lo limita su aspecto).
        const int h = qRound(width() / m_aspect);
        const int capped = m_fullscreen ? h : qMin(h, kMaxPlayerHeight);
        NT_DEBUG("E2E pc aspect=%.3f width=%d maxH=%d curMin=%d curMax=%d\n",
                 m_aspect, width(), capped, minimumHeight(), maximumHeight());
        if (capped == minimumHeight() && capped == maximumHeight())
            return;
        // min = max = cap: dentro del scroll único de la página el layout debe
        // darle al vídeo EXACTAMENTE su altura (aspecto o 560 px), ni estirado
        // por políticas Expanding ni encogido por el contenido scrolleable.
        setMinimumHeight(capped);
        setMaximumHeight(capped);
    }

    QWidget *m_overlay;
    PlayerWidget *m_player;
    QVBoxLayout *m_lay = nullptr;
    double m_aspect = 0.0;
    // Pantalla completa (solo vídeo): desactiva el tope kMaxPlayerHeight.
    bool m_fullscreen = false;
};

// Deslizador de búsqueda: click en cualquier punto del track busca ahí y el
// arrastre no pelea con las actualizaciones de posición mientras se mantiene
// pulsado. Emite el valor buscado (gatillo de seek) por cada cambio.
class SeekSlider : public QSlider
{
public:
    explicit SeekSlider(Qt::Orientation orientation, QWidget *parent = nullptr)
        : QSlider(orientation, parent)
    {
        setTracking(true);
    }

    bool isSeeking() const { return m_dragging; }

protected:
    static int valueFromPos(const QSlider *slider, int x)
    {
        const int w = qMax(1, slider->width());
        const qreal r = qBound(qreal(0.0), qreal(x) / qreal(w), qreal(1.0));
        return slider->minimum()
               + qRound(qreal(slider->maximum() - slider->minimum()) * r);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_dragging = true;
            const int v = valueFromPos(this, event->pos().x());
            setValue(v);
            emit sliderMoved(v);
            event->accept();
            return;
        }
        QSlider::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragging) {
            const int v = valueFromPos(this, event->pos().x());
            setValue(v);
            emit sliderMoved(v);
            event->accept();
            return;
        }
        QSlider::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_dragging) {
            m_dragging = false;
            const int v = valueFromPos(this, event->pos().x());
            setValue(v);
            emit sliderMoved(v);
            event->accept();
            return;
        }
        QSlider::mouseReleaseEvent(event);
    }

private:
    bool m_dragging = false;
};

namespace {

// Avatar recortado en círculo para la fila del canal del reproductor.
QPixmap circularAvatar(const QPixmap &src, int size)
{
    QPixmap out(size, size);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addEllipse(0, 0, size, size);
    p.setClipPath(path);
    p.drawPixmap(0, 0, size, size,
                 src.scaled(size, size, Qt::KeepAspectRatioByExpanding,
                            Qt::SmoothTransformation));
    return out;
}

// Icono del canal con gap px de separación a la derecha del círculo, para
// que el nombre no quede pegado al avatar.
QIcon avatarWithGap(const QPixmap &circular, int size, int gap)
{
    QPixmap canvas(size + gap, size);
    canvas.fill(Qt::transparent);
    QPainter p(&canvas);
    p.drawPixmap(0, 0, size, size, circular);
    return QIcon(canvas);
}

// Activa tracking del ratón + event filter en un widget y todos sus hijos,
// para que WatchPage pueda mostrar/ocultar los controles flotantes.
void enableOverlayTracking(QObject *filterObj, QWidget *root)
{
    if (!root)
        return;
    root->setMouseTracking(true);
    root->installEventFilter(filterObj);
    const auto children = root->findChildren<QWidget *>();
    for (QWidget *w : children) {
        w->setMouseTracking(true);
        w->installEventFilter(filterObj);
    }
}

} // namespace

WatchPage::WatchPage(Yt *yt, PlayerController *player, PlayerWidget *playerWidget,
                     VideoCacheRepo *cache, FavoritesRepo *favorites,
                     StarredRepo *starred, WatchLaterRepo *watchLater,
                     SubscriptionsRepo *subs, SettingsRepo *settings,
                     ThumbnailCache *thumbs, PlaylistsRepo *playlists,
                     QWidget *parent)
    : QWidget(parent)
    , m_yt(yt)
    , m_player(player)
    , m_playerWidget(playerWidget)
    , m_cache(cache)
    , m_favs(favorites)
    , m_starred(starred)
    , m_watchLater(watchLater)
    , m_subs(subs)
    , m_settings(settings)
    , m_thumbs(thumbs)
    , m_playlists(playlists)
{
    // Modo Auto: leer preferencia de calidad guardada (default auto a 360p)
    const QString savedQ = m_settings
                               ? m_settings->getString(QStringLiteral("quality"),
                                                       QStringLiteral("auto"))
                               : QStringLiteral("auto");
    if (savedQ == QStringLiteral("auto")) {
        m_autoQuality = true;
        m_autoHeight = m_settings
                           ? m_settings->getInt(QStringLiteral("quality_auto"), 360)
                           : 360;
    } else {
        m_autoQuality = false;
        int h = savedQ.toInt();
        m_autoHeight = (h > 0) ? h : 360;
    }
    // El botón/índice del menú arrancan en la calidad realmente pedida (Auto =
    // última memorizada), no en un 720p por defecto que no refleja nada.
    m_qualityIndex = qualityIndexForHeight(m_autoHeight);

    auto *splitOuter = new QVBoxLayout(this);
    splitOuter->setContentsMargins(0, 0, 0, 0);
    splitOuter->setSpacing(0);

    m_overlay = new QStackedWidget(this);
    m_overlay->setAttribute(Qt::WA_TranslucentBackground, true);
    // Los dos "background: transparent" de aqui (el stack y OverlayHost) NO son
    // inlines de tema pendientes de migrar: son estructurales. El stack tiene que
    // dejar ver el video que hay detras, y OverlayHost es un QWidget plano que
    // solo existe para colocar la caja de controles encima del video. Moverlos
    // al QSS global no cambia ni un pixel (ya son "sin color"), y si el selector
    // fallara el stack volveria a pintar su fondo de paleta encima del video.
    m_overlay->setStyleSheet(QStringLiteral("background: transparent;"));

    auto *gl = new OverlayHost;
    gl->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *gl1 = new QVBoxLayout(gl);
    // Separador de 12 px: antes 0,0,0,0. m_ctrlBar no recibe geometría (solo la
    // gl1 lo coloca), así que la caja tocaba los tres bordes del vídeo y su
    // border-radius y su borde no delimitaban nada: parecían adornos de una
    // banda pegada al canto. Con separador, la caja flota y su forma dice lo que
    // dice. Arriba 0 a propósito: la caja se apoya en el borde inferior.
    gl1->setContentsMargins(12, 0, 12, 12);
    gl1->setSpacing(0);
    gl1->addStretch(1);

    m_ctrlBar = new QWidget(gl);
    m_ctrlBar->setObjectName(QStringLiteral("playerOverlay"));
    m_ctrlBar->setAttribute(Qt::WA_StyledBackground, true);
    // El estilo (fondo translúcido, borde de dos tonos, radio 12) vive en el
    // QSS global: antes era un setStyleSheet inline con #2A2A2A, que además
    // contradecía kBorder (#262626), y por eso esta superficie era la única que
    // no cubría un cambio de acento.
    auto *barLay = new QVBoxLayout(m_ctrlBar);
    // Antes (10, 4, 10, 6) con spacing 2: 4 px de aire encima de la pista y 2
    // entre filas, frente a 10 px a los lados. Al flotar la caja ese desequilibrio
    // se nota. 8/8/6 es lo que usa el resto de la app. La caja crece ~10 px.
    barLay->setContentsMargins(12, 8, 12, 8);
    barLay->setSpacing(6);

    auto *seekRow = new QHBoxLayout;
    seekRow->setSpacing(8);
    m_ovSeek = new SeekSlider(Qt::Horizontal, m_ctrlBar);
    m_ovSeek->setObjectName(QStringLiteral("ovSeek"));
    m_ovSeek->setCursor(Qt::PointingHandCursor);
    connect(m_ovSeek, &QSlider::sliderMoved, this,
            [this](int value) { m_player->seek(value); });
    seekRow->addWidget(m_ovSeek, 1);
    m_ovTime = new QLabel(QStringLiteral("0:00 / 0:00"), m_ctrlBar);
    m_ovTime->setObjectName(QStringLiteral("ovTime"));
    seekRow->addWidget(m_ovTime);
    barLay->addLayout(seekRow);

    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);

    m_ovPrevBtn = new QPushButton(m_ctrlBar);
    m_ovPrevBtn->setObjectName(QStringLiteral("iconBtn"));
    m_ovPrevBtn->setCursor(Qt::PointingHandCursor);
    m_ovPrevBtn->setIcon(nt::icon(nt::Icon::Prev, nt::kText, 20));
    connect(m_ovPrevBtn, &QPushButton::clicked, this, [this]() {
        if (m_player->hasPrevious())
            m_player->previous();
    });
    btnRow->addWidget(m_ovPrevBtn);

    m_ovPlayBtn = new QPushButton(m_ctrlBar);
    m_ovPlayBtn->setObjectName(QStringLiteral("iconBtn"));
    m_ovPlayBtn->setCursor(Qt::PointingHandCursor);
    m_ovPlayBtn->setIcon(nt::icon(nt::Icon::Play, nt::kText, 22));
    connect(m_ovPlayBtn, &QPushButton::clicked, this,
            &WatchPage::onPlayPause);
    btnRow->addWidget(m_ovPlayBtn);

    m_ovNextBtn = new QPushButton(m_ctrlBar);
    m_ovNextBtn->setObjectName(QStringLiteral("iconBtn"));
    m_ovNextBtn->setCursor(Qt::PointingHandCursor);
    m_ovNextBtn->setIcon(nt::icon(nt::Icon::Next, nt::kText, 20));
    connect(m_ovNextBtn, &QPushButton::clicked, this, [this]() {
        if (m_player->hasNext())
            m_player->next();
    });
    btnRow->addWidget(m_ovNextBtn);

    btnRow->addStretch(1);

    m_ovMuteBtn = new QPushButton(m_ctrlBar);
    m_ovMuteBtn->setObjectName(QStringLiteral("iconBtn"));
    m_ovMuteBtn->setCursor(Qt::PointingHandCursor);
    m_ovMuteBtn->setIcon(nt::icon(nt::Icon::Mute, nt::kText, 18));
    connect(m_ovMuteBtn, &QPushButton::clicked, this,
            &WatchPage::onToggleMute);
    btnRow->addWidget(m_ovMuteBtn);

    m_ovVolume = new QSlider(Qt::Horizontal, m_ctrlBar);
    m_ovVolume->setObjectName(QStringLiteral("ovVolume"));
    m_ovVolume->setRange(0, m_player->volumeMax());
    m_ovVolume->setValue(int(m_player->volume()));
    m_ovVolume->setFixedWidth(80);
    m_ovVolume->setCursor(Qt::PointingHandCursor);
    connect(m_ovVolume, &QSlider::valueChanged, this, [this](int v) {
        m_player->setVolume(v);
        // El capado a 100% en persistencia está en MainWindow::persistVolume,
        // llamado por otros paths. Aquí, si v>100 (boost activo) no se persiste
        // para evitar que un volumen alto sobreviva al reinicio de la app.
        if (m_settings && v <= 100)
            m_settings->set(QStringLiteral("volume"), QString::number(v));
    });
    btnRow->addWidget(m_ovVolume);

    m_ovSubsBtn = new QPushButton(m_ctrlBar);
    m_ovSubsBtn->setObjectName(QStringLiteral("iconBtn"));
    m_ovSubsBtn->setCursor(Qt::PointingHandCursor);
    m_ovSubsBtn->setToolTip(QStringLiteral("Subtítulos (CC)"));
    connect(m_ovSubsBtn, &QPushButton::clicked, this, [this]() {
        m_subsVisible = !m_subsVisible;
        if (m_playerWidget)
            m_playerWidget->setSubVisibility(m_subsVisible);
        updateOverlayIcons();
    });
    btnRow->addWidget(m_ovSubsBtn);

    m_quality = new QPushButton(
        QStringLiteral("%1p").arg(idxToQuality(m_qualityIndex)), m_ctrlBar);
    // Antes objectName="iconBtn" con setFixedWidth(98): el estilo de un botón
    // cuadrado de icono (padding 6, sin borde) aplicado a un botón con texto, que
    // sin relleno ni borde no se leía como control. Ahora tiene estilo propio y
    // ancho por contenido — los 98 fijos además desperdiciaban hueco, y el menú
    // se ancla a m_quality->width() para que siga cuadrando.
    m_quality->setObjectName(QStringLiteral("chip"));
    m_quality->setToolTip(QStringLiteral("Calidad de vídeo"));
    m_quality->setCursor(Qt::PointingHandCursor);
    connect(m_quality, &QPushButton::clicked, this,
            &WatchPage::toggleQualityMenu);
    btnRow->addWidget(m_quality);

    m_ovFsBtn = new QPushButton(m_ctrlBar);
    m_ovFsBtn->setObjectName(QStringLiteral("iconBtn"));
    m_ovFsBtn->setCursor(Qt::PointingHandCursor);
    m_ovFsBtn->setIcon(nt::icon(nt::Icon::Fullscreen, nt::kText, 18));
    connect(m_ovFsBtn, &QPushButton::clicked, this,
            &WatchPage::toggleFullscreen);
    btnRow->addWidget(m_ovFsBtn);

    barLay->addLayout(btnRow);
    gl1->addWidget(m_ctrlBar);
    m_ctrlBar->hide();

    // Lógica responsive: si el ancho del vídeo no cabe la fila completa, se
    // oculta el slider de volumen. El mute sigue estando, asi que no se pierde
    // la capacidad de silenciar. LoCONNECTAMOS al widget que realmente conoce el
    // ancho (OverlayHost), así no hay que adivinarlo por el parent.
    gl->onWidth = [this](int w) {
        if (!m_ovVolume)
            return;
        if (w <= 0) {
            if (m_ovVolume->isHidden())
                m_ovVolume->setVisible(true);
            return;
        }
        const bool showVol = (w >= kOverlayFullRowMinW);
        // isHidden() y NO isVisible(): la caja vive oculta casi todo el tiempo,
        // y isVisible() devuelve false en cuanto el padre está oculto, así que
        // con isVisible() la guarda nunca se cumpliría (el volumen no se
        // escondería nunca) y además se forzaría un relayout en cada resize.
        if (m_ovVolume->isHidden() == showVol)
            m_ovVolume->setVisible(showVol);
    };

    m_qualityMenu = new QFrame(gl);
    m_qualityMenu->setObjectName(QStringLiteral("qualityMenu"));
    m_qualityMenu->setAttribute(Qt::WA_StyledBackground, true);
    // El estilo vive en el QSS global (QFrame#qualityMenu): aquí era un
    // setStyleSheet inline que repetía #1F1F1F y #2D2D2D, que ya son tokens.
    auto *menuLay = new QVBoxLayout(m_qualityMenu);
    menuLay->setContentsMargins(4, 4, 4, 4);
    menuLay->setSpacing(1);
    // En perfil soft-all (decodificación por CPU, sin HW) no tiene sentido
    // ofrecer 1440p/2160p: el menú los oculta para que el usuario no elija una
    // resolución que un CPU puro no puede mover. Los índices se mantienen
    // alineados con idxToQuality(): solo se ocultan, no se quitan.
    const bool softAll = m_settings
        && m_settings->getString(QStringLiteral("codec_profile"),
                                 QStringLiteral("av1-hw"))
               == QStringLiteral("soft-all");
    m_qualityItems.reserve(kQualityCount);
    for (int i = 0; i < kQualityCount; ++i) {
        auto *item = new QPushButton(
            QStringLiteral("%1p").arg(kQualityHeights[i]), m_qualityMenu);
        item->setFixedWidth(90);
        // soft-all: CPU solo, no se ofrecen 1440p/2160p. Cada item se
        // guarda en m_qualityItems para ocultarlo en caliente si el mpv
        // emite un techo real inferior (ver applyQualityLimits).
        item->setVisible(!(softAll && kQualityHeights[i] >= 1440));
        const int idx = i;
        connect(item, &QPushButton::clicked, this, [this, idx]() {
            setQualityIndex(idx);
            m_qualityMenu->hide();
            m_qualityMenuActive = false;
        });
        menuLay->addWidget(item);
        m_qualityItems.append(item);
    }
    m_qualityMenu->adjustSize();
    m_qualityMenu->hide();

    m_qualityMenuAnchor = gl;

    m_overlay->addWidget(gl);
    auto *spinner = new QLabel(QStringLiteral("Cargando…"), this);
    spinner->setAlignment(Qt::AlignCenter);
    spinner->setObjectName(QStringLiteral("ovSpinner"));
    m_overlay->addWidget(spinner);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setAlignment(Qt::AlignCenter);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setObjectName(QStringLiteral("ovError"));
    m_overlay->addWidget(m_errorLabel);
    m_overlay->setCurrentIndex(0);
    enableOverlayTracking(this, gl);

    // Ocultar los controles flotantes tras inactividad del ratón.
    m_overlayTimer = new QTimer(this);
    m_overlayTimer->setSingleShot(true);
    m_overlayTimer->setInterval(2500);
    connect(m_overlayTimer, &QTimer::timeout, this,
            &WatchPage::hideOverlayControls);

    auto *pc = new PlayerContainer(playerWidget, m_overlay, this);
    m_playerContainer = pc;
    pc->setObjectName(QStringLiteral("playerArea"));
    // PlayerContainer es un QWidget plano, no un QFrame: Qt solo pinta el fondo
    // que le da el QSS si el widget lleva WA_StyledBackground. Con el fondo en
    // el stylesheet global (en vez de un setStyleSheet propio) hay que decirlo
    // explícitamente, o las bandas letterbox quedan en kBg (#0F0F0F) en vez de
    // negro y se nota el marco alrededor del vídeo.
    pc->setAttribute(Qt::WA_StyledBackground, true);
    pc->ensurePolished();
    enableOverlayTracking(this, pc);
    connect(playerWidget, &PlayerWidget::videoAspectChanged, pc,
            &PlayerContainer::setVideoAspect);
    // Sin marco de foco tipo botón al hacer click sobre el vídeo.
    pc->setFocusPolicy(Qt::NoFocus);
    playerWidget->setFocusPolicy(Qt::NoFocus);
    m_overlay->setFocusPolicy(Qt::NoFocus);
    gl->setFocusPolicy(Qt::NoFocus);

    // Scroll ÚNICO de toda la página: vídeo + descripción + "A continuación"
    // apilados y afectados por el MISMO scroll (estilo YouTube). Al bajar, el
    // vídeo se va de la pantalla y descripción/cola ganan espacio. La altura
    // del vídeo la fija PlayerContainer::updateMaxHeight (cap por aspecto
    // hasta 560 px) dentro del contenido scrolleable.
    auto *topScroll = new QScrollArea(this);
    topScroll->setWidgetResizable(true);
    topScroll->setFrameShape(QFrame::NoFrame);
    topScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_topScroll = topScroll;
    auto *topWidget = new QWidget;
    auto *top = new QVBoxLayout(topWidget);
    top->setContentsMargins(0, 0, 0, 0);
    top->setSpacing(0);
    topScroll->setWidget(topWidget);
    splitOuter->addWidget(topScroll, 1);

    // El reproductor vive DENTRO del scroll (arriba del contenido): se mueve
    // con la página.
    top->addWidget(pc);

    // Fila media DENTRO del scroll: descripción a la izquierda y "A
    // continuación" a la derecha, SIN límite vertical en la cola (crece con
    // todas sus filas y el scroll general la recorre, sin scroll interno).
    auto *midRow = new QWidget;
    auto *midLay = new QHBoxLayout(midRow);
    midLay->setContentsMargins(0, 0, 0, 0);
    midLay->setSpacing(0);

    auto *infoWidget = new QWidget;
    infoWidget->setObjectName(QStringLiteral("watchBody"));
    m_watchBody = infoWidget;
    auto *infoLay = new QVBoxLayout(infoWidget);
    infoLay->setContentsMargins(16, 14, 16, 8);
    infoLay->setSpacing(6);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("watchTitle"));
    m_title->setWordWrap(true);
    infoLay->addWidget(m_title);

    m_meta = new QLabel(this);
    m_meta->setObjectName(QStringLiteral("watchMeta"));
    infoLay->addWidget(m_meta);

    m_channelBtn = new QPushButton(QStringLiteral("Canal"), this);
    m_channelBtn->setObjectName(QStringLiteral("channelBtn"));
    m_channelBtn->setCursor(Qt::PointingHandCursor);
    m_channelBtn->setIconSize(QSize(43, 40));
    m_channelBtn->setIcon(nt::icon(nt::Icon::User, nt::kMuted, 20));
    // A proposito NO es #ghost: el boton lleva icono a la izquierda y texto a la
    // derecha alineado, y necesita padding de 8px en vez de los 6px de #ghost.
    m_channelBtn->setObjectName(QStringLiteral("channelBtn"));
    m_channelBtn->setMinimumHeight(56);
    m_channelBtn->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);

    m_subscribeBtn = new QPushButton(QStringLiteral("Suscribirse"), this);
    m_subscribeBtn->setObjectName(QStringLiteral("subscribe"));
    m_subscribeBtn->setCursor(Qt::PointingHandCursor);
    m_subscribeBtn->setMinimumHeight(36);
    // Sin stylesheet propio: el estado inicial (no suscrito, con acento) lo da
    // la regla QPushButton#subscribe, y el hover es la variante clara de
    // @ACCENT@, asi que sigue al color elegido sin re-aplicar nada.

    auto *channelRow = new QHBoxLayout;
    channelRow->setSpacing(8);
    channelRow->addWidget(m_channelBtn);
    channelRow->addStretch(1);
    channelRow->addWidget(m_subscribeBtn, 0, Qt::AlignVCenter);
    infoLay->addLayout(channelRow);

    // Fila de acciones. Altura 44 px (WCAG 2.5.5, objetivo táctil recomendado;
    // 40 px se quedaba corto) y 10 px de separacion entre objetivos contiguos.
    // El segmentado crece a la misma altura para que la fila no quede escalonada.
    auto *actionsRow = new FlowLayout(0, 10, 10);

    // Grupo segmentado Me gusta / No me gusta. Ambos son de icono solo: el texto
    // de los 6 chips liberaba media fila y el FlowLayout saltaba de línea. El
    // tooltip y el nombre accesible conservan la etiqueta.
    auto *seg = new QWidget(this);
    seg->setObjectName(QStringLiteral("actionSeg"));
    seg->setFixedHeight(44);
    auto *segLay = new QHBoxLayout(seg);
    segLay->setContentsMargins(0, 0, 0, 0);
    segLay->setSpacing(0);
    auto makeSeg = [&](const QString &objName, nt::Icon icon, const QString &tip,
                       QPushButton **out, nt::Icon *iconOut) {
        auto *b = new QPushButton(seg);
        b->setObjectName(objName);
        b->setCursor(Qt::PointingHandCursor);
        b->setIcon(nt::icon(icon, nt::kMuted, 20));
        b->setIconSize(QSize(20, 20));
        b->setFixedSize(52, 44);
        b->setToolTip(tip);
        b->setAccessibleName(tip);
        *out = b;
        *iconOut = icon;
    };
    makeSeg(QStringLiteral("actionSegL"), nt::Icon::Thumb,
            QStringLiteral("Me gusta"), &m_likeBtn, &m_likeIcon);
    makeSeg(QStringLiteral("actionSegR"), nt::Icon::ThumbDown,
            QStringLiteral("No me gusta"), &m_dislikeBtn, &m_dislikeIcon);
    segLay->addWidget(m_likeBtn);
    segLay->addWidget(m_dislikeBtn);
    actionsRow->addWidget(seg);

    // Resto de acciones: pastilla circular de icono solo, con tooltip. Se
    // añaden EXPLÍCITAMENTE a actionsRow: crear el botón no basta, sin
    // addWidget queda fuera del layout y no se ve.
    auto makeAction = [this, actionsRow](nt::Icon icon, const QString &tip,
                             QPushButton **out, nt::Icon *iconOut) {
        auto *b = new QPushButton(this);
        b->setObjectName(QStringLiteral("actionChip"));
        b->setCursor(Qt::PointingHandCursor);
        b->setIcon(nt::icon(icon, nt::kMuted, 20));
        b->setIconSize(QSize(20, 20));
        b->setFixedSize(44, 44);
        b->setToolTip(tip);
        b->setAccessibleName(tip);
        actionsRow->addWidget(b);
        *out = b;
        *iconOut = icon;
    };
    makeAction(nt::Icon::Share, QStringLiteral("Compartir"),
               &m_shareBtn, &m_shareIcon);
    makeAction(nt::Icon::Star, QStringLiteral("Favorito"),
               &m_starBtn, &m_starIcon);
    makeAction(nt::Icon::WatchLater, QStringLiteral("Ver más tarde"),
               &m_watchLaterBtn, &m_watchLaterIcon);
    makeAction(nt::Icon::ListAdd, QStringLiteral("Agregar a playlist"),
               &m_playlistBtn, &m_playlistIcon);
    connect(m_playlistBtn, &QPushButton::clicked, this,
            &WatchPage::showPlaylistMenu);
    infoLay->addLayout(actionsRow);

    m_descLabel = new QLabel(this);
    m_descLabel->setWordWrap(true);
    m_descLabel->setTextFormat(Qt::RichText);
    m_descLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse
                                         | Qt::TextSelectableByMouse);
    m_descLabel->setOpenExternalLinks(true);
    m_descLabel->setObjectName(QStringLiteral("watchDesc"));
    // El colapso lo decide updateDescCollapse() según el ancho REAL (no un
    // número fijo de caracteres). El filtro de eventos de la página re-evalúa
    // al redimensionar la etiqueta, para que el botón no aparezca de más ni
    // falte al cambiar el tamaño de la ventana.
    // Tarjeta de la descripción: fondo #181818 con el texto y el control
    // "…más"/"…menos", y un degradado en el borde inferior cuando está
    // colapsada (el texto se funde con el fondo en vez de cortarse).
    m_descCard = new QFrame(this);
    m_descCard->setObjectName(QStringLiteral("watchDescCard"));
    m_descCard->installEventFilter(this);
    auto *descLay = new QVBoxLayout(m_descCard);
    descLay->setContentsMargins(12, 10, 12, 10);
    descLay->setSpacing(4);
    m_descLabel->installEventFilter(this);
    descLay->addWidget(m_descLabel);

    m_descFade = new QFrame(m_descCard);
    m_descFade->setObjectName(QStringLiteral("descFade"));
    m_descFade->hide();

    m_descToggle = new QPushButton(QStringLiteral("…más"), m_descCard);
    m_descToggle->setObjectName(QStringLiteral("descToggle"));
    m_descToggle->setCursor(Qt::PointingHandCursor);
    m_descToggle->hide();
    connect(m_descToggle, &QPushButton::clicked, this, [this]() {
        m_descExpanded = !m_descExpanded;
        updateDescCollapse();
    });
    descLay->addWidget(m_descToggle, 0, Qt::AlignLeft);
    infoLay->addWidget(m_descCard);
    midLay->addWidget(infoWidget, 7);

    auto *bottomWidget = new QWidget;
    m_bottomWidget = bottomWidget;
    auto *bot = new QVBoxLayout(bottomWidget);
    bot->setContentsMargins(0, 8, 16, 8);
    bot->setSpacing(6);
    auto *quRow = new QHBoxLayout;
    quRow->setContentsMargins(0, 0, 0, 0);
    quRow->setSpacing(8);
    auto *quTitle = new QLabel(QStringLiteral("A continuación"), this);
    quTitle->setObjectName(QStringLiteral("queueTitle"));
    m_queueCount = new QLabel(this);
    m_queueCount->setObjectName(QStringLiteral("queueCount"));
    quRow->addWidget(quTitle);
    quRow->addStretch(1);
    quRow->addWidget(m_queueCount);
    m_nextUpList = new VideoListView(m_thumbs, this);
    m_nextUpList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_nextUpList->setSizePolicy(QSizePolicy::Expanding,
                                QSizePolicy::Expanding);
    m_nextUpList->setMinimumHeight(90);
    bot->addLayout(quRow);
    bot->addWidget(m_nextUpList);

    // "A continuación" a la derecha de la descripción, dentro del scroll
    // general (sin límite vertical: la altura la fija updateQueue al
    // contenido real).
    midLay->addWidget(bottomWidget, 3);
    // Ambos bloques conservan su alto NATURAL y quedan alineados arriba: la
    // fila crece con la cola (sin límite), pero la descripción no se estira a
    // esa altura — debajo de cada bloque solo queda espacio vacío scrollable.
    midLay->setAlignment(infoWidget, Qt::AlignTop);
    midLay->setAlignment(bottomWidget, Qt::AlignTop);
    top->addWidget(midRow);
    // El stretch absorbe el sobrante cuando el contenido no llena la ventana:
    // los bloques quedan pegados arriba, con hueco al final (como YouTube).
    top->addStretch(1);

    // Botón "Volver" flotante sobre el área de vídeo: usa el estilo ghost
    // transparente que ya tenía, sin ocupar una barra. Flota arriba-izquierda
    // de la página (fijo, NO scrollea con el contenido) y se oculta en
    // fullscreen. Se crea al final para quedar como sibling posterior (por
    // encima del scroll).
    // El icono Back (abajo) ya es la flecha: el "◀" del texto era un glifo
    // que no existe en la fuente por defecto y salia como un punto, ademas
    // duplicaba la misma affordance.
    m_back = new QPushButton(QStringLiteral("Volver"), this);
    m_back->setObjectName(QStringLiteral("ghost"));
    m_back->setIcon(nt::icon(nt::Icon::Back, nt::kText, 14));
    m_back->setCursor(Qt::PointingHandCursor);
    m_back->move(12, 8);
    m_back->raise();

    connect(m_back, &QPushButton::clicked, this,
            &WatchPage::backRequested);
    connect(m_channelBtn, &QPushButton::clicked, this, [this]() {
        if (m_player->index() < 0)
            return;
        const VideoInfo v = m_player->current();
        if (!v.channelId.isEmpty())
            emit channelRequested(v.channelId, v.channel);
    });
    connect(m_subscribeBtn, &QPushButton::clicked, this, [this]() {
        if (m_player->index() < 0)
            return;
        const VideoInfo v = m_player->current();
        if (v.channelId.isEmpty())
            return;
        updateSubscription(v, !m_subs->isSubscribed(v.channelId));
    });
    connect(m_likeBtn, &QPushButton::clicked, this, [this]() {
        const VideoInfo v = m_player->current();
        if (v.id.isEmpty())
            return;
        m_favs->toggle(v.id);
        m_liked = m_favs->isFavorite(v.id);
        if (m_liked)
            m_disliked = false;
        updateActionButtons();
    });
    connect(m_starBtn, &QPushButton::clicked, this, [this]() {
        const VideoInfo v = m_player->current();
        if (v.id.isEmpty())
            return;
        m_starred->toggle(v.id);
        m_isStarred = m_starred->isStarred(v.id);
        updateActionButtons();
    });
    connect(m_watchLaterBtn, &QPushButton::clicked, this, [this]() {
        const VideoInfo v = m_player->current();
        if (v.id.isEmpty())
            return;
        m_watchLater->toggle(v.id);
        m_watchLaterMarked = m_watchLater->isWatchLater(v.id);
        updateActionButtons();
    });
    connect(m_dislikeBtn, &QPushButton::clicked, this, [this]() {
        const VideoInfo v = m_player->current();
        if (v.id.isEmpty())
            return;
        m_disliked = !m_disliked;
        if (m_disliked)
            m_liked = false;
        updateActionButtons();
    });
    connect(m_shareBtn, &QPushButton::clicked, this, [this]() {
        const VideoInfo v = m_player->current();
        if (v.id.isEmpty())
            return;
        const QString url = QStringLiteral(
            "https://www.youtube.com/watch?v=%1").arg(v.id);
        QGuiApplication::clipboard()->setText(url);
        QToolTip::showText(m_shareBtn->mapToGlobal(QPoint(0, 0)),
                           QStringLiteral("Enlace copiado"), m_shareBtn);
    });
    connect(m_player, &PlayerController::currentChanged, this,
            &WatchPage::onCurrentChanged);
    connect(m_player, &PlayerController::queueChanged, this,
            &WatchPage::onQueueChanged);
    connect(m_player, &PlayerController::phaseChanged, this,
            &WatchPage::onPhaseChanged);
    connect(m_player, &PlayerController::positionChanged, this,
            &WatchPage::onPositionChanged);
    connect(m_player, &PlayerController::volumeChanged, this,
            &WatchPage::onVolumeChanged);
    connect(playerWidget, &PlayerWidget::videoSizeChanged, this,
            &WatchPage::onVideoSizeChanged);
    // Cuando mpv carga el archivo emite la altura máxima real de sus
    // pistas; si limita a 720p, ocultamos 1080p/1440p/2160p del menú de
    // resoluciones (mpv no podría seleccionar esas pistas y quedaba como
    // "click inerte" para el usuario).
    connect(playerWidget, &PlayerWidget::qualityLimitsChanged, this,
            &WatchPage::applyQualityLimits);
    // Alturas reales del vídeo (Innertube): si Auto pidió 360 de un vídeo 1080,
    // el menú debe seguir ofreciendo 1080 aunque el stream cargado sea 360.
    connect(m_yt, &Yt::streamHeightsReady, this, &WatchPage::onStreamHeights);
    connect(m_player, &PlayerController::durationChanged, this,
            &WatchPage::onDurationKnown);
    connect(m_yt, &Yt::detailReady, this, &WatchPage::onDetailReady);
    connect(m_yt, &Yt::detailFailed, this, &WatchPage::onDetailFailed);
    connect(m_yt, &Yt::relatedReady, this, &WatchPage::onRelatedReady);
    connect(m_yt, &Yt::radioReady, this, &WatchPage::onRadioReady);
    connect(m_yt, &Yt::currentDetailReady, this,
            &WatchPage::onCurrentDetailReady);
    connect(m_yt, &Yt::channelInfoReady, this,
            &WatchPage::onChannelInfo);
    connect(m_thumbs, &ThumbnailCache::ready, this,
            &WatchPage::onAvatarReady);
    connect(m_nextUpList, &VideoListView::videoActivated, this,
            &WatchPage::onNextUpActivated);

    m_nextUpList->showVideos(QVector<VideoInfo>(), VideoListView::ListMode);
    bottomWidget->hide();
    m_title->setText(QStringLiteral("Reproduciendo…"));
    m_meta->clear();
    m_channelBtn->setText(QStringLiteral("Canal"));
    m_descLabel->hide();
    updateActionButtons();
}

void WatchPage::setWatchData(const QVector<VideoInfo> &queue, int startIndex)
{
    m_queue = queue;
    m_queueStart = startIndex;
    NT_DEBUG("E2E setWatchData queue=%d start=%d\n", (int)queue.size(),
             startIndex);
    updateQueue();
}

void WatchPage::triggerRadio(const QString &videoId)
{
    if (videoId.isEmpty())
        return;
    m_relatedVideoId = videoId;
    NT_DEBUG("E2E nextUp triggerRadio id=%s\n",
             videoId.toUtf8().constData());
    m_yt->radioVideos(videoId);
}

void WatchPage::detachPlayerTo(QWidget *host)
{
    if (!m_playerWidget || !host || m_playerDetached)
        return;
    // El overlay (controles) es hijo del player: se oculta, no se mueve. El
    // player se reparenta visible para que mpv siga renderizando en el mini.
    m_overlay->hide();
    m_playerWidget->setParent(host);
    m_playerWidget->setGeometry(host->rect());
    m_playerWidget->show();
    m_playerDetached = true;
    NT_DEBUG("E2E player detach host=%p\n", static_cast<void *>(host));
}

void WatchPage::attachPlayer()
{
    if (!m_playerWidget || !m_playerContainer || !m_playerDetached)
        return;
    m_playerContainer->givePlayerBack();
    m_overlay->setGeometry(m_playerWidget->rect());
    m_overlay->show();
    m_playerDetached = false;
    NT_DEBUG("E2E player attach\n");
}

void WatchPage::onCurrentChanged()
{
    if (!m_player->isActive()) {
        m_title->setText(QStringLiteral("Reproduciendo…"));
        return;
    }
    // Página en un único scroll (estilo YouTube): al cambiar de vídeo se
    // vuelve arriba para que el nuevo parta desde el vídeo. No en fullscreen
    // (ahí el scroll está oculto y el vídeo llena la página).
    if (m_topScroll && !m_inFullscreen)
        m_topScroll->verticalScrollBar()->setValue(0);
    // Nuevo vídeo: el techo de calidades disponibles se recalcula con su
    // streamHeightsReady; hasta entonces no se arrastra el del anterior.
    m_availableMaxH = 0;
    m_title->setText(m_player->current().title);
    m_liked = m_favs->isFavorite(m_player->current().id);
    m_disliked = false;
    m_isStarred = m_starred->isStarred(m_player->current().id);
    m_watchLaterMarked = m_watchLater->isWatchLater(m_player->current().id);
    updateActionButtons();
    updateInfo();
    updateQueue();
    m_yt->getDetail(m_player->current().id);
    // Detalle traducido (/next normal): solo cuando el idioma del ajuste es
    // explícito (es/en/pt); en "original" el /next no aporta y se ahorra la
    // petición. Se difiere una iteración del event loop para que el POST
    // /player (camino crítico de reproducción) pase por delante: Ipv4Https
    // es FIFO de un solo socket, y si este /next entra antes, el vídeo
    // espera a que termine la traducción antes de pedir el stream.
    if (!m_yt->audioLanguage().isEmpty()) {
        const QString id = m_player->current().id;
        QTimer::singleShot(0, this, [this, id]() {
            if (!m_yt->audioLanguage().isEmpty())
                m_yt->currentDetail(id);
        });
    }
    m_fetchingDetail = true;
    m_description.clear();
    m_meta->setText(QStringLiteral("Cargando detalles…"));
}

void WatchPage::onQueueChanged()
{
    m_queue = m_player->queue();
    updateQueue();
}

void WatchPage::updateQueue()
{
    if (!m_player->isActive()) {
        m_nextUpList->clearVideos();
        if (m_bottomWidget)
            m_bottomWidget->setVisible(false);
        m_relatedVideoId.clear();
        return;
    }
    const int idx = m_player->index();
    const VideoInfo cur = m_player->current();
    const int remaining = m_queue.size() - (idx + 1);
    const NextMode mode = nextMode();

    // Siembra: solo Suggestions auto-llena la cola cuando quedan pocos ítems.
    // Radio se genera por un click explícito desde fuera de "A continuación"
    // (search, home, channel) y NO auto-recarga al agotar.
    if (mode == NextMode::Suggestions && remaining <= 5 && !cur.id.isEmpty()
        && cur.id != m_relatedVideoId) {
        m_relatedVideoId = cur.id;
        NT_DEBUG("E2E nextUp seed mode=suggestions id=%s remaining=%d\n",
                 cur.id.toUtf8().constData(), remaining);
        m_yt->relatedVideos(cur.id);
    }

    // Lista completa de la colección: el vídeo en reproducción queda marcado
    // con recuadro y los saltados/ya vistos siguen visibles y elegibles.
    m_nextUpList->showVideos(m_queue, VideoListView::ListMode);
    m_nextUpList->setCurrentRow(idx);
    if (m_queueCount)
        m_queueCount->setText(m_queue.isEmpty()
            ? QString()
            : QStringLiteral("%1 vídeos").arg(m_queue.size()));
    // La cola vive DENTRO del scroll de la página: su altura se ajusta al
    // contenido real (filas × alto de fila) para que el scroll de la página
    // mueva la cola entera, sin scroll interno duplicado.
    const int rowH = m_nextUpList->sizeHintForRow(0);
    if (rowH > 0)
        m_nextUpList->setFixedHeight(qMax(1, m_queue.size()) * rowH);
    if (m_bottomWidget)
        m_bottomWidget->setVisible(!m_queue.isEmpty());
}

void WatchPage::onRelatedReady(const QString &videoId,
                               const QVector<VideoInfo> &videos)
{
    if (videos.isEmpty() || videoId != m_relatedVideoId)
        return;
    const VideoInfo cur = m_player->current();
    if (cur.id != m_relatedVideoId)
        return;
    NT_DEBUG("E2E nextUp related append id=%s count=%d\n",
             videoId.toUtf8().constData(), (int)videos.size());
    m_player->appendVideos(videos);
}

void WatchPage::onRadioReady(const QString &videoId,
                             const QVector<VideoInfo> &videos)
{
    if (videoId != m_relatedVideoId)
        return;
    const VideoInfo cur = m_player->current();
    if (cur.id != m_relatedVideoId)
        return;
    if (!videos.isEmpty()) {
        NT_DEBUG("E2E nextUp radio append id=%s count=%d 1º=%s\n",
                 videoId.toUtf8().constData(), (int)videos.size(),
                 videos.first().title.toUtf8().constData());
        m_player->appendVideos(videos);
        return;
    }
    // Radio RDAMVM vacía (ni panel ni mezcla en ningún cliente): último
    // recurso con el /next plano de recomendaciones para no dejar "A
    // continuación" vacío. La guarda de onRelatedReady valida m_relatedVideoId.
    NT_DEBUG("E2E nextUp radio vacia -> fallback /next id=%s\n",
             videoId.toUtf8().constData());
    m_yt->relatedVideos(videoId);
}

void WatchPage::updateInfo()
{
    if (!m_player->isActive())
        return;
    const VideoInfo v = m_player->current();
    QStringList meta;
    if (v.viewCount > 0)
        meta << viewsLabel(v.viewCount);
    if (!v.publishedAt.isEmpty()) {
        const QString p = publishedLabel(v.publishedAt);
        if (!p.isEmpty())
            meta << p;
    }
    m_meta->setText(meta.join(QStringLiteral("  •  ")));
    updateChannelRow();
}

void WatchPage::updateChannelRow()
{
    if (!m_player->isActive())
        return;
    const VideoInfo v = m_player->current();
    m_channelBtn->setText(v.channel.isEmpty() ? QStringLiteral("Canal")
                                              : v.channel);
    m_channelAvatarKey.clear();
    m_channelAvatarUrl.clear();
    const QIcon placeholder = nt::icon(nt::Icon::User, nt::kMuted, 20);
    if (!v.channelId.isEmpty()) {
        m_channelAvatarKey = QStringLiteral("ch_%1").arg(v.channelId);
        const QPixmap pm = m_thumbs->pixmap(m_channelAvatarKey);
        if (!pm.isNull()) {
            m_channelBtn->setIcon(avatarWithGap(circularAvatar(pm, 40), 40, 3));
        } else {
            m_channelBtn->setIcon(placeholder);
            // Pide datos del canal (nombre y avatar) si faltan.
            m_yt->channelInfo(v.channelId);
        }
    } else {
        m_channelBtn->setIcon(placeholder);
    }
    updateSubscribeButton(m_subs->isSubscribed(v.channelId));
}

void WatchPage::updateSubscribeButton(bool subscribed)
{
    m_subscribeBtn->setText(subscribed ? QStringLiteral("Suscrito")
                                       : QStringLiteral("Suscribirse"));
    m_subscribeBtn->setIcon(QIcon());
    // Misma regla #subscribe que el boton de la ficha de canal: los dos
    // estados los decide la propiedad `subscribed` y el acento sale de
    // @ACCENT@. Re-polish obligatorio al cambiar la propiedad.
    m_subscribeBtn->setProperty("subscribed", subscribed);
    m_subscribeBtn->style()->unpolish(m_subscribeBtn);
    m_subscribeBtn->style()->polish(m_subscribeBtn);
    m_subscribeBtn->update();
}

void WatchPage::updateSubscription(const VideoInfo &v, bool subscribe)
{
    if (!subscribe) {
        m_subs->remove(v.channelId);
    } else {
        m_subs->add(v.channelId, v.channel, m_channelAvatarUrl);
    }
    updateSubscribeButton(subscribe);
}

void WatchPage::updateActionButtons()
{
    auto paint = [this](QPushButton *b, nt::Icon icon, bool active) {
        b->setIcon(nt::icon(icon, active ? QColor(nt::accentHex())
                                         : nt::kMuted, 20));
        b->setIconSize(QSize(20, 20));
        // Los dos estados viven en el QSS global (#actionChip) y los distingue
        // la propiedad booleana `active`, igual que el boton Suscribirse. El
        // acento sale de @ACCENT@, asi que un cambio de color lo repinta solo
        // y ya no hace falta rehacer el stylesheet aqui.
        b->setProperty("active", active);
        // La propiedad cambia el selector, asi que hay que re-polish: sin esto
        // Qt no vuelve a resolver las reglas y el boton conserva el estado
        // anterior (por ejemplo, "Me gusta" pulsado que no se ve marcado).
        b->style()->unpolish(b);
        b->style()->polish(b);
        b->update();
    };
    paint(m_likeBtn, m_likeIcon, m_liked);
    paint(m_dislikeBtn, m_dislikeIcon, m_disliked);
    paint(m_shareBtn, m_shareIcon, false);
    paint(m_starBtn, m_isStarred ? m_starFilledIcon : m_starIcon, m_isStarred);
    paint(m_watchLaterBtn, m_watchLaterIcon, m_watchLaterMarked);
    paint(m_playlistBtn, m_playlistIcon, false);
}

void WatchPage::showPlaylistMenu()
{
    if (!m_player->isActive())
        return;
    const VideoInfo v = m_player->current();
    if (v.id.isEmpty())
        return;

    QMenu menu(this);
    const auto playlists = m_playlists->all();
    bool anyAdded = false;
    for (const PlaylistItem &p : playlists) {
        const bool contains = m_playlists->contains(p.id, v.id);
        if (contains)
            anyAdded = true;
        // Sin glifo de check: "✔" (U+2714) no esta en la fuente por defecto y
        // salia como un punto. El "(ya esta)" ya dice lo mismo.
        const QString label = contains
            ? QStringLiteral("%1 (ya está)").arg(p.title)
            : p.title;
        QAction *act = menu.addAction(label);
        act->setEnabled(!contains);
        connect(act, &QAction::triggered, this,
                [this, p, v]() { m_playlists->addVideo(p.id, v.id); });
    }
    menu.addSeparator();
    QAction *newAct = menu.addAction(QStringLiteral("+ Nueva playlist"));
    connect(newAct, &QAction::triggered, this, [this, v]() {
        m_plPendingVideoId = v.id;
        openCreatePlaylistPopup();
    });
    if (playlists.isEmpty() && !anyAdded)
        menu.addAction(QStringLiteral("Sin playlists todavía"))->setEnabled(false);
    menu.exec(m_playlistBtn
                  ? m_playlistBtn->mapToGlobal(QPoint(0, m_playlistBtn->height()))
                  : QCursor::pos());
}

// Popup estilado "Nueva playlist" con el mismo popupDialog que Biblioteca,
// para que el flujo de agregar a playlist tenga el mismo diseño en toda la app.
void WatchPage::openCreatePlaylistPopup()
{
    if (!m_plPopup) {
        m_plPopup = new QFrame(this);
        m_plPopup->setObjectName(QStringLiteral("popupDialog"));
        m_plPopup->setFixedWidth(340);
        m_plPopup->raise();
        m_plPopup->hide();

        auto *v = new QVBoxLayout(m_plPopup);
        v->setContentsMargins(18, 16, 18, 14);
        v->setSpacing(10);

        m_plPopupTitle = new QLabel(QStringLiteral("Nueva playlist"), m_plPopup);
        m_plPopupTitle->setObjectName(QStringLiteral("popupTitle"));
        v->addWidget(m_plPopupTitle);

        m_plName = new QLineEdit(m_plPopup);
        m_plName->setPlaceholderText(QStringLiteral("Nombre de la playlist"));
        m_plName->setClearButtonEnabled(true);
        m_plName->installEventFilter(this);
        v->addWidget(m_plName);

        auto *row = new QHBoxLayout;
        row->addStretch(1);
        auto *cancel = new QPushButton(QStringLiteral("Cancelar"), m_plPopup);
        cancel->setObjectName(QStringLiteral("ghost"));
        cancel->setCursor(Qt::PointingHandCursor);
        connect(cancel, &QPushButton::clicked, this,
                &WatchPage::closeCreatePlaylistPopup);
        row->addWidget(cancel);
        m_plOk = new QPushButton(QStringLiteral("Crear"), m_plPopup);
        m_plOk->setObjectName(QStringLiteral("primary"));
        m_plOk->setCursor(Qt::PointingHandCursor);
        connect(m_plOk, &QPushButton::clicked, this,
                &WatchPage::submitCreatePlaylist);
        row->addWidget(m_plOk);
        v->addLayout(row);

        connect(m_plName, &QLineEdit::returnPressed, this,
                &WatchPage::submitCreatePlaylist);
    }
    m_plPopupTitle->setText(QStringLiteral("Nueva playlist"));
    m_plOk->setText(QStringLiteral("Crear"));
    m_plName->clear();
    m_plPopup->adjustSize();
    const int x = (width() - m_plPopup->width()) / 2;
    const int y = (height() - m_plPopup->height()) / 2;
    m_plPopup->move(qMax(0, x), qMax(0, y));
    m_plPopup->raise();
    m_plPopup->show();
    m_plName->setFocus(Qt::OtherFocusReason);
}

void WatchPage::closeCreatePlaylistPopup()
{
    if (m_plPopup)
        m_plPopup->hide();
}

void WatchPage::submitCreatePlaylist()
{
    if (!m_plPopup || !m_plName)
        return;
    const QString name = m_plName->text().trimmed();
    if (name.isEmpty())
        return;
    const QString id = m_playlists->create(name);
    if (id.isEmpty())
        return;
    if (!m_plPendingVideoId.isEmpty()) {
        m_playlists->addVideo(id, m_plPendingVideoId);
        m_plPendingVideoId.clear();
    }
    closeCreatePlaylistPopup();
}

void WatchPage::onChannelInfo(const ChannelInfo &channel)
{
    if (!m_player->isActive())
        return;
    const VideoInfo v = m_player->current();
    if (channel.id != v.channelId)
        return;
    if (!channel.name.isEmpty())
        m_channelBtn->setText(channel.name);
    m_channelAvatarKey = QStringLiteral("ch_%1").arg(channel.id);
    if (!channel.avatarUrl.isEmpty()) {
        m_channelAvatarUrl = channel.avatarUrl;
        m_thumbs->requestChannelAvatar(channel.id, channel.avatarUrl);
        if (m_subs->isSubscribed(channel.id))
            m_subs->setAvatar(channel.id, channel.avatarUrl);
    }
}

void WatchPage::onAvatarReady(const QString &key)
{
    if (key != m_channelAvatarKey)
        return;
    const QPixmap pm = m_thumbs->pixmap(key);
    if (!pm.isNull())
        m_channelBtn->setIcon(avatarWithGap(circularAvatar(pm, 40), 40, 3));
}

// El backend ya conoce la duración real del medio (la de mpv o el reloj de
// audio del FfmpegBackend). Los vídeos abiertos desde search/home/canal llegan
// con durationSeconds=0, y "Continuar viendo" descarta todo lo que no tenga
// duración en la caché: por eso un vídeo visto varios minutos nunca aparecía
// allí. Se rellena SOLO la duración y se respeta lo ya cacheado (el upsert es
// un INSERT OR REPLACE, meter un ítem de cola con campos vacíos los borraría).
void WatchPage::onDurationKnown(double seconds)
{
    const int dur = qRound(seconds);
    if (dur <= 0 || !m_cache || !m_player->isActive())
        return;
    const VideoInfo cur = m_player->current();
    if (cur.id.isEmpty())
        return;
    VideoInfo cached;
    const bool known = m_cache->get(cur.id, cached);
    if (known && cached.durationSeconds == dur)
        return; // ya está al día: no escribir por gusto
    if (known) {
        // Rellenar solo lo que falte: el upsert es INSERT OR REPLACE, así que
        // meter el ítem de cola tal cual borraría lo ya cacheado.
        if (cached.title.isEmpty())
            cached.title = cur.title;
        if (cached.channel.isEmpty())
            cached.channel = cur.channel;
        if (cached.channelId.isEmpty())
            cached.channelId = cur.channelId;
        if (cached.thumbnailUrl.isEmpty())
            cached.thumbnailUrl = cur.thumbnailUrl;
        if (cached.viewCount == 0)
            cached.viewCount = cur.viewCount;
        if (cached.publishedAt.isEmpty())
            cached.publishedAt = cur.publishedAt;
        cached.durationSeconds = dur;
    } else {
        cached = cur;
        cached.durationSeconds = dur;
    }
    m_cache->upsert(cached);
}

void WatchPage::onDetailFailed(const QString &videoId)
{
    if (!isCurrentVideo(videoId))
        return;
    // Ningún cliente devolvió metadatos; no dejar "Cargando detalles…"
    // clavado. Se muestra lo que ya se conoce del elemento de la cola.
    m_fetchingDetail = false;
    m_description.clear();
    m_descLabel->hide();
    updateInfo();
}

void WatchPage::onDetailReady(const VideoInfo &video, const QString &desc,
                              qint64 likes, const QVector<FormatInfo> &formats)
{
    Q_UNUSED(formats);
    if (!isCurrentVideo(video.id))
        return;
    m_player->applyCurrentDetail(video);
    m_title->setText(m_player->current().title);
    updateChannelRow();
    m_fetchingDetail = false;
    m_likeCount = likes;
    applyDescription(desc);
    QStringList meta;
    if (video.viewCount > 0)
        meta << viewsLabel(video.viewCount);
    if (!video.publishedAt.isEmpty()) {
        const QString p = publishedLabel(video.publishedAt);
        if (!p.isEmpty())
            meta << p;
    }
    meta << QStringLiteral("%1 me gusta").arg(likes);
    m_meta->setText(meta.join(QStringLiteral("  •  ")));
    // El videoMetadataRenderer del /next NO trae duración. Como el upsert es
    // INSERT OR REPLACE, guardarlo tal cual pondría duration=0 y borraría la
    // duración real que ya cacheó onDurationKnown(). "Continuar viendo" filtra
    // por duración > 0, así que perderla sacaba el vídeo de la lista.
    VideoInfo toCache = video;
    if (toCache.durationSeconds <= 0) {
        VideoInfo known;
        if (m_cache->get(toCache.id, known) && known.durationSeconds > 0)
            toCache.durationSeconds = known.durationSeconds;
    }
    m_cache->upsert(toCache);
}

// Detalle traducido del vídeo actual (/next): título y descripción en el
// idioma "hl" solicitado. Se aplica si coincide el vídeo y el idioma no es
// "original" (cuando es original, el detail de Innertube ya muestra lo que
// YouTube entrega por defecto y no queremos sobrescribir).
void WatchPage::onCurrentDetailReady(const QString &videoId,
                                     const QString &title,
                                     const QString &description)
{
    NT_DEBUG("E2E currentDetail app id=%s title=%d desc=%d\n",
             videoId.toUtf8().constData(), title.size(), description.size());
    if (!isCurrentVideo(videoId))
        return;
    const QString audioLang = m_yt->audioLanguage();
    // Si el usuario pidió "original", no sobrescribir la descripción
    // (youtube entrega en idioma del canal; el fix hl=es ya preserva
    // hispanohablantes). Solo aplicar para idiomas explícitos.
    if (audioLang.isEmpty())
        return;
    if (!title.isEmpty()) {
        m_player->renameCurrent(videoId, title);
        m_title->setText(title);
    }
    if (!description.isEmpty())
        applyDescription(description);
}

// True si el vídeo dado es el que está en reproducción.
bool WatchPage::isCurrentVideo(const QString &videoId) const
{
    return m_player->isActive() && videoId == m_player->current().id;
}

// Pinta la descripción del vídeo como HTML enriquecido (enlaces + saltos de
// línea) y guarda el texto plano. Crece a altura completa (sin tope ni
// "Ver más"). Vacía = oculta la descripción.
void WatchPage::applyDescription(const QString &description)
{
    m_description = description;
    if (description.isEmpty()) {
        m_descLabel->hide();
        if (m_descToggle)
            m_descToggle->hide();
        if (m_descFade)
            m_descFade->hide();
        if (m_descCard)
            m_descCard->hide();
        return;
    }
    QString rich = description.toHtmlEscaped();
    rich.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    const QRegularExpression urlRe(QStringLiteral(R"((https?://[^\s<]+))"));
    rich.replace(urlRe, QStringLiteral(
        "<a href=\"\\1\" style=\"color:%1;\">\\1</a>").arg(nt::accentHex()));
    m_descLabel->setText(QStringLiteral(
        "<style>a { text-decoration:none; }</style>") + rich);
    m_descLabel->show();
    if (m_descCard)
        m_descCard->show();
    // Cada vídeo arranca colapsado. El ancho real aún no está calculado en este
    // punto, así que la decisión se toma en el siguiente ciclo del bucle.
    m_descExpanded = false;
    QTimer::singleShot(0, this, [this]() { updateDescCollapse(); });
}

// Recorta la descripción a kDescLines y muestra "…más"/"…menos" solo cuando el
// texto no cabe. Se recalcula al cambiar el ancho (eventFilter) para que el
// botón no aparezca de más ni falte al redimensionar la ventana.
void WatchPage::updateDescCollapse()
{
    if (!m_descLabel || !m_descToggle || m_inDescCollapse)
        return;
    if (m_description.isEmpty()) {
        m_descToggle->hide();
        if (m_descFade)
            m_descFade->hide();
        m_descCollapsible = false;
        return;
    }
    m_inDescCollapse = true;
    m_descLabel->setMaximumHeight(QWIDGETSIZE_MAX);
    const int w = m_descLabel->width();
    const int lineH = m_descLabel->fontMetrics().lineSpacing();
    const int maxH = lineH * kDescLines;
    const int fullH = (w > 0 && lineH > 0) ? m_descLabel->heightForWidth(w) : 0;
    m_descCollapsible = fullH > maxH + lineH / 2;
    if (!m_descCollapsible) {
        m_descToggle->hide();
        if (m_descFade)
            m_descFade->hide();
    } else {
        m_descToggle->setText(m_descExpanded ? QStringLiteral("…menos")
                                             : QStringLiteral("…más"));
        m_descToggle->show();
        if (!m_descExpanded)
            m_descLabel->setMaximumHeight(maxH);
    }
    m_inDescCollapse = false;
    positionDescFade();
}

// Coloca el degradado en el borde inferior de la tarjeta (solo con la
// descripción colapsada) y lo sube por encima del texto, dejando el botón
// "…más" por delante.
void WatchPage::positionDescFade()
{
    if (!m_descFade || !m_descCard)
        return;
    if (!(m_descCollapsible && !m_descExpanded && m_descCard->isVisible())) {
        m_descFade->hide();
        return;
    }
    const int fh = 26;
    m_descFade->setGeometry(1, qMax(0, m_descCard->height() - fh - 1),
                            qMax(0, m_descCard->width() - 2), fh);
    m_descFade->show();
    m_descFade->raise();
    if (m_descToggle)
        m_descToggle->raise();
}

void WatchPage::onPhaseChanged(const QString &phase)
{
    if (m_qualityMenuActive) {
        m_qualityMenu->hide();
        m_qualityMenuActive = false;
    }
    if (phase == QLatin1String("loading")) {
        m_overlay->setCurrentIndex(1);
    } else if (phase == QLatin1String("error")) {
        m_errorLabel->setText(m_player->errorMessage());
        m_overlay->setCurrentIndex(2);
    } else {
        m_overlay->setCurrentIndex(0);
    }
    updateOverlayIcons();
    if (m_overlay->currentIndex() == 0)
        showOverlayControls();
    else
        hideOverlayControls();
    // Auto: la memoria (quality_auto) solo cambia con una elección explícita
    // del menú (ver onQualityChanged); al terminar un vídeo no se toca el
    // reproductor ni se sube resolución en caliente.
    NT_DEBUG("E2E overlay visible=%d idx=%d\n", m_overlay->isVisible(),
             m_overlay->currentIndex());
    const QPoint qp = m_quality->mapToGlobal(QPoint(0, 0));
    NT_DEBUG("E2E combo geo global=(%d,%d) %dx%d\n", qp.x(), qp.y(),
             m_quality->width(), m_quality->height());
    if (QWidget *tw = m_quality->window())
        NT_DEBUG("E2E win-top global=(%d,%d) size=%dx%d combo-rel=(%d,%d)\n",
                 tw->mapToGlobal(QPoint(0, 0)).x(),
                 tw->mapToGlobal(QPoint(0, 0)).y(), tw->width(), tw->height(),
                 qp.x() - tw->mapToGlobal(QPoint(0, 0)).x(),
                 qp.y() - tw->mapToGlobal(QPoint(0, 0)).y());
}

void WatchPage::onPositionChanged(double position, double duration)
{
    QSignalBlocker blockOv(m_ovSeek);
    m_ovSeek->setRange(0, duration > 0 ? qRound(duration) : 0);
    if (!m_ovSeek->isSeeking())
        m_ovSeek->setValue(qRound(position));
    m_ovTime->setText(QStringLiteral("%1 / %2")
                          .arg(fmtTime(position), fmtTime(duration)));
}

void WatchPage::onQualityChanged(int index)
{
    const int height = idxToQuality(index);
    NT_DEBUG("E2E qualityChanged index=%d -> %dp auto=%d\n", index, height,
             m_autoQuality ? 1 : 0);
    m_player->setMaxHeight(height);
    if (m_autoQuality) {
        // En Auto el menú no sale de modo: la elección pasa a ser la ÚLTIMA
        // resolución memorizada (quality_auto) y el siguiente vídeo la reusa.
        m_autoHeight = height;
        if (m_settings)
            m_settings->set(QStringLiteral("quality_auto"),
                            QString::number(height));
    } else if (m_settings) {
        // Calidad fija: se persiste como valor explícito (sale de Auto).
        m_settings->set(QStringLiteral("quality"), QString::number(height));
    }
    if (m_player->isActive())
        m_player->reloadCurrent();
}

// Alturas reales del vídeo (adaptiveFormats de Innertube) para el vídeo en
// reproducción. Con Auto el stream va capado a la calidad memorizada (p.ej.
// 360) y el archivo cargado no trae las pistas superiores, así que el techo del
// stream no sirve para el menú; este dato sí refleja lo que el vídeo ofrece.
void WatchPage::onStreamHeights(const QString &videoId,
                                const QVector<int> &heights)
{
    if (heights.isEmpty() || !m_player->isActive())
        return;
    // Solo del vídeo en reproducción: descartar resoluciones atrasadas.
    if (!isCurrentVideo(videoId))
        return;
    int maxH = 0;
    for (int h : heights) {
        if (h > maxH)
            maxH = h;
    }
    if (maxH <= 0)
        return;
    m_availableMaxH = maxH;
    NT_DEBUG("E2E streamHeights video=%s max=%d n=%d\n",
             videoId.toUtf8().constData(), maxH, int(heights.size()));
    // Reaplicar con el techo real del vídeo (no el del stream filtrado).
    applyQualityLimits(m_availableMaxH);
}

// Oculta los ítems del menú de resoluciones por encima del techo real: el del
// vídeo (m_availableMaxH de Innertube) si se conoce; si no, el del archivo
// cargado. Los ítems se ocultan (no se quitan) para preservar los índices de
// idxToQuality(); un valor <= 0 (consulta fallida o FILE_LOADED sin pistas) no
// toca el menú. Para clips sub-360p quedan todos ocultos y el botón muestra la
// altura real (ver onVideoSizeChanged).
void WatchPage::applyQualityLimits(int maxVideoHeight)
{
    if (m_availableMaxH > 0)
        maxVideoHeight = m_availableMaxH;
    if (maxVideoHeight <= 0 || m_qualityItems.isEmpty())
        return;
    NT_DEBUG("E2E qualityLimits apply max=%d items=%d\n",
             maxVideoHeight, int(m_qualityItems.size()));
    for (int i = 0; i < int(m_qualityItems.size()); ++i) {
        QPushButton *b = m_qualityItems.at(i);
        if (!b)
            continue;
        b->setVisible(kQualityHeights[i] <= maxVideoHeight);
    }
    m_qualityMenu->adjustSize();
}

int WatchPage::qualityIndexForHeight(int height) const
{
    int best = 0;
    for (int i = 0; i < kQualityCount; ++i) {
        if (kQualityHeights[i] <= height)
            best = i;
        else
            break;
    }
    return best;
}

void WatchPage::onVideoSizeChanged(int width, int height)
{
    if (height <= 0 || !m_player->isActive())
        return;
    m_videoHeight = height;
    // Calidad pedida (la última guardada por el usuario o al clickear el menú):
    // el botón debe mostrar esa calidad mientras el archivo la tenga.
    const int requested = idxToQuality(m_qualityIndex);
    const int visible = (requested <= height) ? requested : height;
    m_quality->setText(QStringLiteral("%1p").arg(visible));
    // El menú se ancla con m_quality->mapTo(anchor, ...) al abrirse
    // (toggleQualityMenu), así que sigue solo al ancho por contenido: no hace
    // falta reajustar nada aquí al cambiar el texto.
    NT_DEBUG("E2E combo sync %dx%d requested=%d button=%d\n",
             width, height, requested, visible);
}

int WatchPage::idxToQuality(int idx) const
{
    if (idx >= 0 && idx < kQualityCount)
        return kQualityHeights[idx];
    return 720;
}

void WatchPage::setQualityIndex(int index)
{
    m_qualityIndex = index;
    onQualityChanged(index);
    m_quality->setText(QStringLiteral("%1p").arg(idxToQuality(index)));
}

void WatchPage::toggleQualityMenu()
{
    if (m_qualityMenuActive) {
        m_qualityMenu->hide();
        m_qualityMenuActive = false;
        return;
    }
    QWidget *anchor = m_qualityMenuAnchor ? m_qualityMenuAnchor : m_quality;
    const QPoint btnPos = m_quality->mapTo(anchor, QPoint(0, 0));
    m_qualityMenu->move(btnPos.x(), btnPos.y() - m_qualityMenu->height() - 4);
    m_qualityMenu->show();
    m_qualityMenu->raise();
    m_qualityMenuActive = true;
    const QPoint mg = m_qualityMenu->mapToGlobal(QPoint(0, 0));
    NT_DEBUG("E2E menu shown global=(%d,%d) size=%dx%d\n",
             mg.x(), mg.y(), m_qualityMenu->width(),
             m_qualityMenu->height());
    if (QWidget *tw = m_quality->window())
        NT_DEBUG("E2E win-top global=(%d,%d) size=%dx%d\n",
                 tw->mapToGlobal(QPoint(0, 0)).x(),
                 tw->mapToGlobal(QPoint(0, 0)).y(),
                 tw->width(), tw->height());
}

void WatchPage::onPlayPause()
{
    m_player->playPause();
}

void WatchPage::onToggleMute()
{
    m_player->toggleMute();
    // Persistir silencio actual (y volumen por si el overlay lo cambió
    // sin pasar por el slider de la mini bar).
    if (m_settings) {
        m_settings->set(QStringLiteral("volume"),
                        QString::number(int(m_player->volume())));
        m_settings->set(QStringLiteral("volume_muted"),
                        m_player->muted() ? QStringLiteral("1")
                                          : QStringLiteral("0"));
    }
    updateOverlayIcons();
}

void WatchPage::onNextUpActivated(int index, const VideoInfo &)
{
    m_player->playIndex(index);
}

// Fullscreen de SOLO el área de vídeo (+ barra de controles), no de la app
// completa: se ocultan los bloques de información/cola para que el área del
// reproductor llene toda la página; MainWindow se encarga de la ventana.
void WatchPage::toggleFullscreen()
{
    if (m_inFullscreen)
        leaveFullscreen();
    else
        enterFullscreen();
}

void WatchPage::enterFullscreen()
{
    if (m_inFullscreen)
        return;
    m_inFullscreen = true;
    if (m_playerContainer)
        m_playerContainer->setFullscreen(true);
    // Fullscreen de SOLO vídeo: se ocultan botón Volver + descripción + cola.
    // Con todo en el scroll único, ocultarlos deja el contenido = solo vídeo y
    // widgetResizable lo estira a toda la página (sin reparent del player).
    if (m_back)
        m_back->hide();
    if (m_watchBody)
        m_watchBody->hide();
    if (m_bottomWidget)
        m_bottomWidget->hide();
    if (m_topScroll)
        m_topScroll->verticalScrollBar()->setValue(0);
    if (m_overlay->currentIndex() == 0)
        showOverlayControls();
    emit fullscreenToggled(true);
    NT_DEBUG("E2E fullscreen enter\n");
}

void WatchPage::leaveFullscreen()
{
    if (!m_inFullscreen)
        return;
    m_inFullscreen = false;
    if (m_playerContainer)
        m_playerContainer->setFullscreen(false);
    if (m_back)
        m_back->show();
    if (m_watchBody)
        m_watchBody->show();
    if (m_bottomWidget)
        m_bottomWidget->show();
    emit fullscreenToggled(false);
    NT_DEBUG("E2E fullscreen exit\n");
}

// Bucle de eventos para el overlay: cualquier movimiento del ratón (o click o
// scroll) sobre el área de vídeo muestra los controles y reinicia el temporizador.
bool WatchPage::eventFilter(QObject *watched, QEvent *event)
{
    // La descripción re-evalúa su colapso cuando cambia de ancho: así el botón
    // "…más" aparece justo cuando el texto deja de caber y desaparece cuando
    // vuelve a caber.
    if (watched == m_descLabel && event->type() == QEvent::Resize) {
        updateDescCollapse();
        return false;
    }
    if (watched == m_descCard && event->type() == QEvent::Resize) {
        positionDescFade();
        return false;
    }
    // En el mini player el PlayerWidget está reparentado al hueco del mini bar
    // (el filtro sigue instalado en él). Los eventos sobre el vídeo del mini no
    // deben alternar play/pausa ni mostrar el overlay: el mini bar gestiona el
    // click (expandir al Player completo). Sin esta guarda, en modo "Ver vídeo"
    // (miniatura oculta) el click en el vídeo pausaba al expandir.
    if (m_playerDetached)
        return QWidget::eventFilter(watched, event);
    if (event->type() == QEvent::KeyPress) {
        const auto *ke = static_cast<const QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Escape && m_inFullscreen) {
            leaveFullscreen();
            return true;
        }
    }
    switch (event->type()) {
    case QEvent::Wheel:
        showOverlayControls();
        break;
    case QEvent::MouseButtonRelease:
        // Suelta lo que hubiera pulsado en la barra: al soltar, el temporizador de
        // 2,5 s puede volver a ocultar. Sin esto, un clic lento se comía.
        if (m_ctrlPressed && isControlClick(watched)) {
            m_ctrlPressed = false;
            showOverlayControls();
        }
        break;
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        // Ignorar los MouseMove sintéticos que genera el slider de búsqueda al
        // desplazarse bajo un cursor estacionario; solo contar movimiento real.
        const QPoint gp =
            static_cast<const QMouseEvent *>(event)->globalPos();
        if (gp == m_lastMousePos && event->type() == QEvent::MouseMove)
            break;
        m_lastMousePos = gp;
        showOverlayControls();
        // Marca que hay un control pulsado para que hideOverlayControls() no
        // desaparezca la caja a mitad de gesto (el temporador de 2,5 s se agota
        // igual si el usuario mantiene pulsado sin mover: pulsas el volumen, te
        // quedas quieto y la caja se va con el dedo encima).
        if (event->type() == QEvent::MouseButtonPress && isControlClick(watched))
            m_ctrlPressed = true;
        // Un solo click sobre el vídeo alterna play/pausa de forma inmediata.
        // Ojo: un mismo click engancha eventos en dos widgets (PlayerWidget y
        // el QWidget del overlay), así que deduplicamos el mismo click físico
        // por posición en una ventana de 50 ms: si no, saldría pause+play.
        if (event->type() == QEvent::MouseButtonPress
            && isVideoClick(watched)) {
            const qint64 nowMs =
                static_cast<const QMouseEvent *>(event)->timestamp();
            if (nowMs == m_lastVideoPressMs
                && gp == m_lastVideoPressPos)
                break;
            m_lastVideoPressMs = nowMs;
            m_lastVideoPressPos = gp;
            NT_DEBUG("E2E playPause phase=%s\n",
                     m_player->phase().toUtf8().constData());
            m_player->playPause();
        }
        break;
    }
    default:
        break;
    }
    // Deja pasar el evento: el overlay no consume clics del vídeo.
    return QWidget::eventFilter(watched, event);
}

// Considera "click sobre el vídeo" cualquier click que no apunte a la barra de
// controles flotante ni al menú de calidad, con el overlay en la vista normal.
bool WatchPage::isVideoClick(const QObject *watched) const
{
    if (!m_overlay || m_overlay->currentIndex() != 0)
        return false;
    const auto *w = qobject_cast<const QWidget *>(watched);
    if (!w)
        return false;
    if (m_ctrlBar && (w == m_ctrlBar || m_ctrlBar->isAncestorOf(w)))
        return false;
    if (m_qualityMenu && (w == m_qualityMenu || m_qualityMenu->isAncestorOf(w)))
        return false;
    return true;
}

// Considera "click sobre un control" cualquier click que apunte a la barra de
// controles flotante (o al menú de calidad), con el overlay en la vista normal.
// Es la contraparte de isVideoClick(): sirve para no ocultar la caja mientras el
// usuario tiene un control pulsado.
bool WatchPage::isControlClick(const QObject *watched) const
{
    if (!m_overlay || m_overlay->currentIndex() != 0)
        return false;
    const auto *w = qobject_cast<const QWidget *>(watched);
    if (!w)
        return false;
    if (m_ctrlBar && (w == m_ctrlBar || m_ctrlBar->isAncestorOf(w)))
        return true;
    if (m_qualityMenu && (w == m_qualityMenu || m_qualityMenu->isAncestorOf(w)))
        return true;
    return false;
}

void WatchPage::showOverlayControls()
{
    if (!m_ctrlBar)
        return;
    if (m_overlay->currentIndex() != 0)
        return;
    NT_DEBUG("E2E overlayShow idx=%d barVisible=%d\n",
             m_overlay->currentIndex(), m_ctrlBar->isVisible());
    m_ctrlBar->show();
    m_ctrlBar->raise();
    if (m_qualityMenuActive && m_qualityMenu->isVisible())
        m_qualityMenu->raise();
    updateOverlayIcons();
    if (m_overlayTimer)
        m_overlayTimer->start();
}

void WatchPage::hideOverlayControls()
{
    if (!m_ctrlBar)
        return;
    // Si hay un control pulsado, no ocultamos: postponemos. Sin esto, mantener
    // pulsado sin mover (p. ej. sambilando el volumen) agota el temporizador de
    // 2,5 s y la caja desaparece con el dedo encima, Qt suelta el grab y el
    // gesto se pierde a mitad.
    if (m_ctrlPressed) {
        if (m_overlayTimer)
            m_overlayTimer->start();
        return;
    }
    NT_DEBUG("E2E overlayHide idx=%d barVisible=%d\n",
             m_overlay->currentIndex(), m_ctrlBar->isVisible());
    m_ctrlBar->hide();
    // reaches a la guarda de m_ctrlPressed, así que aquí el estado ya no puede
    // estar atascado. Se pone a cero igualmente: si un release se perdiera
    // (el filtro está en el reproductor y sus hijos, pero por si acaso), la
    // caja dejaría de auto-ocultarse para siempre.
    m_ctrlPressed = false;
    if (m_qualityMenuActive) {
        m_qualityMenu->hide();
        m_qualityMenuActive = false;
    }
}

void WatchPage::updateOverlayIcons()
{
    if (!m_ovPlayBtn)
        return;
    m_ovPlayBtn->setIcon(nt::icon(
        m_player->isPaused() ? nt::Icon::Play : nt::Icon::Pause,
        nt::kText, 22));
    m_ovNextBtn->setEnabled(m_player->hasNext());
    m_ovNextBtn->setIcon(nt::icon(nt::Icon::Next, nt::kText, 20));
    if (m_ovPrevBtn) {
        m_ovPrevBtn->setEnabled(m_player->hasPrevious());
        m_ovPrevBtn->setIcon(nt::icon(nt::Icon::Prev, nt::kText, 20));
    }
    m_ovMuteBtn->setIcon(nt::icon(
        m_player->muted() ? nt::Icon::Unmute : nt::Icon::Mute,
        nt::kText, 18));
    m_ovSubsBtn->setIcon(nt::icon(
        nt::Icon::Subtitles,
        m_subsVisible ? QColor(nt::accentHex()) : nt::kText, 18));
    QSignalBlocker block(m_ovVolume);
    m_ovVolume->setValue(qRound(m_player->volume()));
}

void WatchPage::refreshVolumeSlider()
{
    if (!m_ovVolume)
        return;
    QSignalBlocker block(m_ovVolume);
    // Aplica el rango nuevo (puede haber cambiado por el ajuste de Ajustes).
    m_ovVolume->setRange(0, m_player->volumeMax());
    // El volumen actual puede haber quedado capeado por el nuevo tope.
    m_ovVolume->setValue(qRound(m_player->volume()));
}

void WatchPage::onVolumeChanged()
{
    if (!m_ovVolume)
        return;
    // Sincronización en caliente del slider tras cualquier cambio de volumen
    // (teclado de MainWindow, botón de silencio, slider, ajuste de volumen
    // máximo). QSignalBlocker: el setValue programático NO debe re-disparar
    // el valueChanged (que persistiría y re-aplicaría el volumen).
    QSignalBlocker block(m_ovVolume);
    m_ovVolume->setValue(qRound(m_player->volume()));
    m_ovMuteBtn->setIcon(nt::icon(
        m_player->muted() ? nt::Icon::Unmute : nt::Icon::Mute,
        nt::kText, 18));
}

void WatchPage::applyAccent()
{
    // Botón Suscribirse: repinta con el estado (suscrito / acento vivo) y el
    // color de acento vigente.
    if (m_subscribeBtn)
        updateSubscribeButton(
            m_subscribeBtn->property("subscribed").toBool());
    // Acciones (like/dislike/estrella/ver después): el icono y el texto del
    // estado activo usan el acento.
    updateActionButtons();
    // Descripción: los enlaces se pintaron con el color de acento anterior;
    // se regenera el texto enriquecido (siempre a altura completa).
    if (!m_description.isEmpty())
        applyDescription(m_description);
    // Overlay: el icono de subtítulos activo usa el acento.
    updateOverlayIcons();
}