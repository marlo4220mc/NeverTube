#include "channel_page.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <utility>

#include "../../db/repositories.h"
#include "../../debug.h"
#include "../../yt.h"
#include "../icons.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

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
    m_avatar->setStyleSheet(QStringLiteral("border-radius:32px;"));
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
    m_subscribeBtn->setCursor(Qt::PointingHandCursor);
    m_subscribeBtn->setMinimumHeight(36);
    headerRow->addWidget(m_subscribeBtn, 0, Qt::AlignVCenter);

    root->addLayout(headerRow);

    m_description = new QLabel(this);
    m_description->setObjectName(QStringLiteral("sectionSub"));
    m_description->setWordWrap(true);
    root->addWidget(m_description);

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
    if (subscribed)
        m_subscribeBtn->setStyleSheet(
            QStringLiteral("QPushButton { background:#262626;"
                           " border-radius:18px; padding:0 18px;"
                           " color:#ECECEC; font-weight:600; font-size:13px; }"
                           "QPushButton:hover { background:#333333; }"));
    else
        m_subscribeBtn->setStyleSheet(
            QStringLiteral("QPushButton { background:#FF0033;"
                           " border-radius:18px; padding:0 18px;"
                           " color:white; font-weight:600; font-size:13px; }"
                           "QPushButton:hover { background:#FF1A47; }"));
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
