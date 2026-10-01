#include "library_page.h"

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QRandomGenerator>
#include <QTimer>
#include <QToolTip>
#include <QCursor>
#include <QFrame>
#include <QFontMetricsF>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include <functional>

#include "../../db/repositories.h"
#include "../../yt.h"
#include "../channel_row.h"
#include "../confirm_dialog.h"
#include "../format.h"
#include "../icons.h"
#include "../row_menu.h"
#include "../theme.h"
#include "../thumbnail_cache.h"

namespace {

constexpr int kVisibleRows = 3;  // filas por defecto antes de "Ver todo"
constexpr int kThumbW = 236;
constexpr int kThumbH = 133;
constexpr int kCardMinW = 400;   // ancho mínimo de cada tarjeta (grid)
constexpr int kMaxCardCols = 4;

// Recorta manteniendo el centro y aplica esquinas redondeadas.
QPixmap coverCrop(const QPixmap &pm, const QSize &sz, int radius)
{
    QPixmap scaled =
        pm.scaled(sz, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    const int x = (scaled.width() - sz.width()) / 2;
    const int y = (scaled.height() - sz.height()) / 2;
    scaled = scaled.copy(x, y, sz.width(), sz.height());

    QPixmap out(sz);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(0, 0, sz.width(), sz.height(), radius, radius);
    p.setClipPath(clip);
    p.drawPixmap(0, 0, scaled);
    return out;
}

QPixmap roundedSolid(const QSize &sz, int radius, const QPixmap &glyph)
{
    QPixmap out(sz);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(0, 0, sz.width(), sz.height(), radius, radius);
    p.setClipPath(clip);
    p.fillRect(0, 0, sz.width(), sz.height(), nt::kElevated);
    const int s = 28;
    p.drawPixmap((sz.width() - s) / 2, (sz.height() - s) / 2, glyph);
    return out;
}

// Miniatura o avatar que se rellena solo desde ThumbnailCache.
class AsyncThumb : public QLabel
{
public:
    AsyncThumb(ThumbnailCache *thumbs, const QString &channelId,
               const QString &cacheKey, const QString &url, int w, int h,
               int radius, bool avatar, QWidget *parent = nullptr)
        : QLabel(parent)
        , m_thumbs(thumbs)
        , m_key(cacheKey)
        , m_channelId(channelId)
        , m_url(url)
        , m_w(w)
        , m_h(h)
        , m_r(radius)
        , m_avatar(avatar)
    {
        setFixedSize(w, h);
        setCursor(Qt::PointingHandCursor);
        const QPixmap cached = thumbs->pixmap(m_key);
        if (!cached.isNull()) {
            apply(cached);
        } else {
            setPlaceholder();
            if (m_url.isEmpty())
                return;
            connect(thumbs, &ThumbnailCache::ready, this,
                    [this](const QString &key) {
                        if (key != m_key)
                            return;
                        const QPixmap pm = m_thumbs->pixmap(m_key);
                        if (!pm.isNull())
                            apply(pm);
                    });
            if (avatar)
                thumbs->requestChannelAvatar(m_channelId, m_url);
            else
                thumbs->request(m_key, m_url);
        }
    }

    // Fracción de reproducción (0..1) que se pinta al pie de la miniatura, el
    // mismo "estado de reproducción" que dibuja el delegado de VideoListView
    // en el Home y la página de Historial. 0 = sin barra.
    void setProgress(double fraction)
    {
        const double f = qBound(0.0, fraction, 1.0);
        if (qFuzzyCompare(m_frac + 1.0, f + 1.0))
            return;
        m_frac = f;
        update();
    }

protected:
    void paintEvent(QPaintEvent *e) override
    {
        QLabel::paintEvent(e);
        if (m_frac <= 0.0)
            return;
        // Mismas medidas que VideoListView::drawProgressBar (2 px de margen,
        // 3 px de alto, 1 px por encima del borde) para que la barra se lea
        // igual en todos los sitios. Se recorta al rectángulo redondeado de la
        // miniatura para no asomar por las esquinas.
        const QRectF bar(2, height() - 4, width() - 4, 3);
        if (bar.width() <= 0)
            return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()), m_avatar ? m_h / 2 : m_r,
                            m_avatar ? m_h / 2 : m_r);
        p.setClipPath(clip);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 170));
        p.drawRoundedRect(bar, 1.5, 1.5);
        p.setBrush(nt::kAccent);
        p.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * m_frac,
                                  bar.height()),
                          1.5, 1.5);
    }

