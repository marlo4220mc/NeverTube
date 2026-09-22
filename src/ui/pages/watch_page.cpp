#include "watch_page.h"

#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QEvent>
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
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

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

// Resoluciones del menú del reproductor, de menor a mayor. Fuente ÚNICA de
// verdad: los índices del menú (m_qualityItems), idxToQuality,
// qualityIndexForHeight y la altura pedida a Yt salen de aquí. Debe coincidir
// con las opciones de calidad de Ajustes (settings_page.cpp).
namespace {
const int kQualityHeights[] = {144, 240, 360, 480, 720, 1080, 1440, 2160};
constexpr int kQualityCount =
    static_cast<int>(sizeof(kQualityHeights) / sizeof(kQualityHeights[0]));
} // namespace

class PlayerContainer : public QWidget
{
public:
    PlayerContainer(PlayerWidget *player, QWidget *overlay, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_overlay(overlay)
        , m_player(player)
    {
        setStyleSheet(QStringLiteral("background:#000;"));
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
        const int h = qRound(width() / m_aspect);
        NT_DEBUG("E2E pc aspect=%.3f width=%d maxH=%d curMax=%d\n",
                 m_aspect, width(), h, maximumHeight());
        if (h == maximumHeight())
            return;
        setMaximumHeight(h);
    }

    QWidget *m_overlay;
    PlayerWidget *m_player;
    QVBoxLayout *m_lay = nullptr;
    double m_aspect = 0.0;
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
    m_overlay->setStyleSheet(QStringLiteral("background: transparent;"));

    auto *gl = new QWidget;
    gl->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *gl1 = new QVBoxLayout(gl);
    gl1->setContentsMargins(0, 0, 0, 0);
    gl1->setSpacing(0);
    gl1->addStretch(1);

    m_ctrlBar = new QWidget(gl);
    m_ctrlBar->setObjectName(QStringLiteral("playerOverlay"));
    m_ctrlBar->setAttribute(Qt::WA_StyledBackground, true);
    m_ctrlBar->setStyleSheet(
        QStringLiteral("QWidget#playerOverlay { background-color: rgba(15,15,15,165);"
                       " border:1px solid #2A2A2A; border-radius:8px; }"));
    auto *barLay = new QVBoxLayout(m_ctrlBar);
    barLay->setContentsMargins(10, 4, 10, 6);
    barLay->setSpacing(2);

    auto *seekRow = new QHBoxLayout;
    seekRow->setSpacing(8);
    m_ovSeek = new SeekSlider(Qt::Horizontal, m_ctrlBar);
    m_ovSeek->setCursor(Qt::PointingHandCursor);
    connect(m_ovSeek, &QSlider::sliderMoved, this,
            [this](int value) { m_player->seek(value); });
    seekRow->addWidget(m_ovSeek, 1);
    m_ovTime = new QLabel(QStringLiteral("0:00 / 0:00"), m_ctrlBar);
    m_ovTime->setStyleSheet(QStringLiteral("color:#ECECEC; font-size:11px;"));
    seekRow->addWidget(m_ovTime);
    barLay->addLayout(seekRow);

    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(6);

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
    m_quality->setObjectName(QStringLiteral("iconBtn"));
    m_quality->setFixedWidth(98);
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

    m_qualityMenu = new QFrame(gl);
    m_qualityMenu->setObjectName(QStringLiteral("qualityMenu"));
    m_qualityMenu->setAttribute(Qt::WA_StyledBackground, true);
    m_qualityMenu->setStyleSheet(
        QStringLiteral("QFrame#qualityMenu { background-color:#1F1F1F;"
                       " border:1px solid #2D2D2D; border-radius:6px; }"
                       "QPushButton { background:transparent; color:#ECECEC;"
                       " border:none; text-align:left; padding:6px 8px;"
                       " border-radius:4px; }"
                       "QPushButton:hover { background:#2D2D2D; }"));
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
    spinner->setStyleSheet(
        QStringLiteral("background: transparent; color: #8F8F8F; font-size: 13px;"));
    m_overlay->addWidget(spinner);

    m_errorLabel = new QLabel(this);
    m_errorLabel->setAlignment(Qt::AlignCenter);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet(
        QStringLiteral("background: transparent; color: #C8C8C8; "
                       "font-size: 14px; padding: 16px;"));
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
    splitOuter->addWidget(pc, 3);
    enableOverlayTracking(this, pc);
    connect(playerWidget, &PlayerWidget::videoAspectChanged, pc,
            &PlayerContainer::setVideoAspect);
    // Sin marco de foco tipo botón al hacer click sobre el vídeo.
    pc->setFocusPolicy(Qt::NoFocus);
    playerWidget->setFocusPolicy(Qt::NoFocus);
    m_overlay->setFocusPolicy(Qt::NoFocus);
    gl->setFocusPolicy(Qt::NoFocus);

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

    auto *backRow = new QHBoxLayout;
    backRow->setContentsMargins(12, 8, 12, 0);
    m_back = new QPushButton(QStringLiteral("◀ Volver"), this);
    m_back->setObjectName(QStringLiteral("ghost"));
    m_back->setIcon(nt::icon(nt::Icon::Back, nt::kText, 14));
    backRow->addWidget(m_back);
    backRow->addStretch(1);
    top->addLayout(backRow);

    auto *infoWidget = new QWidget;
    infoWidget->setObjectName(QStringLiteral("watchBody"));
    auto *infoLay = new QVBoxLayout(infoWidget);
    infoLay->setContentsMargins(16, 14, 16, 8);
    infoLay->setSpacing(6);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("sectionTitle"));
    m_title->setWordWrap(true);
    infoLay->addWidget(m_title);

