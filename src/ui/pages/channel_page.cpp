#include "channel_page.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

#include "../../db/repositories.h"
#include "../../debug.h"
#include "../../yt.h"
#include "../icons.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

// Tope vertical de la caja de descripción del canal. Si el texto completo la
// supera, la descripción se recorta a este alto y se muestra "Mostrar más"
// (ver applyDescriptionCap).
static const int kDescMaxHeight = 165;
// Margen de tolerancia: si el texto completo solo excede el tope por menos de
// este valor (~media línea), se muestra entero SIN botón "Mostrar más" (un
// recorte de pocos píxeles no merece desplegable).
static const qreal kDescSlack = 5.5;

ChannelPage::ChannelPage(Yt *yt, ThumbnailCache *thumbs,
                         SubscriptionsRepo *subs, QWidget *parent)
    : QWidget(parent)
    , m_yt(yt)
    , m_thumbs(thumbs)
    , m_subs(subs)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *top = new QHBoxLayout;
    auto *back = new QPushButton(QStringLiteral("Volver"), this);
    back->setObjectName(QStringLiteral("ghost"));
    connect(back, &QPushButton::clicked, this, &ChannelPage::backRequested);
    top->addWidget(back);
    top->addStretch(1);
    root->addLayout(top);

    auto *headerRow = new QHBoxLayout;
    headerRow->setSpacing(12);

    m_avatar = new QLabel(this);
    m_avatar->setFixedSize(64, 64);
    m_avatar->setScaledContents(true);
    m_avatar->setPixmap(nt::icon(nt::Icon::User, nt::kMuted, 40)
                            .pixmap(64, 64));
    m_avatar->setObjectName(QStringLiteral("channelAvatar"));
    headerRow->addWidget(m_avatar, 0, Qt::AlignVCenter);

    auto *titleCol = new QVBoxLayout;
    titleCol->setSpacing(2);
    m_title = new QLabel(QStringLiteral("Canal"), this);
    m_title->setObjectName(QStringLiteral("sectionTitle"));
    m_avatarKey.clear();
    titleCol->addWidget(m_title);
    m_meta = new QLabel(this);
    m_meta->setObjectName(QStringLiteral("sectionSub"));
    titleCol->addWidget(m_meta);
    headerRow->addLayout(titleCol, 1);

    m_subscribeBtn = new QPushButton(this);
    m_subscribeBtn->setObjectName(QStringLiteral("subscribe"));
    m_subscribeBtn->setCursor(Qt::PointingHandCursor);
    m_subscribeBtn->setMinimumHeight(36);
    headerRow->addWidget(m_subscribeBtn, 0, Qt::AlignVCenter);

    root->addLayout(headerRow);

    m_description = new QLabel(this);
    m_description->setObjectName(QStringLiteral("sectionSub"));
    m_description->setWordWrap(true);
    root->addWidget(m_description);

    // Botón "Mostrar más/ menos" de la descripción: aparece solo cuando el
    // texto completo supera el tope vertical (ver applyDescriptionCap).
    m_descMore = new QPushButton(QStringLiteral("Mostrar más"), this);
    m_descMore->setObjectName(QStringLiteral("ghost"));
    m_descMore->setCursor(Qt::PointingHandCursor);
    m_descMore->hide();
    root->addWidget(m_descMore, 0, Qt::AlignLeft);
    connect(m_descMore, &QPushButton::clicked, this,
            &ChannelPage::toggleDescription);

    m_tabs = new QTabWidget(this);
    const QStringList names{QStringLiteral("Videos"), QStringLiteral("Shorts"),
                            QStringLiteral("Directos")};
    const QStringList keys{QStringLiteral("videos"), QStringLiteral("shorts"),
                           QStringLiteral("live")};
    for (int i = 0; i < names.size(); ++i) {
        auto *tabWrap = new QWidget(m_tabs);
        auto *tabLayout = new QVBoxLayout(tabWrap);
        // Margen inferior: eleva el botón "Mostrar más" para que no quede
        // recortado/pegado al borde inferior del cuadro de vídeos.
        tabLayout->setContentsMargins(0, 0, 0, 8);
        tabLayout->setSpacing(6);
        auto *list = new VideoListView(m_thumbs, tabWrap);
        list->showVideos(QVector<VideoInfo>(), VideoListView::ListMode);
        m_lists.append(list);
        m_videos.append(QVector<VideoInfo>());
        m_tabKeys.append(keys.at(i));
        m_tabLoaded.append(0);
        tabLayout->addWidget(list, 1);
        auto *more = new QPushButton(QStringLiteral("Mostrar más"), tabWrap);
        more->setObjectName(QStringLiteral("primary"));
        more->setCursor(Qt::PointingHandCursor);
        more->setFixedWidth(180);
        more->hide();
        tabLayout->addWidget(more, 0, Qt::AlignHCenter);
        m_moreButtons.append(more);
        m_tabs->addTab(tabWrap, names.at(i));
        const int index = i;
        connect(list, &VideoListView::videoActivated, this,
                [this, index](int item, const VideoInfo &) {
                    emit openRequested(m_videos.at(index), item);
                });
        connect(more, &QPushButton::clicked, this, [this, index]() {
            m_yt->channelTabVideos(m_channelId, m_tabKeys.at(index), 25,
                                   m_videos.at(index).size());
            m_moreButtons.at(index)->setEnabled(false);
        });
    }
    root->addWidget(m_tabs, 1);

    connect(m_yt, &Yt::channelInfoReady, this, &ChannelPage::onChannelInfo);
    connect(m_yt, &Yt::channelTabVideosFinished, this,
            &ChannelPage::onTabVideos);
    // Carga perezosa: la pestaña que el usuario abre se pide en ese momento.
    connect(m_tabs, &QTabWidget::currentChanged, this,
            [this](int index) { loadTab(index); });
    connect(m_thumbs, &ThumbnailCache::ready, this,
            &ChannelPage::onAvatarReady);
    connect(m_subscribeBtn, &QPushButton::clicked, this, [this]() {
        if (m_channelId.isEmpty())
            return;
        const bool subscribed = m_subs->isSubscribed(m_channelId);
        if (subscribed) {
            m_subs->remove(m_channelId);
        } else {
            m_subs->add(m_channelId, m_channelName, m_avatarUrl);
        }
        updateSubscribeButton(!subscribed);
    });
}