private:
    void apply(const QPixmap &pm)
    {
        setPixmap(
            coverCrop(pm, QSize(m_w, m_h), m_avatar ? m_h / 2 : m_r));
    }

    void setPlaceholder()
    {
        const QPixmap glyph =
            (m_avatar ? nt::icon(nt::Icon::User, nt::kMuted, 20)
                      : nt::icon(nt::Icon::Play, nt::kMuted, 24))
                .pixmap(28, 28);
        setPixmap(roundedSolid(QSize(m_w, m_h), m_r, glyph));
    }

    ThumbnailCache *m_thumbs;
    QString m_key;
    QString m_channelId;
    QString m_url;
    int m_w;
    int m_h;
    int m_r;
    bool m_avatar;
    double m_frac = 0.0;
};

// Fila genérica: miniatura/avatar a la izquierda, texto y botón de acción.
class RowItem : public QWidget
{
public:
    explicit RowItem(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
        m_lay = new QHBoxLayout(this);
        m_lay->setContentsMargins(6, 6, 6, 6);
        m_lay->setSpacing(14);

        m_textLay = new QVBoxLayout;
        m_textLay->setContentsMargins(0, 0, 0, 0);
        m_textLay->setSpacing(2);
        m_lay->addLayout(m_textLay, 1);
        m_textLay->addStretch(1);
        m_textLay->addStretch(1);
    }

    void setLeading(QWidget *w) { m_lay->insertWidget(0, w); }

    void setTextSpacing(int s) { m_textLay->setSpacing(s); }

    QLabel *metaLabel() const { return m_meta; }

    void setTitleLines(int lines)
    {
        if (!m_title)
            return;
        const QFontMetricsF fm(m_title->font());
        m_title->setFixedHeight(lines <= 1 ? qRound(fm.height() + 1)
                                           : qRound(fm.height() * lines + 3));
    }

    void setTitle(const QString &title)
    {
        if (!m_title) {
            m_title = new QLabel(this);
            m_title->setWordWrap(true);
            m_title->setObjectName(QStringLiteral("libraryItemTitle"));
            const QFontMetricsF fm(m_title->font());
            m_title->setFixedHeight(qRound(fm.height() * 2.0 + 3));
            m_textLay->insertWidget(1, m_title);
        }
        m_title->setText(title);
    }

    void setMeta(const QString &meta)
    {
        if (!m_meta) {
            m_meta = new QLabel(this);
            m_meta->setWordWrap(true);
            m_meta->setObjectName(QStringLiteral("libraryItemMeta"));
            m_textLay->insertWidget(m_textLay->count() - 1, m_meta);
        }
        m_meta->setText(meta);
    }

    void setAction(QWidget *btn) { m_lay->addWidget(btn, 0, Qt::AlignVCenter); }

    void setActivate(std::function<void()> fn) { m_activate = std::move(fn); }

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
            if (m_activate)
                m_activate();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void enterEvent(QEvent *event) override
    {
        Q_UNUSED(event);
        setHover(true);
    }
    void leaveEvent(QEvent *event) override
    {
        Q_UNUSED(event);
        setHover(false);
    }

private:
    void setHover(bool on)
    {
        setAutoFillBackground(on);
        QPalette pal = palette();
        pal.setColor(QPalette::Window, on ? nt::kHover : Qt::transparent);
        setPalette(pal);
    }

    QHBoxLayout *m_lay;
    QVBoxLayout *m_textLay;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    std::function<void()> m_activate;
};

class ThreeDotBtn : public QWidget
{
public:
    explicit ThreeDotBtn(QMenu *menu, QWidget *parent = nullptr)
        : QWidget(parent), m_menu(menu)
    {
        setFixedSize(26, 26);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QStringLiteral("Opciones"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor col(m_hover ? "#ECECEC" : "#8F8F8F");
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        const int cx = width() / 2;
        const int y0 = (height() - 12) / 2;
        for (int i = 0; i < 3; ++i)
            p.drawEllipse(cx - 2, y0 + i * 5, 4, 4);
    }

    void enterEvent(QEvent *) override
    {
        m_hover = true;
        update();
    }
    void leaveEvent(QEvent *) override
    {
        m_hover = false;
        update();
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && m_menu) {
            m_menu->popup(mapToGlobal(rect().bottomLeft()));
        }
    }

private:
    QMenu *m_menu;
    bool m_hover = false;
};

QPushButton *makeIconBtn(const QString &toolTip, bool more, QWidget *parent)
{
    auto *btn = new QPushButton(QString(), parent);
    btn->setObjectName(QStringLiteral("iconBtn"));
    btn->setCursor(Qt::PointingHandCursor);
    if (more) {
        btn->setFixedSize(34, 34);
        QFont f = btn->font();
        f.setPointSizeF(12);
        f.setBold(true);
        btn->setFont(f);
    } else {
        btn->setFixedSize(28, 28);
        btn->setIcon(nt::icon(nt::Icon::Clear, nt::kMuted, 14));
        btn->setIconSize(QSize(14, 14));
    }
    if (!toolTip.isEmpty())
        btn->setToolTip(toolTip);
    return btn;
}

