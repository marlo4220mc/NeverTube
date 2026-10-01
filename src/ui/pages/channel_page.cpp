#include "channel_page.h"

#include <QClipboard>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

#include "../../db/repositories.h"
#include "../../debug.h"
#include "../../yt.h"
#include "../format.h"
#include "../icons.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

namespace {

// Avatar de la ficha: 80 px y circular. Antes eran 64 px con setScaledContents,
// que deforma cualquier avatar que no sea cuadrado y además lo pintaba en un
// cuadrado (el radio del QSS solo redondea el fondo, no el pixmap).
const int kAvatar = 80;
// Líneas de la descripción antes de colapsar. El Player recorta a 3 (kDescLines
// en watch_page.cpp) y aquí se adopta el mismo corte: el tope en píxeles que
// usaba esta página dependía del interlineado de la fuente, así que cambiaba de
// una máquina a otra.
const int kDescLines = 3;
// Vídeos por página. Es también lo que decide si hay más que cargar: si la
// respuesta viene llena, existe una página siguiente.
const int kTabPage = 25;

// Texto del estado vacío de una pestaña sin vídeos. Cada pestaña tiene su
// palabra, para que se sepa cuál se abrió.
QString emptyTextFor(const QString &tab)
{
    if (tab == QLatin1String("shorts"))
        return QStringLiteral("Este canal no tiene Shorts.");
    if (tab == QLatin1String("live"))
        return QStringLiteral("Este canal no tiene directos.");
    return QStringLiteral("Este canal no tiene vídeos.");
}

} // namespace

