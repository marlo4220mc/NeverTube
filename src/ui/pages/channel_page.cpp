#include "channel_page.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <utility>

#include "../../yt.h"
#include "../video_list_view.h"
#include "../thumbnail_cache.h"

ChannelPage::ChannelPage(Yt *yt, ThumbnailCache *thumbs, QWidget *parent)
    : QWidget(parent)
    , m_yt(yt)
    , m_thumbs(thumbs)
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

    m_title = new QLabel(QStringLiteral("Canal"), this);
    m_title->setObjectName(QStringLiteral("sectionTitle"));
    root->addWidget(m_title);

    m_meta = new QLabel(this);
    m_meta->setObjectName(QStringLiteral("sectionSub"));
    root->addWidget(m_meta);

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
        tabLayout->setContentsMargins(0, 0, 0, 0);
        tabLayout->setSpacing(6);
        auto *list = new VideoListView(m_thumbs, tabWrap);
        list->showVideos(QVector<VideoInfo>(), VideoListView::ListMode);
        m_lists.append(list);
        m_videos.append(QVector<VideoInfo>());
        m_tabKeys.append(keys.at(i));
        tabLayout->addWidget(list, 1);
        auto *more = new QPushButton(QStringLiteral("Mostrar más"), tabWrap);
        more->setObjectName(QStringLiteral("ghost"));
        more->setCursor(Qt::PointingHandCursor);
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
    connect(m_yt, &Yt::channelVideosFinished, this, &ChannelPage::onVideos);
    connect(m_yt, &Yt::channelTabVideosFinished, this,
            &ChannelPage::onTabVideos);
}

void ChannelPage::openChannel(const QString &channelId)
{
    m_channelId = channelId;
    m_channelName.clear();
    m_title->setText(QStringLiteral("Cargando canal…"));
    m_meta->clear();
    m_description->clear();
    for (auto *list : std::as_const(m_lists))
        list->clearVideos();
    for (auto &videos : m_videos)
        videos.clear();
    for (auto *button : std::as_const(m_moreButtons)) {
        button->setVisible(false);
        button->setEnabled(true);
    }
    m_yt->channelInfo(channelId);
    m_yt->channelTabVideos(channelId, QStringLiteral("videos"), 25);
    m_yt->channelTabVideos(channelId, QStringLiteral("shorts"), 25);
    m_yt->channelTabVideos(channelId, QStringLiteral("live"), 25);
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
}

void ChannelPage::onVideos(const QVector<VideoInfo> &videos)
{
    onTabVideos(QStringLiteral("videos"), videos);
}

void ChannelPage::onTabVideos(const QString &tab, const QVector<VideoInfo> &videos)
{
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
