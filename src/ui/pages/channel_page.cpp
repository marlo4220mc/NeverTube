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

// Avatar of the card: 80 px and circular. It used to be 64 px with
// setScaledContents, which distorts any non-square avatar and also painted it
// in a square (the QSS border-radius only rounds the background, not the
// pixmap).
const int kAvatar = 80;
// Description lines before collapsing. The Player caps at 3 (kDescLines in
// watch_page.cpp) and here the same cut is adopted: the pixel cap this page
// used depended on the font line spacing, so it changed from machine to
// machine.
const int kDescLines = 3;
// Videos per page. It also decides whether there is more to load: if the
// response comes back full, there is a next page.
const int kTabPage = 25;

// Empty-state text for a tab without videos. Each tab has its own word, so
// you can tell which one was opened.
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

    // Same "Volver" on the four surfaces with a way back (History, Playlist
    // and the Player): the #backLink token, a discreet link rather than a
    // capsule. It is navigation, not a page control.
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

    // --- Channel card ------------------------------------------------------
    // #channelCard box: avatar, name, subscribers and the two actions (the
    // Suscribirse pill and the Compartir chip), with the description below.
    // All of that used to sit loose on the page background.
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
    // RichText with links, like the Player description: each URL goes in an
    // <a> of the accent color (format::descriptionHtml) and opens externally.
    m_description->setTextFormat(Qt::RichText);
    m_description->setTextInteractionFlags(Qt::TextSelectableByMouse
                                           | Qt::LinksAccessibleByMouse);
    m_description->setOpenExternalLinks(true);
    m_description->hide();
    cardLay->addWidget(m_description);

    // Gradient over the last lines while the description is collapsed: it is
    // the same #descFade as the Player card (and #181818 at the bottom, the
    // same as the box, so it blends seamlessly).
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

    // --- Tabs -------------------------------------------------------------
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
        // Per-row options button (same rule as Search): the page re-emits the
        // video and it is MainWindow that owns the repos and shows the menu.
        list->setRowMoreVisible(true);
        pageLay->addWidget(list, 1);

        // Tab empty state (dotted box, the same #emptyState as Library). It
        // only shows when the response arrived empty: that is why the list is
        // hidden at the same time (the two with stretch would split the box).
        // The FIXED height of 96 px matters: measured, a label with stretch 0
        // inside a QVBoxLayout whose only stretch item is hidden stretches to
        // the FULL height of the area (700 px of dotted box); with a fixed
        // height it stays as a vertically centered band (y=302 of 700, full
        // width).
        auto *empty = new QLabel(nt::emptyStateHtml(emptyTextFor(keys.at(i))),
                                 page);
        empty->setObjectName(QStringLiteral("emptyState"));
        empty->setAlignment(Qt::AlignCenter);
        empty->setFixedHeight(116);
        empty->hide();
        pageLay->addWidget(empty);

        // Pagination: a centered ghost. The button used to be #primary with
        // 180 px fixed (a wide red bar under the list); "load more" is not the
        // main action of the page, it is a help to go further down.
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
    m_descText.clear();
    m_description->hide();
    m_description->setMaximumHeight(QWIDGETSIZE_MAX);
    // New description: always starts collapsed with the cap applied (if the
    // text does not fit it is re-evaluated in onChannelInfo).
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
    // Each tab falls back to its list: if the previous one was empty, its
    // empty state had hidden the list and without this the slot stays black
    // until the new channel's response arrives.
    for (auto *list : std::as_const(m_lists))
        list->show();
    for (auto *empty : std::as_const(m_emptyLabels))
        empty->hide();
    m_yt->channelInfo(channelId);
    // Lazy loading: only the videos are requested on open; shorts/live when
    // entering their tab (each response weighs ~400-500 KB).
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
        // The property picks the QSS selector, so a re-polish is required:
        // without it Qt does not re-resolve the rules and the button keeps the
        // appearance of the previous state.
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
    // With 0 videos nothing is shown: the tab's empty state already says it
    // (and the counter is computed on the fly, so only a real channel shows
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
    // Subscribers with the same compact format the rest of the app uses
    // (compactCount): it used to print the raw number ("1234567 suscriptores").
    m_meta->setText(channel.subscriberCount > 0
                        ? QStringLiteral("%1 suscriptores")
                              .arg(compactCount(channel.subscriberCount))
                        : QString());
    if (channel.description.isEmpty()) {
        m_descText.clear();
        m_description->setText(QString());
        m_description->hide();
    } else {
        // Same HTML as the Player description: the plain text is stored apart
        // (m_descText) for the empty check of the line cap, which must not
        // depend on the rendered HTML.
        m_descText = channel.description;
        m_description->setText(descriptionHtml(channel.description));
        m_description->show();
    }
    // After the layout: apply the line cap and measure the box in E2E (only
    // with NEVERTUBE_DEBUG=1). The delay lets the geometry settle.
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
    // The look of the two states lives in the global QSS (QPushButton#subscribe
    // and #subscribed) and the boolean `subscribed` property tells them apart.
    // The accent is set by nt::styleSheet(), which is regenerated whole on every
    // change, so there is no need to repaint the button by hand anymore.
    m_subscribeBtn->setProperty("subscribed", subscribed);
    // The property changes the QSS selector, so a re-polish is required:
    // without it Qt does not re-resolve the rules and the button keeps the
    // appearance of the previous state.
    m_subscribeBtn->style()->unpolish(m_subscribeBtn);
    m_subscribeBtn->style()->polish(m_subscribeBtn);
    m_subscribeBtn->update();
}

// Re-applies the Suscribirse button with the current accent (called from
// MainWindow when the accent color changes in Ajustes).
void ChannelPage::applyAccent()
{
    updateSubscribeButton(m_subs->isSubscribed(m_channelId));
    // The Compartir chip paints its icon by hand, and icons carry their color
    // embedded: they must be rebuilt with the current accent/kText.
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
    // Empty response = tab without videos: the empty state shows and the list
    // is hidden (otherwise the two would split the box with the two stretches).
    const bool empty = m_videos.at(index).isEmpty();
    m_lists.at(index)->setVisible(!empty);
    m_emptyLabels.at(index)->setVisible(empty);
    m_moreButtons.at(index)->setVisible(!empty && videos.size() >= kTabPage);
    updateTabCount();
}

// Description cap: if the full text does not fit in kDescLines lines it is cut
// to that height, "…más" appears and a gradient shows over the last line. With
// the text expanded it becomes "…menos". It is also re-evaluated on resize (the
// re-wrap by width changes the required height) and when switching channel.
void ChannelPage::applyDescriptionCap()
{
    if (!m_description)
        return;
    if (m_descText.isEmpty()) {
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
    if (m_descText.isEmpty())
        return;
    m_descExpanded = !m_descExpanded;
    applyDescriptionCap();
}

// The gradient is a child of the box OUTSIDE the layout (as in the Player), so
// it must be positioned by hand over the last lines of the text.
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