ThreeDotBtn *makeMoreButton(QMenu *menu, QWidget *parent)
{
    auto *btn = new ThreeDotBtn(menu, parent);
    return btn;
}

void addMenuAction(QMenu *menu, const QString &text, std::function<void()> fn)
{
    QAction *act = menu->addAction(text);
    QObject::connect(act, &QAction::triggered, menu, [fn]() {
        QTimer::singleShot(0, fn);
    });
}

void clearRows(QLayout *lay)
{
    while (QLayoutItem *item = lay->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

} // namespace

LibraryPage::LibraryPage(Yt *yt, FavoritesRepo *favorites, StarredRepo *starred,
                         WatchLaterRepo *watchLater, HistoryRepo *history,
                         SubscriptionsRepo *subs, VideoCacheRepo *cache,
                         ThumbnailCache *thumbs, PlaylistsRepo *playlists,
                         QWidget *parent)
    : QWidget(parent)
    , m_yt(yt)
    , m_favs(favorites)
    , m_starred(starred)
    , m_watchLater(watchLater)
    , m_history(history)
    , m_subs(subs)
    , m_cache(cache)
    , m_thumbs(thumbs)
    , m_playlists(playlists)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_body = new QWidget;
    m_scroll->setWidget(m_body);

    auto *lay = new QVBoxLayout(m_body);
    lay->setContentsMargins(20, 18, 20, 32);
    lay->setSpacing(8);

    auto *title = new QLabel(QStringLiteral("Biblioteca"), m_body);
    title->setObjectName(QStringLiteral("sectionTitle"));
    lay->addWidget(title);
    lay->addSpacing(12);

    const auto addSectionHeader = [this, lay](const QString &t, QPushButton **btn,
                                              const QString &btnText,
                                              QLabel **titleOut = nullptr) {
        auto *header = new QHBoxLayout;
        auto *l = new QLabel(t, m_body);
        l->setObjectName(QStringLiteral("sectionTitle"));
        l->setProperty("baseTitle", t);
        header->addWidget(l);
        header->addStretch(1);
        auto *b = new QPushButton(btnText, m_body);
        // Enlace discreto y no #ghost con marco: son cinco botones (Administrar
        // + tres "Ver todo") alineados a la derecha de cada titulo, y con
        // capsula parecian botones de la pagina en vez de enlaces.
        b->setObjectName(QStringLiteral("sectionLink"));
        b->setCursor(Qt::PointingHandCursor);
        header->addWidget(b);
        if (btn)
            *btn = b;
        if (titleOut)
            *titleOut = l;
        lay->addLayout(header);
    };

    // ---- Suscripciones -------------------------------------------------
    QPushButton *manageBtn = nullptr;
    addSectionHeader(QStringLiteral("Suscripciones"), &manageBtn,
                     QStringLiteral("Administrar"), &m_subsTitle);
    connect(manageBtn, &QPushButton::clicked, this, &LibraryPage::openManageSubs);

    m_channelHint = new QLabel(
        QStringLiteral("Aún no te has suscrito a ningún canal. Abre un vídeo "
                       "o un canal y pulsa «Suscribirse»."),
        m_body);
    m_channelHint->setObjectName(QStringLiteral("emptyState"));
    m_channelHint->setWordWrap(true);
    m_channelHint->setAlignment(Qt::AlignCenter);
    lay->addWidget(m_channelHint);

    m_subHost = new QWidget(m_body);
    m_subRowLay = new QHBoxLayout(m_subHost);
    m_subRowLay->setContentsMargins(0, 4, 0, 0);
    m_subRowLay->setSpacing(6);
    m_subRowLay->setAlignment(Qt::AlignVCenter);
    m_subHost->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    m_subScroll = new QScrollArea(m_body);
    m_subScroll->setWidgetResizable(false);
    m_subScroll->setFrameShape(QFrame::NoFrame);
    m_subScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // El scrollbar horizontal está dentro del área y consume altura del
    // viewport: sin esto, veríamos ~37px de tarjetas de 54px y el logo del
    // canal quedará recortado por abajo. El mínimo debe incluir esa franja.
    m_subScroll->setMinimumHeight(
        kChannelRowHeight + m_subScroll->horizontalScrollBar()->sizeHint().height());
    m_subScroll->setWidget(m_subHost);
    lay->addWidget(m_subScroll);

    lay->addSpacing(16);

    // ---- Videos que me gustan ------------------------------------------
    addSectionHeader(QStringLiteral("Videos que me gustan"), &m_fav.allBtn,
                     QStringLiteral("Ver todo"), &m_fav.title);
    connect(m_fav.allBtn, &QPushButton::clicked, this, [this]() {
        m_fav.showAll = !m_fav.showAll;
        rebuildFavorites();
    });
    m_fav.host = new QWidget(m_body);
    m_fav.lay = new QVBoxLayout(m_fav.host);
    m_fav.lay->setContentsMargins(0, 4, 0, 0);
    m_fav.lay->setSpacing(2);
    lay->addWidget(m_fav.host);
    m_fav.empty = new QLabel(
        QStringLiteral("Pulsa «Me gusta» en un vídeo y aparecerá aquí."),
        m_body);
    m_fav.empty->setObjectName(QStringLiteral("emptyState"));
    m_fav.empty->setWordWrap(true);
    m_fav.empty->setAlignment(Qt::AlignCenter);
    lay->addWidget(m_fav.empty);

    lay->addSpacing(16);

    // ---- Ver más tarde -------------------------------------------------
    addSectionHeader(QStringLiteral("Ver más tarde"), &m_later.allBtn,
                     QStringLiteral("Ver todo"), &m_later.title);
    connect(m_later.allBtn, &QPushButton::clicked, this, [this]() {
        m_later.showAll = !m_later.showAll;
        rebuildWatchLater();
    });
    m_later.host = new QWidget(m_body);
    m_later.lay = new QVBoxLayout(m_later.host);
    m_later.lay->setContentsMargins(0, 4, 0, 0);
    m_later.lay->setSpacing(2);
    lay->addWidget(m_later.host);
    m_later.empty = new QLabel(
        QStringLiteral("Pulsa el reloj en un vídeo para guardarlo aquí."),
        m_body);
    m_later.empty->setObjectName(QStringLiteral("emptyState"));
    m_later.empty->setWordWrap(true);
    m_later.empty->setAlignment(Qt::AlignCenter);
    lay->addWidget(m_later.empty);

    lay->addSpacing(16);

    // ---- Historial -----------------------------------------------------
    addSectionHeader(QStringLiteral("Historial"), &m_historySec.allBtn,
                     QStringLiteral("Ver todo"), &m_historySec.title);
    connect(m_historySec.allBtn, &QPushButton::clicked, this,
            &LibraryPage::historyAllRequested);
    m_historySec.host = new QWidget(m_body);
    m_historySec.lay = new QVBoxLayout(m_historySec.host);
    m_historySec.lay->setContentsMargins(0, 4, 0, 0);
    m_historySec.lay->setSpacing(2);
    lay->addWidget(m_historySec.host);
    m_historySec.empty = new QLabel(
        QStringLiteral("Los vídeos que veas aparecerán aquí."), m_body);
    m_historySec.empty->setObjectName(QStringLiteral("emptyState"));
    m_historySec.empty->setWordWrap(true);
    m_historySec.empty->setAlignment(Qt::AlignCenter);
    lay->addWidget(m_historySec.empty);

    lay->addSpacing(16);

    // ---- Playlists -----------------------------------------------------
    QPushButton *newBtn = nullptr;
    addSectionHeader(QStringLiteral("Playlists"), &newBtn,
                     QStringLiteral("+ Nueva"), &m_playlistsTitle);
    connect(newBtn, &QPushButton::clicked, this, &LibraryPage::createPlaylist);

    auto *playlistHost = new QWidget(m_body);
    m_playlistGrid = new QGridLayout(playlistHost);
    m_playlistGrid->setContentsMargins(0, 4, 0, 0);
    m_playlistGrid->setHorizontalSpacing(12);
    m_playlistGrid->setVerticalSpacing(6);
    lay->addWidget(playlistHost);
    m_playlistEmpty = new QLabel(
        QStringLiteral("Crea tu primera playlist con «+ Nueva»."), m_body);
    m_playlistEmpty->setObjectName(QStringLiteral("emptyState"));
    m_playlistEmpty->setWordWrap(true);
    m_playlistEmpty->setAlignment(Qt::AlignCenter);
    lay->addWidget(m_playlistEmpty);

    lay->addStretch(1);

    outer->addWidget(m_scroll);

    // "Crear / renombrar playlist" ya no necesita setup: es nt::inputText, que
    // se construye en cada llamada. "Gestionar suscripciones" sí sigue siendo un
    // popover propio (una lista con scroll, no un diálogo).
    setupManageSubsPopup();

    connect(m_yt, &Yt::channelInfoReady, this, &LibraryPage::onChannelInfo);

    reload();
}