ChannelPage::ChannelPage(Yt *yt, ThumbnailCache *thumbs,
                         SubscriptionsRepo *subs, QWidget *parent)
    : QWidget(parent)
    , m_yt(yt)
    , m_thumbs(thumbs)
    , m_subs(subs)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    // Mismo "Volver" en las cuatro superficies con vuelta atrás (Historial,
    // Playlist y el Player): el token #backLink, que es un enlace discreto y
    // no una cápsula. Es navegación, no un control de la página.
    auto *top = new QHBoxLayout;
    auto *back = new QPushButton(QStringLiteral("Volver"), this);
    back->setObjectName(QStringLiteral("backLink"));
    back->setIcon(nt::icon(nt::Icon::Back, nt::kMuted, 13));
    back->setCursor(Qt::PointingHandCursor);
    connect(back, &QPushButton::clicked, this, &ChannelPage::backRequested);
    top->addWidget(back);
    top->addSpacing(4);
    top->addStretch(1);
    root->addLayout(top);

    // --- Ficha del canal ---------------------------------------------------
    // Caja #channelCard: avatar, nombre, suscriptores y las dos acciones (la
    // pastilla de Suscribirse y el chip de Compartir), con la descripción
    // debajo. Antes todo eso iba suelto sobre el fondo de la página.
    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("channelCard"));
    m_card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *cardLay = new QVBoxLayout(m_card);
    cardLay->setContentsMargins(16, 14, 16, 12);
    cardLay->setSpacing(10);

    auto *head = new QHBoxLayout;
    head->setSpacing(14);

    m_avatar = new QLabel(m_card);
    m_avatar->setObjectName(QStringLiteral("channelAvatar"));
    m_avatar->setFixedSize(kAvatar, kAvatar);
    m_avatar->setAlignment(Qt::AlignCenter);
    setAvatar(QPixmap());
    head->addWidget(m_avatar, 0, Qt::AlignVCenter);

    auto *titleCol = new QVBoxLayout;
    titleCol->setContentsMargins(0, 0, 0, 0);
    titleCol->setSpacing(3);
    m_title = new QLabel(QStringLiteral("Canal"), m_card);
    m_title->setObjectName(QStringLiteral("channelName"));
    m_title->setWordWrap(true);
    titleCol->addWidget(m_title);
    m_meta = new QLabel(m_card);
    m_meta->setObjectName(QStringLiteral("channelMeta"));
    titleCol->addWidget(m_meta);
    head->addLayout(titleCol, 1);

    m_subscribeBtn = new QPushButton(m_card);
    m_subscribeBtn->setObjectName(QStringLiteral("subscribe"));
    m_subscribeBtn->setCursor(Qt::PointingHandCursor);
    m_subscribeBtn->setFixedHeight(38);
    head->addWidget(m_subscribeBtn, 0, Qt::AlignVCenter);

    m_shareBtn = new QPushButton(m_card);
    m_shareBtn->setObjectName(QStringLiteral("actionChip"));
    m_shareBtn->setIconSize(QSize(18, 18));
    m_shareBtn->setIcon(nt::icon(nt::Icon::Share, nt::kText, 18));
    m_shareBtn->setCursor(Qt::PointingHandCursor);
    m_shareBtn->setFixedSize(38, 38);
    m_shareBtn->setToolTip(QStringLiteral("Copiar el enlace del canal"));
    m_shareBtn->setAccessibleName(QStringLiteral("Compartir el canal"));
    head->addWidget(m_shareBtn, 0, Qt::AlignVCenter);
    cardLay->addLayout(head);

    m_description = new QLabel(m_card);
    m_description->setObjectName(QStringLiteral("channelDesc"));
    m_description->setWordWrap(true);
    m_description->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_description->hide();
    cardLay->addWidget(m_description);

    // Degradado sobre las últimas líneas mientras la descripción está
    // colapsada: es el mismo #descFade de la tarjeta del Player (y al fondo
    // #181818, el mismo de la caja, así que funde sin costura).
    m_descFade = new QFrame(m_card);
    m_descFade->setObjectName(QStringLiteral("descFade"));
    m_descFade->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_descFade->hide();

    m_descToggle = new QPushButton(QStringLiteral("…más"), m_card);
    m_descToggle->setObjectName(QStringLiteral("descToggle"));
    m_descToggle->setCursor(Qt::PointingHandCursor);
    m_descToggle->hide();
    connect(m_descToggle, &QPushButton::clicked, this,
            &ChannelPage::toggleDescription);
    cardLay->addWidget(m_descToggle, 0, Qt::AlignLeft);
    root->addWidget(m_card);

    // --- Pestañas ----------------------------------------------------------
    const QStringList names{QStringLiteral("Videos"), QStringLiteral("Shorts"),
                            QStringLiteral("Directos")};
    const QStringList keys{QStringLiteral("videos"), QStringLiteral("shorts"),
                           QStringLiteral("live")};

    auto *tabRow = new QHBoxLayout;
    tabRow->setSpacing(10);
    m_tabsBox = new QWidget(this);
    m_tabsBox->setObjectName(QStringLiteral("channelTabs"));
    auto *tabsLay = new QHBoxLayout(m_tabsBox);
    tabsLay->setContentsMargins(3, 3, 3, 3);
    tabsLay->setSpacing(2);
    tabRow->addWidget(m_tabsBox, 0, Qt::AlignVCenter);
    m_tabCount = new QLabel(this);
    m_tabCount->setObjectName(QStringLiteral("tabCount"));
    m_tabCount->hide();
    tabRow->addWidget(m_tabCount, 0, Qt::AlignVCenter);
    tabRow->addStretch(1);
    root->addLayout(tabRow);

    for (int i = 0; i < names.size(); ++i) {
        auto *btn = new QPushButton(names.at(i), m_tabsBox);
        btn->setObjectName(QStringLiteral("tabBtn"));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setProperty("selected", i == 0);
        tabsLay->addWidget(btn);
        m_tabButtons.append(btn);

        auto *page = new QWidget(this);
        auto *pageLay = new QVBoxLayout(page);
        pageLay->setContentsMargins(0, 0, 0, 0);
        pageLay->setSpacing(8);

        auto *list = new VideoListView(m_thumbs, page);
        list->showVideos(QVector<VideoInfo>(), VideoListView::ListMode);
        // Botón de opciones por fila (mismo criterio que Search): la página
        // re-emite el vídeo y es MainWindow quien tiene los repos y muestra el
        // menú.
        list->setRowMoreVisible(true);
        pageLay->addWidget(list, 1);

        // Estado vacío de la pestaña (caja punteada, el mismo #emptyState de la
        // Biblioteca). Solo se ve cuando la respuesta llegó vacía: por eso la
        // lista se oculta a la vez (los dos con stretch partirían la caja).
        // El alto FIJO de 96 px importa: medido, una etiqueta con stretch 0 en un
        // QVBoxLayout cuyo único elemento con stretch está oculto se estira a
        // TODO el alto del área (700 px de caja punteada); con alto fijo queda
        // como una banda centrada verticalmente (y=302 de 700, ancho completo).
        auto *empty = new QLabel(emptyTextFor(keys.at(i)), page);
        empty->setObjectName(QStringLiteral("emptyState"));
        empty->setAlignment(Qt::AlignCenter);
        empty->setFixedHeight(96);
        empty->hide();
        pageLay->addWidget(empty);

        // Paginación: un ghost centrado. El botón era #primary de 180 px fijos
        // (una barra roja ancha bajo la lista); "cargar más" no es la acción
        // principal de la página, es una ayuda para bajar.
        auto *more = new QPushButton(QStringLiteral("Cargar más vídeos"), page);
        more->setObjectName(QStringLiteral("ghost"));
        more->setCursor(Qt::PointingHandCursor);
        more->setFixedHeight(34);
        more->hide();
        pageLay->addWidget(more, 0, Qt::AlignHCenter);

        page->setVisible(i == 0);
        root->addWidget(page, 1);

        m_pages.append(page);
        m_lists.append(list);
        m_moreButtons.append(more);
        m_emptyLabels.append(empty);
        m_videos.append(QVector<VideoInfo>());
        m_tabKeys.append(keys.at(i));
        m_tabLoaded.append(0);

        connect(btn, &QPushButton::clicked, this, [this, i]() {
            showTab(i);
            loadTab(i);
        });
        connect(list, &VideoListView::videoActivated, this,
                [this, i](int item, const VideoInfo &) {
                    emit openRequested(m_videos.at(i), item);
                });
        connect(list, &VideoListView::moreRequested, this,
                [this](int, const VideoInfo &v, const QPoint &pos) {
                    emit rowMenuRequested(v, pos);
                });
        connect(more, &QPushButton::clicked, this, [this, i]() {
            m_yt->channelTabVideos(m_channelId, m_tabKeys.at(i), kTabPage,
                                   m_videos.at(i).size());
            m_moreButtons.at(i)->setEnabled(false);
        });
    }

    connect(m_yt, &Yt::channelInfoReady, this, &ChannelPage::onChannelInfo);
    connect(m_yt, &Yt::channelTabVideosFinished, this,
            &ChannelPage::onTabVideos);
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
    connect(m_shareBtn, &QPushButton::clicked, this, [this]() {
        if (m_channelId.isEmpty())
            return;
        QGuiApplication::clipboard()->setText(
            QStringLiteral("https://www.youtube.com/channel/%1").arg(m_channelId));
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
    m_description->hide();
    m_description->setMaximumHeight(QWIDGETSIZE_MAX);
    // Nueva descripción: siempre arranca colapsada con el tope aplicado
    // (si el texto no cabe se re-evalúa en onChannelInfo).
    m_descExpanded = false;
    m_descCollapsible = false;
    m_descToggle->hide();
    if (m_descFade)
        m_descFade->hide();
    setAvatar(QPixmap());
    updateSubscribeButton(m_subs->isSubscribed(channelId));
    for (auto *list : std::as_const(m_lists))
        list->clearVideos();
    for (auto &videos : m_videos)
        videos.clear();
    for (auto *button : std::as_const(m_moreButtons)) {
        button->setVisible(false);
        button->setEnabled(true);
    }
    // Cada pestaña vuelve a su lista: si la anterior estaba vacía, su estado
    // vacío había ocultado la lista y sin esto el hueco se queda en negro
    // hasta que llegue la respuesta del canal nuevo.
    for (auto *list : std::as_const(m_lists))
        list->show();
    for (auto *empty : std::as_const(m_emptyLabels))
        empty->hide();
    m_yt->channelInfo(channelId);
    // Carga perezosa: solo se piden los vídeos al abrir; shorts/directos al
    // entrar en su pestaña (cada respuesta pesa ~400-500 KB).
    m_tabLoaded.fill(0);
    showTab(0);
    loadTab(0);
}

void ChannelPage::loadTab(int index)
{
    if (index < 0 || index >= m_tabKeys.size() || m_channelId.isEmpty())
        return;
    if (m_tabLoaded.at(index))
        return;
    m_tabLoaded[index] = 1;
    NT_DEBUG("E2E channel loadTab id=%s tab=%s\n",
             m_channelId.toUtf8().constData(),
             m_tabKeys.at(index).toUtf8().constData());
    m_yt->channelTabVideos(m_channelId, m_tabKeys.at(index), kTabPage);
}

void ChannelPage::showTab(int index)
{
    if (index < 0 || index >= m_pages.size())
        return;
    m_currentTab = index;
    for (int i = 0; i < m_pages.size(); ++i) {
        m_pages.at(i)->setVisible(i == index);
        auto *btn = m_tabButtons.at(i);
        btn->setProperty("selected", i == index);
        // La propiedad elige el selector del QSS, así que hay que re-polish:
        // sin esto Qt no vuelve a resolver las reglas y el botón se queda con
        // el aspecto del estado anterior.
        btn->style()->unpolish(btn);
        btn->style()->polish(btn);
        btn->update();
    }
    updateTabCount();
}

void ChannelPage::updateTabCount()
{
    const int n = m_currentTab < m_videos.size() ? m_videos.at(m_currentTab).size()
                                                 : 0;
    // Con 0 vídeos no se pone nada: el estado vacío de la pestaña ya lo dice (y
    // el contador se conoce al vuelo, así que solo en un canal de verdad se ve
    // "1 vídeo").
    m_tabCount->setVisible(n > 0);
    m_tabCount->setText(n == 1 ? QStringLiteral("1 vídeo")
                                : QStringLiteral("%1 vídeos").arg(n));
}

void ChannelPage::onChannelInfo(const ChannelInfo &channel)
{
    if (channel.id != m_channelId)
        return;
    m_channelName = channel.name;
    m_title->setText(channel.name);
    // Suscriptores con el mismo formato compacto que usa el resto de la app
    // (compactCount): antes salía el número en crudo ("1234567 suscriptores").
    m_meta->setText(channel.subscriberCount > 0
                        ? QStringLiteral("%1 suscriptores")
                              .arg(compactCount(channel.subscriberCount))
                        : QString());
    if (channel.description.isEmpty()) {
        m_description->setText(QString());
        m_description->hide();
    } else {
        m_description->setText(channel.description);
        m_description->show();
    }
    // Tras el layout: aplicar el tope de líneas y medir la caja en E2E (solo
    // con NEVERTUBE_DEBUG=1). El retardo deja asentar la geometría.
    QTimer::singleShot(120, this, [this]() {
        if (m_channelId.isEmpty())
            return;
        applyDescriptionCap();
        NT_DEBUG("E2E canal descripcion caja: size=%dx%d page=%dx%d fullH=%d "
                 "btn=%d card=%dx%d\n",
                 m_description->width(), m_description->height(), width(),
                 height(), m_description->heightForWidth(m_description->width()),
                 m_descToggle->isVisible() ? 1 : 0, m_card->width(),
                 m_card->height());
    });
    m_avatarKey = QStringLiteral("ch_%1").arg(channel.id);
    if (!channel.avatarUrl.isEmpty())
        m_avatarUrl = channel.avatarUrl;
    const QPixmap cached = m_thumbs->pixmap(m_avatarKey);
    if (!cached.isNull()) {
        setAvatar(cached);
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
        setAvatar(pm);
}

void ChannelPage::setAvatar(const QPixmap &pm)
{
    m_avatar->setPixmap(pm.isNull()
                            ? nt::icon(nt::Icon::User, nt::kMuted, 40)
                                  .pixmap(kAvatar, kAvatar)
                            : nt::circularAvatar(pm, kAvatar));
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
    // El chip de Compartir pinta su icono a mano, y los iconos llevan el color
    // embebido: hay que rehacerlos con el acento/cTexto vigente.
    m_shareBtn->setIcon(nt::icon(nt::Icon::Share, nt::kText, 18));
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
        m_lists.at(index)->showVideos(loaded, VideoListView::ListMode);
    } else {
        m_videos[index] += loaded;
        m_lists.at(index)->appendVideos(loaded);
    }
    // Respuesta vacía = pestaña sin vídeos: sale el estado vacío y la lista se
    // oculta (si no, los dos se partirían la caja con los dos stretch).
    const bool empty = m_videos.at(index).isEmpty();
    m_lists.at(index)->setVisible(!empty);
    m_emptyLabels.at(index)->setVisible(empty);
    m_moreButtons.at(index)->setVisible(!empty && videos.size() >= kTabPage);
    updateTabCount();
}

// Tope de la descripción: si el texto completo no cabe en kDescLines líneas se
// recorta a ese alto, aparece "…más" y un degradado sobre la última línea. Con
// el texto desplegado pasa a "…menos". Se re-evalúa también en el resize (el
// re-wrap por ancho cambia elalto necesario) y al cambiar de canal.
void ChannelPage::applyDescriptionCap()
{
    if (!m_description)
        return;
    if (m_description->text().isEmpty()) {
        m_descToggle->hide();
        if (m_descFade)
            m_descFade->hide();
        m_descCollapsible = false;
        return;
    }
    m_description->setMaximumHeight(QWIDGETSIZE_MAX);
    const int w = m_description->width();
    const int lineH = m_description->fontMetrics().lineSpacing();
    const int maxH = lineH * kDescLines;
    const int fullH = (w > 0 && lineH > 0) ? m_description->heightForWidth(w) : 0;
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
            m_description->setMaximumHeight(maxH);
    }
    positionDescFade();
}

void ChannelPage::toggleDescription()
{
    if (m_description->text().isEmpty())
        return;
    m_descExpanded = !m_descExpanded;
    applyDescriptionCap();
}

// El degradado es un hijo de la caja FUERA del layout (como en el Player), así
// que hay que colocarlo a mano sobre las últimas líneas del texto.
void ChannelPage::positionDescFade()
{
    if (!m_descFade || !m_card)
        return;
    if (!(m_descCollapsible && !m_descExpanded && m_card->isVisible())) {
        m_descFade->hide();
        return;
    }
    const int fh = 26;
    m_descFade->setGeometry(1, qMax(0, m_card->height() - fh - 1),
                            qMax(0, m_card->width() - 2), fh);
    m_descFade->show();
    m_descFade->raise();
    if (m_descToggle)
        m_descToggle->raise();
}

void ChannelPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    applyDescriptionCap();
}