void ChannelPage::openChannel(const QString &channelId)
{
    m_channelId = channelId;
    m_channelName.clear();
    m_avatarKey.clear();
    m_avatarUrl.clear();
    m_title->setText(QStringLiteral("Cargando canal…"));
    m_meta->clear();
    m_description->clear();
    // Nueva descripción: siempre arranca colapsada con el tope aplicado
    // (si el texto no cabe se re-evalúa en onChannelInfo).
    m_descExpanded = false;
    m_descMore->setText(QStringLiteral("Mostrar más"));
    m_descMore->hide();
    m_avatar->setPixmap(nt::icon(nt::Icon::User, nt::kMuted, 40)
                            .pixmap(64, 64));
    updateSubscribeButton(m_subs->isSubscribed(channelId));
    for (auto *list : std::as_const(m_lists))
        list->clearVideos();
    for (auto &videos : m_videos)
        videos.clear();
    for (auto *button : std::as_const(m_moreButtons)) {
        button->setVisible(false);
        button->setEnabled(true);
    }
    m_yt->channelInfo(channelId);
    // Carga perezosa: solo se piden los vídeos al abrir; shorts/directos al
    // entrar en su pestaña (cada respuesta pesa ~400-500 KB).
    m_tabLoaded.fill(0);
    m_tabs->setCurrentIndex(0);
    loadTab(0);
}

void ChannelPage::loadTab(int index)
{
    if (index < 0 || index >= m_tabKeys.size() || m_channelId.isEmpty())
        return;
    if (index < m_tabLoaded.size() && m_tabLoaded.at(index))
        return;
    if (index < m_tabLoaded.size())
        m_tabLoaded[index] = 1;
    NT_DEBUG("E2E channel loadTab id=%s tab=%s\n",
             m_channelId.toUtf8().constData(),
             m_tabKeys.at(index).toUtf8().constData());
    m_yt->channelTabVideos(m_channelId, m_tabKeys.at(index), 25);
}

void ChannelPage::onChannelInfo(const ChannelInfo &channel)
{
    if (channel.id != m_channelId)
        return;
    m_channelName = channel.name;
    m_title->setText(channel.name);
    m_meta->setText(channel.subscriberCount > 0
                        ? QStringLiteral("%1 suscriptores").arg(channel.subscriberCount)
                        : QString());
    m_description->setText(channel.description);
    // Tras el layout: aplicar el tope vertical (165 px) y medir la caja en
    // E2E (solo con NEVERTUBE_DEBUG=1). El retardo deja asentar la geometría.
    QTimer::singleShot(120, this, [this]() {
        if (m_channelId.isEmpty())
            return;
        applyDescriptionCap();
        NT_DEBUG("E2E canal descripcion caja: size=%dx%d global=(%d,%d) "
                 "local=(%d,%d) page=%dx%d fullH=%d btn=%d\n",
                 m_description->width(), m_description->height(),
                 m_description->mapToGlobal(QPoint(0, 0)).x(),
                 m_description->mapToGlobal(QPoint(0, 0)).y(),
                 m_description->x(), m_description->y(), width(), height(),
                 m_description->heightForWidth(m_description->width()),
                 m_descMore->isVisible() ? 1 : 0);
    });
    m_avatarKey = QStringLiteral("ch_%1").arg(channel.id);
    if (!channel.avatarUrl.isEmpty())
        m_avatarUrl = channel.avatarUrl;
    const QPixmap cached = m_thumbs->pixmap(m_avatarKey);
    if (!cached.isNull()) {
        m_avatar->setPixmap(cached);
    } else if (!channel.avatarUrl.isEmpty()) {
        m_thumbs->requestChannelAvatar(channel.id, channel.avatarUrl);
    }
    if (m_subs->isSubscribed(channel.id))
        m_subs->setAvatar(channel.id, channel.avatarUrl);
}