void LibraryPage::reload()
{
    rebuildSubscriptions();
    rebuildFavorites();
    rebuildWatchLater();
    rebuildHistory();
    rebuildPlaylists();
}

void LibraryPage::applyAccent()
{
    // La barra de progreso de la miniatura la pinta AsyncThumb::paintEvent con
    // nt::kAccent, que el QSS no alcanza: hay que repintar las miniaturas.
    for (AsyncThumb *t : m_body->findChildren<AsyncThumb *>())
        t->update();
}

// --------------------------------------------------------------------------
// Suscripciones
// --------------------------------------------------------------------------
// Contador de sección: el título base va en la propiedad baseTitle del QLabel y
// aquí se compone "Título · N" (sin sufijo si N es 0).
static void setSectionCount(QLabel *l, int n)
{
    if (!l)
        return;
    const QString base = l->property("baseTitle").toString();
    if (base.isEmpty())
        return;
    l->setText(n > 0 ? QStringLiteral("%1 · %2").arg(base).arg(n) : base);
}

void LibraryPage::rebuildSubscriptions()
{
    clearRows(m_subRowLay);
    m_subTitles.clear();
    m_subMeta.clear();

    const auto channels = m_subs->all();
    m_channelHint->setVisible(channels.isEmpty());
    setSectionCount(m_subsTitle, channels.size());

    for (const SubscriptionInfo &s : channels) {
        auto *row = new ChannelRow(m_thumbs, s.id,
                                   s.avatar.isEmpty() ? QString() : s.avatar,
                                   m_body);
        row->setTitle(s.title);
        m_subTitles.insert(s.id, s.title);
        row->setTitleLines(1);
        row->setMeta(subMetaFor(s.id));
        m_subMeta.insert(s.id, row);

        auto *x = makeIconBtn(QStringLiteral("Dejar de seguir"), false, row);
        connect(x, &QPushButton::clicked, this, [this, s]() {
            m_subs->remove(s.id);
            reload();
        });
        row->setActionWidget(x);
        row->applyContentWidth();
        row->setActivate([this, s]() { emit channelRequested(s.id, s.title); });
        m_subRowLay->addWidget(row);

        if (!m_subCounts.contains(s.id) || s.avatar.isEmpty())
            m_yt->channelInfo(s.id);
    }
    m_subRowLay->addStretch(1);
    QTimer::singleShot(0, this, &LibraryPage::updateSubsRowGeometry);
}