    m_meta = new QLabel(this);
    m_meta->setStyleSheet(QStringLiteral("color: #9aa2ad; font-size: 12px;"));
    infoLay->addWidget(m_meta);

    m_channelBtn = new QPushButton(QStringLiteral("Canal"), this);
    m_channelBtn->setObjectName(QStringLiteral("ghost"));
    m_channelBtn->setCursor(Qt::PointingHandCursor);
    m_channelBtn->setIconSize(QSize(43, 40));
    m_channelBtn->setIcon(nt::icon(nt::Icon::User, nt::kMuted, 20));
    m_channelBtn->setStyleSheet(
        QStringLiteral("QPushButton { background:transparent; border:none;"
                       " border-radius:8px; text-align:left; padding:8px;"
                       " color:#ECECEC; font-weight:600; font-size:13px; }"
                       "QPushButton:hover { background:#1F1F1F; }"));
    m_channelBtn->setMinimumHeight(56);
    m_channelBtn->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);

    m_subscribeBtn = new QPushButton(QStringLiteral("Suscribirse"), this);
    m_subscribeBtn->setCursor(Qt::PointingHandCursor);
    m_subscribeBtn->setMinimumHeight(36);
    m_subscribeBtn->setStyleSheet(
        QStringLiteral("QPushButton { background:#FF0033;"
                       " border-radius:18px; padding:0 18px;"
                       " color:white; font-weight:600; font-size:13px; }"
                       "QPushButton:hover { background:#FF1A47; }"));

    auto *channelRow = new QHBoxLayout;
    channelRow->setSpacing(8);
    channelRow->addWidget(m_channelBtn);
    channelRow->addStretch(1);
    channelRow->addWidget(m_subscribeBtn, 0, Qt::AlignVCenter);
    infoLay->addLayout(channelRow);

    auto *actionsRow = new FlowLayout(0, 8, 8);
    auto makeAction = [this](nt::Icon icon, const QString &text,
                             QPushButton **out, nt::Icon *iconOut)
        -> QPushButton * {
        auto *b = new QPushButton(text, this);
        b->setObjectName(QStringLiteral("ghost"));
        b->setCursor(Qt::PointingHandCursor);
        b->setIcon(nt::icon(icon, nt::kMuted, 20));
        b->setIconSize(QSize(20, 20));
        *out = b;
        *iconOut = icon;
        return b;
    };
    actionsRow->addWidget(makeAction(nt::Icon::Thumb, QStringLiteral("Me gusta"),
                                   &m_likeBtn, &m_likeIcon));
    actionsRow->addWidget(makeAction(nt::Icon::ThumbDown,
                                   QStringLiteral("No me gusta"),
                                   &m_dislikeBtn, &m_dislikeIcon));
    actionsRow->addWidget(makeAction(nt::Icon::Share,
                                   QStringLiteral("Compartir"),
                                   &m_shareBtn, &m_shareIcon));
    actionsRow->addWidget(makeAction(nt::Icon::Star,
                                   QStringLiteral("Favorito"),
                                   &m_starBtn, &m_starIcon));
    actionsRow->addWidget(makeAction(nt::Icon::WatchLater,
                                   QStringLiteral("Ver más tarde"),
                                   &m_watchLaterBtn, &m_watchLaterIcon));
    actionsRow->addWidget(makeAction(nt::Icon::ListAdd,
                                   QStringLiteral("Agregar a playlist"),
                                   &m_playlistBtn, &m_playlistIcon));
    connect(m_playlistBtn, &QPushButton::clicked, this,
            &WatchPage::showPlaylistMenu);
    infoLay->addLayout(actionsRow);

    m_descLabel = new QLabel(this);
    m_descLabel->setWordWrap(true);
    m_descLabel->setTextFormat(Qt::RichText);
    m_descLabel->setTextInteractionFlags(Qt::LinksAccessibleByMouse
                                         | Qt::TextSelectableByMouse);
    m_descLabel->setOpenExternalLinks(true);
    m_descLabel->setStyleSheet(
        QStringLiteral("color:#9aa2ad; font-size:12px; background:transparent;"));
    m_descLabel->setMaximumHeight(60);
    m_descToggle = new QPushButton(this);
    m_descToggle->setObjectName(QStringLiteral("ghost"));
    m_descToggle->setFixedWidth(100);
    m_descToggle->setCursor(Qt::PointingHandCursor);
    m_descToggle->hide();
    auto *descRow = new QHBoxLayout;
    descRow->setSpacing(8);
    descRow->addWidget(m_descLabel, 1);
    descRow->addWidget(m_descToggle, 0, Qt::AlignTop);
    connect(m_descToggle, &QPushButton::clicked, this, [this]() {
        m_descExpanded = !m_descExpanded;
        m_descLabel->setMaximumHeight(m_descExpanded ? 16777215 : 60);
        m_descToggle->setText(m_descExpanded
                                  ? QStringLiteral("Ver menos")
                                  : QStringLiteral("Ver más"));
    });
    infoLay->addLayout(descRow);
    top->addWidget(infoWidget);
    top->addStretch(1);

    auto *bottomWidget = new QWidget;
    m_bottomWidget = bottomWidget;
    auto *bot = new QVBoxLayout(bottomWidget);
    bot->setContentsMargins(0, 8, 16, 8);
    bot->setSpacing(6);
    auto *quTitle = new QLabel(QStringLiteral("A continuación"), this);
    quTitle->setStyleSheet(
        QStringLiteral("font-weight:600; font-size:13px; color:#ECECEC;"));
    m_nextUpList = new VideoListView(m_thumbs, this);
    m_nextUpList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_nextUpList->setSizePolicy(QSizePolicy::Expanding,
                                QSizePolicy::Expanding);
    m_nextUpList->setMinimumHeight(90);
    bot->addWidget(quTitle);
    bot->addWidget(m_nextUpList);

    auto *belowRow = new QWidget(this);
    m_belowWidget = belowRow;
    auto *row = new QHBoxLayout(belowRow);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addWidget(topScroll, 7);
    row->addWidget(bottomWidget, 3);
    splitOuter->addWidget(belowRow, 1);

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
    m_descToggle->hide();
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
    m_descExpanded = false;
    m_descLabel->setMaximumHeight(60);
    m_descToggle->hide();
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
    if (subscribed) {
        m_subscribeBtn->setProperty("subscribed", true);
        m_subscribeBtn->setStyleSheet(
            QStringLiteral("QPushButton { background:#262626;"
                           " border-radius:18px; padding:0 18px;"
                           " color:#ECECEC; font-weight:600; font-size:13px; }"
                           "QPushButton:hover { background:#333333; }"));
    } else {
        m_subscribeBtn->setProperty("subscribed", false);
        m_subscribeBtn->setStyleSheet(
            QStringLiteral("QPushButton { background:#FF0033;"
                           " border-radius:18px; padding:0 18px;"
                           " color:white; font-weight:600; font-size:13px; }"
                           "QPushButton:hover { background:#FF1A47; }"));
    }
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
        b->setIcon(nt::icon(icon, active ? QColor(QStringLiteral("#FF0033"))
                                         : nt::kMuted, 20));
        b->setIconSize(QSize(20, 20));
        if (active)
            b->setStyleSheet(
                QStringLiteral("QPushButton { background:#262626;"
                               " border-radius:16px; padding:7px 14px;"
                               " color:#FF0033; font-weight:600; font-size:13px; }"
                               "QPushButton:hover { background:#333333; }"));
        else
            b->setStyleSheet(
                QStringLiteral("QPushButton { background:#1F1F1F;"
                               " border-radius:16px; padding:7px 14px;"
                               " color:#ECECEC; font-size:13px; }"
                               "QPushButton:hover { background:#2A2A2A; }"));
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
        const QString label = contains
            ? QStringLiteral("✔ %1 (ya está)").arg(p.title)
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