void ChannelPage::onAvatarReady(const QString &key)
{
    if (key != m_avatarKey)
        return;
    const QPixmap pm = m_thumbs->pixmap(key);
    if (!pm.isNull())
        m_avatar->setPixmap(pm);
}

void ChannelPage::updateSubscribeButton(bool subscribed)
{
    m_subscribeBtn->setText(subscribed ? QStringLiteral("Suscrito")
                                       : QStringLiteral("Suscribirse"));
    // El aspecto de los dos estados vive en el QSS global (QPushButton#subscribe
    // y #subscribed) y lo distingue la propiedad booleana `subscribed`. El
    // acento lo pone nt::styleSheet(), que se regenera entero en cada cambio, asi
    // que ya no hace falta re-pintar el boton a mano.
    m_subscribeBtn->setProperty("subscribed", subscribed);
    // La propiedad cambia el selector del QSS, asi que hay que re-polish: sin
    // esto Qt no vuelve a resolver las reglas y el boton se queda con el
    // aspecto del estado anterior.
    m_subscribeBtn->style()->unpolish(m_subscribeBtn);
    m_subscribeBtn->style()->polish(m_subscribeBtn);
    m_subscribeBtn->update();
}

// Re-aplica el botón Suscribirse con el acento vigente (llamada desde
// MainWindow al cambiar el color de acento en Ajustes).
void ChannelPage::applyAccent()
{
    updateSubscribeButton(m_subs->isSubscribed(m_channelId));
}

void ChannelPage::onTabVideos(const QString &channelId, const QString &tab,
                              const QVector<VideoInfo> &videos)
{
    if (channelId != m_channelId)
        return;
    const int index = tab == QLatin1String("shorts") ? 1
                    : tab == QLatin1String("live") ? 2 : 0;
    QVector<VideoInfo> loaded = videos;
    m_moreButtons.at(index)->setEnabled(true);
    if (!m_channelName.isEmpty()) {
        for (VideoInfo &v : loaded) {
            if (v.channel.isEmpty())
                v.channel = m_channelName;
        }
    }
    if (m_videos.at(index).isEmpty()) {
        m_videos[index] = loaded;
        m_lists[index]->showVideos(loaded, VideoListView::ListMode);
    } else {
        m_videos[index] += loaded;
        m_lists[index]->appendVideos(loaded);
    }
    m_moreButtons.at(index)->setVisible(videos.size() >= 25);
}

// Tope vertical de la descripción: si el texto completo supera
// kDescMaxHeight se recorta a ese alto y el botón "Mostrar más" permite
// desplegarla a altura natural (y volver a colapsar con "Mostrar menos").
// Se re-evalúa también en el resize (el re-wrap por ancho cambia la altura
// necesaria) y al cambiar de canal.
void ChannelPage::applyDescriptionCap()
{
    if (m_description->text().isEmpty()) {
        m_descMore->hide();
        m_description->setMinimumHeight(0);
        m_description->setMaximumHeight(QWIDGETSIZE_MAX);
        return;
    }
    if (m_descExpanded) {
        // Desplegada: altura natural, el botón pasa a "Mostrar menos".
        m_descMore->show();
        m_descMore->setText(QStringLiteral("Mostrar menos"));
        m_description->setMinimumHeight(0);
        m_description->setMaximumHeight(QWIDGETSIZE_MAX);
        return;
    }
    const int fullHeight =
        m_description->heightForWidth(m_description->width());
    const qreal overflow = fullHeight - kDescMaxHeight;
    if (overflow <= kDescSlack) {
        // Cabe dentro del tope (o casi: un exceso menor que el margen se
        // muestra entero sin desplegable; ver kDescSlack).
        m_descMore->hide();
        m_description->setMinimumHeight(0);
        m_description->setMaximumHeight(QWIDGETSIZE_MAX);
    } else {
        // Recortada a kDescMaxHeight con "Mostrar más" para ver el texto
        // completo. El texto sobrante queda visualmente fuera de la caja
        // (el QLabel con wordWrap recorta el pintado al rect del widget).
        m_descMore->show();
        m_descMore->setText(QStringLiteral("Mostrar más"));
        m_description->setMinimumHeight(0);
        m_description->setMaximumHeight(kDescMaxHeight);
    }
}

void ChannelPage::toggleDescription()
{
    if (m_description->text().isEmpty())
        return;
    m_descExpanded = !m_descExpanded;
    applyDescriptionCap();
}

void ChannelPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    applyDescriptionCap();
}