void LibraryPage::updateSubsRowGeometry()
{
    if (!m_subHost || !m_subHost->layout())
        return;
    m_subHost->layout()->invalidate();
    m_subHost->layout()->activate();
    m_subHost->adjustSize();
}

QString LibraryPage::subMetaFor(const QString &channelId) const
{
    const QString handle = channelHandleFor(m_subTitles.value(channelId));
    const auto it = m_subCounts.constFind(channelId);
    if (it == m_subCounts.constEnd() || it.value() <= 0)
        return handle;
    return QStringLiteral("%1\n%2 suscriptores")
        .arg(handle, compactCount(it.value()));
}

void LibraryPage::onChannelInfo(const ChannelInfo &channel)
{
    if (channel.subscriberCount > 0) {
        m_subCounts.insert(channel.id, channel.subscriberCount);
        ChannelRow *row = m_subMeta.value(channel.id);
        if (row) {
            QLabel *meta = row->metaLabel();
            if (meta)
                meta->setText(subMetaFor(channel.id));
            row->applyContentWidth();
        }
    }
    if (!channel.avatarUrl.isEmpty()) {
        m_thumbs->requestChannelAvatar(channel.id, channel.avatarUrl);
        // Persistir el avatar para que la suscripción no vuelva a nacer sin él
        // (p. ej. al suscribirse desde el reproductor sin tener aún los datos).
        if (m_subs->isSubscribed(channel.id))
            m_subs->setAvatar(channel.id, channel.avatarUrl);
    }
    QTimer::singleShot(0, this, &LibraryPage::updateSubsRowGeometry);
}

// --------------------------------------------------------------------------
// Listas de vídeos (filas clicables con menú)
// --------------------------------------------------------------------------
void LibraryPage::buildRows(
    QVBoxLayout *lay, const QVector<VideoInfo> &videos, int visible,
    const std::function<void(int)> &activate,
    const std::function<void(QMenu *, const VideoInfo &)> &fillMenu,
    const QHash<QString, double> &progress)
{
    const int n = qMin(visible, videos.size());
    for (int i = 0; i < n; ++i) {
        const VideoInfo &v = videos.at(i);
        auto *row = new RowItem(m_body);
        QString url = v.thumbnailUrl;
        if (url.isEmpty())
            url = fallbackThumbnailUrl(v.id);
        auto *thumb = new AsyncThumb(m_thumbs, QString(),
                                     QStringLiteral("v_%1").arg(v.id), url,
                                     kThumbW, kThumbH, 8, false, row);
        // Estado de reproducción: barra de progreso al pie de la miniatura
        // (vacío el mapa = sin barra). Solo lo pasan las secciones que saben
        // la posición: el Historial.
        thumb->setProgress(progress.value(v.id, 0.0));
        row->setLeading(thumb);
        row->setTitle(v.title);
        row->setMeta(metaLine(v));

        auto *menu = new QMenu(row);
        if (fillMenu)
            fillMenu(menu, v);
        row->setAction(makeMoreButton(menu, row));

        const int index = i;
        row->setActivate([this, activate, index]() { activate(index); });
        lay->addWidget(row);
    }
}