void WatchPage::onDetailFailed(const QString &videoId)
{
    if (!m_player->isActive() || videoId != m_player->current().id)
        return;
    // Ningún cliente devolvió metadatos; no dejar "Cargando detalles…"
    // clavado. Se muestra lo que ya se conoce del elemento de la cola.
    m_fetchingDetail = false;
    m_description.clear();
    m_descLabel->hide();
    m_descToggle->hide();
    updateInfo();
}

void WatchPage::onDetailReady(const VideoInfo &video, const QString &desc,
                              qint64 likes, const QVector<FormatInfo> &formats)
{
    Q_UNUSED(formats);
    if (!m_player->isActive() || video.id != m_player->current().id)
        return;
    m_player->applyCurrentDetail(video);
    m_title->setText(m_player->current().title);
    updateChannelRow();
    m_fetchingDetail = false;
    m_description = desc;
    m_likeCount = likes;
    if (!desc.isEmpty()) {
        QString rich = desc.toHtmlEscaped();
        rich.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        const QRegularExpression urlRe(QStringLiteral(
            R"((https?://[^\s<]+))"));
        rich.replace(urlRe, QStringLiteral(
            "<a href=\"\\1\" style=\"color:#FF0033;\">\\1</a>"));
        m_descLabel->setText(QStringLiteral(
            "<style>a { text-decoration:none; }</style>") + rich);
        m_descLabel->setMaximumHeight(60);
        m_descExpanded = false;
        m_descToggle->setText(QStringLiteral("Ver más"));
        m_descLabel->show();
        m_descToggle->show();
    } else {
        m_descLabel->hide();
        m_descToggle->hide();
    }
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
    m_cache->upsert(video);
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
    if (!m_player->isActive() || videoId != m_player->current().id)
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
    if (!description.isEmpty()) {
        m_description = description;
        QString rich = description.toHtmlEscaped();
        rich.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        const QRegularExpression urlRe(QStringLiteral(
            R"((https?://[^\s<]+))"));
        rich.replace(urlRe, QStringLiteral(
            "<a href=\"\\1\" style=\"color:#FF0033;\">\\1</a>"));
        m_descLabel->setText(QStringLiteral(
            "<style>a { text-decoration:none; }</style>") + rich);
        m_descLabel->setMaximumHeight(60);
        m_descExpanded = false;
        m_descToggle->setText(QStringLiteral("Ver más"));
        m_descLabel->show();
        m_descToggle->show();
    }
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
    if (m_player->current().id != videoId)
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
    if (m_belowWidget)
        m_belowWidget->hide();
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
    if (m_belowWidget)
        m_belowWidget->show();
    emit fullscreenToggled(false);
    NT_DEBUG("E2E fullscreen exit\n");
}

// Bucle de eventos para el overlay: cualquier movimiento del ratón (o click o
// scroll) sobre el área de vídeo muestra los controles y reinicia el temporizador.
bool WatchPage::eventFilter(QObject *watched, QEvent *event)
{
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
    NT_DEBUG("E2E overlayHide idx=%d barVisible=%d\n",
             m_overlay->currentIndex(), m_ctrlBar->isVisible());
    m_ctrlBar->hide();
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
    m_ovMuteBtn->setIcon(nt::icon(
        m_player->muted() ? nt::Icon::Unmute : nt::Icon::Mute,
        nt::kText, 18));
    m_ovSubsBtn->setIcon(nt::icon(
        nt::Icon::Subtitles,
        m_subsVisible ? QColor("#FF0033") : nt::kText, 18));
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