void LibraryPage::rebuildFavorites()
{
    clearRows(m_fav.lay);
    m_fav.videos = m_cache->getMany(m_favs->ids());
    const bool has = !m_fav.videos.isEmpty();
    m_fav.host->setVisible(has);
    m_fav.empty->setVisible(!has);
    m_fav.allBtn->setVisible(has && m_fav.videos.size() > kVisibleRows);
    m_fav.allBtn->setText(m_fav.showAll ? QStringLiteral("Ver menos")
                                        : QStringLiteral("Ver todo"));
    setSectionCount(m_fav.title, m_fav.videos.size());
    if (!has)
        return;
    const int visible = m_fav.showAll ? m_fav.videos.size() : kVisibleRows;
    buildRows(m_fav.lay, m_fav.videos, visible,
              [this](int index) { emit openRequested(m_fav.videos, index); },
              [this](QMenu *menu, const VideoInfo &v) {
                  addMenuAction(menu, QStringLiteral("Quitar de Me gusta"),
                                [this, id = v.id]() {
                                    m_favs->toggle(id);
                                    reload();
                                });
                  addStandardMenuActions(menu, v, true);
              });
}

void LibraryPage::rebuildWatchLater()
{
    clearRows(m_later.lay);
    m_later.videos = m_cache->getMany(m_watchLater->ids());
    const bool has = !m_later.videos.isEmpty();
    m_later.host->setVisible(has);
    m_later.empty->setVisible(!has);
    m_later.allBtn->setVisible(has && m_later.videos.size() > kVisibleRows);
    m_later.allBtn->setText(m_later.showAll ? QStringLiteral("Ver menos")
                                            : QStringLiteral("Ver todo"));
    setSectionCount(m_later.title, m_later.videos.size());
    if (!has)
        return;
    const int visible = m_later.showAll ? m_later.videos.size() : kVisibleRows;
    buildRows(m_later.lay, m_later.videos, visible,
              [this](int index) { emit openRequested(m_later.videos, index); },
              [this](QMenu *menu, const VideoInfo &v) {
                  addMenuAction(menu,
                                QStringLiteral("Quitar de Ver más tarde"),
                                [this, id = v.id]() {
                                    m_watchLater->remove(id);
                                    reload();
                                });
                  addStandardMenuActions(menu, v, false);
              });
}

void LibraryPage::rebuildHistory()
{
    clearRows(m_historySec.lay);
    m_historySec.videos = m_cache->getMany(m_history->recentIds(10));
    const bool has = !m_historySec.videos.isEmpty();
    m_historySec.host->setVisible(has);
    m_historySec.empty->setVisible(!has);
    m_historySec.allBtn->setVisible(has && m_historySec.videos.size() > kVisibleRows);
    setSectionCount(m_historySec.title, m_historySec.videos.size());
    if (!has)
        return;
    // Estado de reproducción, igual que en el Home: barra de progreso al pie
    // de la miniatura de cada fila que se puede reanudar.
    QHash<QString, double> progress;
    for (const VideoInfo &v : m_historySec.videos) {
        const double f =
            HistoryRepo::progressFraction(v, m_history->positionOf(v.id));
        if (f > 0.0)
            progress.insert(v.id, f);
    }
    buildRows(m_historySec.lay, m_historySec.videos,
              qMin<int>(kVisibleRows, m_historySec.videos.size()),
              [this](int index) {
                  const qlonglong pos = m_history->positionOf(
                      m_historySec.videos.at(index).id);
                  const VideoInfo &v = m_historySec.videos.at(index);
                  emit resumeRequested(m_historySec.videos, index,
                                       HistoryRepo::resumeStartMs(v, pos));
              },
              [this](QMenu *menu, const VideoInfo &v) {
                  addMenuAction(menu, QStringLiteral("Quitar del historial"),
                                [this, id = v.id]() {
                                    m_history->remove(id);
                                    reload();
                                });
                  addStandardMenuActions(menu, v, true);
              },
              progress);
}

// La lista de acciones es la compartida por TODAS las superficies con fila
// (nt::addRowMenuActions); aquí solo se pasan las piezas que son de esta
// página: su popup de "Nueva playlist" y la recarga de la sección al escribir.
void LibraryPage::addStandardMenuActions(QMenu *menu, const VideoInfo &v,
                                         bool withWatchLater)
{
    nt::RowMenuHooks hooks;
    hooks.onNewPlaylist = [this](const QString &videoId) {
        promptNewPlaylist(videoId);
    };
    hooks.onChannel = [this](const QString &channelId, const QString &name) {
        emit channelRequested(channelId, name);
    };
    hooks.onChanged = [this]() { reload(); };
    nt::addRowMenuActions(menu, v, m_watchLater, m_playlists, hooks,
                          withWatchLater);
}

// --------------------------------------------------------------------------
// Playlists
// --------------------------------------------------------------------------
void LibraryPage::rebuildPlaylists()
{
    for (QWidget *w : m_playlistCards)
        delete w;
    m_playlistCards.clear();
    m_playlistMeta.clear();

    const auto playlists = m_playlists->all();
    m_playlistEmpty->setVisible(playlists.isEmpty());
    setSectionCount(m_playlistsTitle, playlists.size());

    for (const PlaylistItem &p : playlists) {
        const QVector<VideoInfo> videos =
            m_cache->getMany(m_playlists->videoIds(p.id));
        auto *row = new RowItem(m_body);

        QString url;
        QString key = QStringLiteral("pl_%1").arg(p.id);
        if (!videos.isEmpty()) {
            const VideoInfo &first = videos.first();
            key = QStringLiteral("v_%1").arg(first.id);
            url = first.thumbnailUrl;
        }
        auto *thumb = new AsyncThumb(m_thumbs, QString(), key, url,
                                     kThumbW, kThumbH, 8, false, row);
        row->setLeading(thumb);
        row->setTitle(p.title);
        row->setMeta(QStringLiteral("%1 vídeos").arg(videos.size()));

        auto *menu = new QMenu(row);
        addMenuAction(menu, QStringLiteral("Renombrar"),
                      [this, p]() { promptRenamePlaylist(p); });
        // Misma confirmación que el botón "Eliminar" de la página Playlist, y el
        // motivo es el mismo: la playlist desaparece entera y no hay vuelta atrás.
        // No hace falta un QTimer::singleShot(0) aquí: addMenuAction ya lo hace,
        // para que la lista de la que nació el menú no se quede sin la fila con
        // el diálogo abierto.
        addMenuAction(menu, QStringLiteral("Eliminar"),
                      [this, p, n = videos.size()]() {
            const QString where = p.title.trimmed().isEmpty()
                ? QStringLiteral("esta playlist")
                : QStringLiteral("«%1»").arg(p.title.trimmed());
            nt::confirm(this, QStringLiteral("Eliminar playlist"),
                        QStringLiteral("Se eliminará %1 con sus %2 vídeos. Esta "
                                       "acción no se puede deshacer.")
                            .arg(where, QStringLiteral("%1").arg(n)),
                        QStringLiteral("Eliminar"), [this, p]() {
                            m_playlists->deletePlaylist(p.id);
                            rebuildPlaylists();
                        });
        });
        row->setAction(makeMoreButton(menu, row));
        row->setActivate([this, p]() { emit playlistRequested(p.id, p.title); });
        m_playlistCards.append(row);
        m_playlistMeta.append(p);
    }
    layoutCards(m_playlistGrid, m_playlistCards);
}

void LibraryPage::layoutCards(QGridLayout *grid, const QVector<QWidget *> &cards)
{
    while (QLayoutItem *item = grid->takeAt(0))
        delete item;
    const int cols =
        qBound(1, (width() - 40) / kCardMinW, kMaxCardCols);
    int row = 0;
    int col = 0;
    for (QWidget *w : cards) {
        grid->addWidget(w, row, col);
        if (++col == cols) {
            col = 0;
            ++row;
        }
    }
    for (int c = 0; c < kMaxCardCols; ++c)
        grid->setColumnStretch(c, c < cols ? 1 : 0);
}

// --------------------------------------------------------------------------
// Crear / renombrar playlist: el diálogo de la app (nt::inputText)
// --------------------------------------------------------------------------
// Antes esta página tenía SU PROPIO QFrame#popupDialog con el campo
// (setupCreatePlaylistPopup y compañía, ~105 líneas) y el Player tenía otro
// idéntico. Los dos hacen ahora exactamente lo mismo que nt::confirm y que el
// prompt de Historial y Playlist: una caja hija de la VENTANA (no de la
// página), con velo, modal y sin ventana flotante. Antes los dos popoveres no
// bloqueaban y el del Player ni siquiera cerraba con Escape, así que el mismo
// "Añadir a playlist" se comportaba de tres maneras según la página.
void LibraryPage::promptNewPlaylist(const QString &videoId)
{
    QString name;
    if (!nt::inputText(this, QStringLiteral("Nueva playlist"),
                       QStringLiteral("Nombre:"), QString(),
                       QStringLiteral("Crear"), &name))
        return;
    if (name.isEmpty())
        return;
    const QString id = m_playlists->create(name);
    if (id.isEmpty())
        return;
    if (!videoId.isEmpty())
        m_playlists->addVideo(id, videoId);
    rebuildPlaylists();
}

void LibraryPage::promptRenamePlaylist(const PlaylistItem &playlist)
{
    if (playlist.id.isEmpty())
        return;
    QString name;
    if (!nt::inputText(this, QStringLiteral("Renombrar playlist"),
                       QStringLiteral("Nuevo nombre:"), playlist.title,
                       QStringLiteral("Guardar"), &name))
        return;
    if (name.isEmpty())
        return;
    if (!m_playlists->rename(playlist.id, name))
        return;
    rebuildPlaylists();
}

void LibraryPage::createPlaylist()
{
    promptNewPlaylist(QString());
}

void LibraryPage::createPlaylistForVideo(const QString &videoId)
{
    promptNewPlaylist(videoId);
}

void LibraryPage::centerPopup(QFrame *popup)
{
    if (!popup)
        return;
    popup->adjustSize();
    const int x = (width() - popup->width()) / 2;
    const int y = (height() - popup->height()) / 2;
    popup->move(qMax(0, x), qMax(0, y));
}

// --------------------------------------------------------------------------
// Popup: gestionar suscripciones
// --------------------------------------------------------------------------
void LibraryPage::setupManageSubsPopup()
{
    m_managePopup = new QFrame(this);
    m_managePopup->setObjectName(QStringLiteral("popupDialog"));
    m_managePopup->setFixedWidth(420);
    m_managePopup->raise();
    m_managePopup->hide();

    auto *v = new QVBoxLayout(m_managePopup);
    v->setContentsMargins(18, 16, 18, 14);
    v->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("Gestionar suscripciones"),
                             m_managePopup);
    title->setObjectName(QStringLiteral("popupTitle"));
    v->addWidget(title);

    auto *scroll = new QScrollArea(m_managePopup);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget;
    m_manageLay = new QVBoxLayout(content);
    m_manageLay->setContentsMargins(0, 0, 0, 0);
    m_manageLay->setSpacing(2);
    scroll->setWidget(content);
    v->addWidget(scroll, 1);

    auto *row = new QHBoxLayout;
    row->addStretch(1);
    auto *close = new QPushButton(QStringLiteral("Cerrar"), m_managePopup);
    close->setObjectName(QStringLiteral("ghost"));
    close->setCursor(Qt::PointingHandCursor);
    connect(close, &QPushButton::clicked, this, &LibraryPage::closeManageSubs);
    row->addWidget(close);
    v->addLayout(row);
}

void LibraryPage::openManageSubs()
{
    rebuildManageSubs();
    m_managePopup->adjustSize();
    m_managePopup->setFixedHeight(qMin(560, height() - 60));
    centerPopup(m_managePopup);
    m_managePopup->raise();
    m_managePopup->show();
}

void LibraryPage::closeManageSubs()
{
    m_managePopup->hide();
}

void LibraryPage::rebuildManageSubs()
{
    clearRows(m_manageLay);
    const auto channels = m_subs->all();
    for (const SubscriptionInfo &s : channels) {
        auto *row = new RowItem;
        auto *avatar = new AsyncThumb(
            m_thumbs, s.id, QStringLiteral("ch_%1").arg(s.id),
            s.avatar.isEmpty() ? QString() : s.avatar, 40, 40, 20, true, row);
        row->setLeading(avatar);
        row->setTitle(s.title);
        row->setMeta(channelHandleFor(s.title));
        auto *x = makeIconBtn(QStringLiteral("Dejar de seguir"), false, row);
        connect(x, &QPushButton::clicked, this, [this, s]() {
            m_subs->remove(s.id);
            reload();
            rebuildManageSubs();
        });
        row->setAction(x);
        row->setActivate([this, s]() { emit channelRequested(s.id, s.title); });
        m_manageLay->addWidget(row);
    }
    m_manageLay->addStretch(1);
}

// Antes este eventFilter solo existia para que Escape cerrara el QLineEdit del
// popup de playlist. Ese popup es ahora nt::inputText, que gestiona su propio
// Escape, asi que el filtro sobra entero y se elimina el override.

void LibraryPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutCards(m_playlistGrid, m_playlistCards);
    // El popup de crear/renombrar playlist ya no se recentra aquí: es
    // nt::inputText, que es hijo de la VENTANA y no de esta página, así que su
    // centro no depende del ancho de Biblioteca.
    if (m_managePopup && m_managePopup->isVisible())
        centerPopup(m_managePopup);
}