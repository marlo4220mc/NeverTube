#include "search_page.h"

#include <QBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QRegularExpression>

#include <utility>

#include "../../debug.h"
#include "../../db/repositories.h"
#include "../../models.h"
#include "../../yt.h"
#include "../channel_row.h"
#include "../format.h"
#include "../icons.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

SearchPage::SearchPage(Yt *yt, SettingsRepo *settings, ThumbnailCache *thumbs,
                       QWidget *parent)
    : QWidget(parent)
    , m_yt(yt)
    , m_settings(settings)
    , m_thumbs(thumbs)
{
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(600);
    connect(m_debounce, &QTimer::timeout, this, &SearchPage::doSearch);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(6);

    auto *title = new QLabel(QStringLiteral("Buscar"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    title->setFixedHeight(20);
    outer->addWidget(title);

    auto *searchCard = new QFrame(this);
    searchCard->setObjectName(QStringLiteral("cardPanel"));
    searchCard->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *searchRow = new QHBoxLayout(searchCard);
    searchRow->setContentsMargins(10, 3, 10, 3);
    searchRow->setSpacing(8);
    auto *searchIcon = new QLabel(searchCard);
    searchIcon->setPixmap(nt::icon(nt::Icon::Search, nt::kMuted, 18).pixmap(20, 20));
    m_field = new QLineEdit(searchCard);
    m_field->setPlaceholderText(QStringLiteral("Buscar en YouTube…"));
    m_field->setFixedHeight(32);
    m_clearBtn = new QPushButton(QStringLiteral("✕"), searchCard);
    m_clearBtn->setObjectName(QStringLiteral("ghost"));
    m_clearBtn->setCursor(Qt::PointingHandCursor);
    m_clearBtn->setFixedSize(26, 26);
    m_clearBtn->setFocusPolicy(Qt::NoFocus);
    m_clearBtn->hide();
    auto *searchBtn = new QPushButton(QStringLiteral("Buscar"), searchCard);
    searchBtn->setObjectName(QStringLiteral("primary"));
    searchBtn->setCursor(Qt::PointingHandCursor);
    searchBtn->setFixedHeight(32);
    searchRow->addWidget(searchIcon);
    searchRow->addWidget(m_field, 1);
    searchRow->addWidget(m_clearBtn);
    searchRow->addWidget(searchBtn);
    outer->addWidget(searchCard);

    auto syncClear = [this](const QString &text) {
        m_clearBtn->setVisible(!text.isEmpty());
    };
    connect(m_field, &QLineEdit::textChanged, this, syncClear);
    connect(m_clearBtn, &QPushButton::clicked, this, [this]() {
        m_field->clear();
        m_field->setFocus();
    });

    m_historyBox = new QWidget(this);
    auto *hBox = new QVBoxLayout(m_historyBox);
    hBox->setContentsMargins(0, 4, 0, 0);
    hBox->setSpacing(2);
    outer->addWidget(m_historyBox);

    m_resultBox = new QWidget(this);
    m_resultBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto *rBox = new QVBoxLayout(m_resultBox);
    rBox->setContentsMargins(0, 0, 0, 0);
    rBox->setSpacing(8);
    auto *spinner = new QLabel(QStringLiteral("Buscando…"), this);
    spinner->setObjectName(QStringLiteral("sectionSub"));
    spinner->setAlignment(Qt::AlignCenter);
    spinner->setContentsMargins(0, 40, 0, 0);
    spinner->hide();
    auto *empty = new QLabel(QStringLiteral("Sin resultados."), this);
    empty->setObjectName(QStringLiteral("sectionSub"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setContentsMargins(0, 40, 0, 0);
    empty->hide();
    m_channelBox = new QWidget(this);
    auto *channelLayout = new QVBoxLayout(m_channelBox);
    channelLayout->setContentsMargins(0, 0, 0, 0);
    channelLayout->setSpacing(2);
    m_channelBox->hide();
    m_list = new VideoListView(m_thumbs, this);
    m_list->showVideos(QVector<VideoInfo>(), VideoListView::ListMode);
    m_moreButton = new QPushButton(QStringLiteral("Mostrar más"), this);
    m_moreButton->setObjectName(QStringLiteral("primary"));
    m_moreButton->setCursor(Qt::PointingHandCursor);
    m_moreButton->hide();
    m_moreButton->setFixedWidth(180);
    connect(m_moreButton, &QPushButton::clicked, this, [this]() {
        m_moreButton->setEnabled(false);
        m_yt->search(m_activeQuery, 20, m_results.size());
    });
    rBox->addWidget(spinner);
    rBox->addWidget(empty);
    rBox->addWidget(m_channelBox);
    rBox->addWidget(m_list, 1);
    rBox->addWidget(m_moreButton, 0, Qt::AlignHCenter);
    outer->addWidget(m_resultBox, 1);
    m_resultBox->hide();
    m_bottomStretch = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
    outer->addSpacerItem(m_bottomStretch);

    connect(m_field, &QLineEdit::textChanged, this,
            [this](const QString &) { m_debounce->start(); });
    connect(m_field, &QLineEdit::returnPressed, this, &SearchPage::doSearch);
    connect(searchBtn, &QPushButton::clicked, this, &SearchPage::doSearch);
    connect(m_yt, &Yt::searchFinished, this, &SearchPage::onSearchReady);
    connect(m_yt, &Yt::channelInfoReady, this, &SearchPage::onChannelInfo);

    connect(m_list, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                emit openRequested(m_results, index);
            });

    showHistory();
}

void SearchPage::startSearch(const QString &query)
{
    const QSignalBlocker blocker(m_field);
    m_field->setText(query);
    m_clearBtn->setVisible(!query.trimmed().isEmpty());
    doSearch();
}

void SearchPage::setResultsMode(bool active)
{
    if (m_resultBox->isVisible() == active)
        return;
    m_resultBox->setVisible(active);
    m_historyBox->setVisible(!active);
    if (m_bottomStretch) {
        m_bottomStretch->changeSize(0, 0, QSizePolicy::Minimum,
                                    active ? QSizePolicy::Fixed
                                           : QSizePolicy::Expanding);
        if (auto *lay = qobject_cast<QBoxLayout *>(layout()))
            lay->invalidate();
    }
    updateGeometry();
}

void SearchPage::doSearch()
{
    m_debounce->stop();
    const QString q = m_field->text().trimmed();
    if (q.isEmpty()) {
        setResultsMode(false);
        showHistory();
        return;
    }
    setResultsMode(true);
    m_searching = true;
    m_activeQuery = q;
    m_results.clear();
    m_moreButton->hide();
    m_list->clearVideos();
    while (QLayoutItem *item = m_channelBox->layout()->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_channelButtons.clear();
    m_channels.clear();
    m_channelBox->hide();
    m_list->showVideos(QVector<VideoInfo>(), VideoListView::ListMode);
    for (auto *child : m_resultBox->findChildren<QLabel *>()) {
        if (child->objectName() == QLatin1String("sectionSub")) {
            if (child->text().startsWith(QStringLiteral("Buscando")))
                child->show();
            else if (child->text().startsWith(QStringLiteral("Sin")))
                child->hide();
        }
    }
    m_error.clear();

    QStringList hist = history();
    hist.removeAll(q);
    hist.prepend(q);
    while (hist.size() > 8)
        hist.removeLast();
    setHistory(hist);

    m_yt->search(q);
}

void SearchPage::onSearchReady(const QString &query, const QVector<VideoInfo> &videos)
{
    if (query != m_activeQuery)
        return;
    m_searching = false;
    NT_DEBUG("E2E searchReady count=%d\n", (int)videos.size());
    for (auto *child : m_resultBox->findChildren<QLabel *>()) {
        if (child->text().startsWith(QStringLiteral("Buscando")))
            child->hide();
    }
    if (m_results.isEmpty())
        m_results = videos;
    else
        m_results += videos;
    if (m_results.isEmpty()) {
        m_list->clearVideos();
        m_moreButton->hide();
        for (auto *child : m_resultBox->findChildren<QLabel *>()) {
            if (child->text().startsWith(QStringLiteral("Sin")))
                child->show();
        }
        return;
    }
    for (auto *child : m_resultBox->findChildren<QLabel *>()) {
        if (child->text().startsWith(QStringLiteral("Sin")))
            child->hide();
    }
    if (m_list->isEmpty()) {
        buildChannelButtons();
        m_list->showVideos(m_results, VideoListView::ListMode);
    } else {
        m_list->appendVideos(videos);
    }
    m_moreButton->setVisible(videos.size() >= 20);
    m_moreButton->setEnabled(true);
}

void SearchPage::buildChannelButtons()
{
    while (QLayoutItem *item = m_channelBox->layout()->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_channels.clear();
    m_channelButtons.clear();
    QSet<QString> seenChannels;
    for (const VideoInfo &video : m_results) {
        if (video.channelId.isEmpty() || seenChannels.contains(video.channelId))
            continue;
        seenChannels.insert(video.channelId);
        ChannelInfo channel;
        channel.id = video.channelId;
        channel.name = video.channel;
        m_channels.append(channel);
    }
    if (m_channels.isEmpty()) {
        m_channelBox->hide();
        return;
    }
    auto *title = new QLabel(QStringLiteral("Canales"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    m_channelBox->layout()->addWidget(title);
    auto *scroll = new QScrollArea(m_channelBox);
    scroll->setWidgetResizable(false);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    // El scrollbar horizontal está dentro del área y consume altura del
    // viewport: el mínimo debe incluir esa franja (igual que en la
    // Biblioteca), o la tarjeta quedará recortada por abajo.
    scroll->setMinimumHeight(
        kChannelRowHeight + scroll->horizontalScrollBar()->sizeHint().height());
    auto *strip = new QWidget(scroll);
    m_channelStrip = strip;
    strip->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto *row = new QHBoxLayout(strip);
    row->setContentsMargins(0, 4, 0, 0);
    row->setSpacing(14);
    row->setAlignment(Qt::AlignVCenter);
    strip->setLayout(row);
    scroll->setWidget(strip);
    for (const ChannelInfo &channel : std::as_const(m_channels)) {
        auto *card = new ChannelRow(m_thumbs, channel.id, QString(), strip);
        card->setTitle(channel.name);
        card->setTitleLines(1);
        card->setMeta(channelHandleFor(channel.name));
        card->applyContentWidth();
        card->setActivate(
            [this, channel]() { emit channelRequested(channel.id); });
        row->addWidget(card);
        m_channelButtons.insert(channel.id, card);
        m_yt->channelInfo(channel.id);
    }
    row->addStretch(1);
    m_channelBox->layout()->addWidget(scroll);
    m_channelBox->show();
    // El QScrollArea (widgetResizable=false) dimensiona el anfitrión a partir
    // de su layout; sin activarlo/adjustSize la franja colapsa a ancho 0
    // (igual que hace la Biblioteca con updateSubsRowGeometry).
    QTimer::singleShot(0, this, [strip]() {
        if (auto *lay = strip->layout()) {
            lay->invalidate();
            lay->activate();
        }
        strip->adjustSize();
    });
}

void SearchPage::onChannelInfo(const ChannelInfo &channel)
{
    ChannelRow *card = m_channelButtons.value(channel.id, nullptr);
    if (!card)
        return;
    card->setTitle(channel.name);
    if (!channel.avatarUrl.isEmpty())
        card->setAvatarUrl(channel.avatarUrl);
    QString meta = channelHandleFor(channel.name);
    if (channel.subscriberCount > 0)
        meta = QStringLiteral("%1\n%2 suscriptores")
                   .arg(meta, compactCount(channel.subscriberCount));
    card->setMeta(meta);
    card->applyContentWidth();
    if (m_channelStrip && m_channelStrip->layout()) {
        m_channelStrip->layout()->invalidate();
        m_channelStrip->layout()->activate();
        m_channelStrip->adjustSize();
    }
}

void SearchPage::showHistory()
{
    QLayoutItem *item;
    while ((item = m_historyBox->layout()->takeAt(0))) {
        delete item->widget();
        delete item;
    }
    const QStringList hist = history();
    if (hist.isEmpty()) {
        m_historyBox->hide();
        if (m_bottomStretch) {
            m_bottomStretch->changeSize(0, 0, QSizePolicy::Minimum,
                                        QSizePolicy::Expanding);
            if (auto *lay = qobject_cast<QBoxLayout *>(layout()))
                lay->invalidate();
        }
        return;
    }
    m_historyBox->show();
    auto *hTitle = new QLabel(QStringLiteral("Búsquedas recientes"), this);
    hTitle->setObjectName(QStringLiteral("sectionSub"));
    m_historyBox->layout()->addWidget(hTitle);

    for (const QString &q : hist) {
        auto *row = new QWidget(m_historyBox);
        auto *rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(0, 0, 0, 0);
        rowLay->setSpacing(4);
        auto *btn = new QPushButton(QStringLiteral("  %1").arg(q), row);
        btn->setIcon(nt::icon(nt::Icon::Search, nt::kMuted, 14));
        btn->setStyleSheet(
            QStringLiteral("QPushButton { text-align:left; background:transparent; "
                           "border:none; color:#D6D6D6; padding:7px 4px; }"
                           "QPushButton:hover { background:#1F1F1F; border-radius:6px; }"));
        connect(btn, &QPushButton::clicked, this,
                [this, q]() { onHistoryQuery(q); });
        auto *del = new QPushButton(row);
        del->setObjectName(QStringLiteral("iconBtn"));
        del->setIcon(nt::icon(nt::Icon::Clear, nt::kMuted, 14));
        del->setToolTip(QStringLiteral("Borrar esta búsqueda"));
        del->setFixedSize(28, 28);
        del->setCursor(Qt::PointingHandCursor);
        connect(del, &QPushButton::clicked, this,
                [this, q]() { onHistoryRemove(q); });
        rowLay->addWidget(btn, 1);
        rowLay->addWidget(del);
        m_historyBox->layout()->addWidget(row);
    }
    auto *clear = new QPushButton(QStringLiteral("Borrar historial"), this);
    clear->setObjectName(QStringLiteral("primary"));
    clear->setCursor(Qt::PointingHandCursor);
    clear->setFixedWidth(180);
    connect(clear, &QPushButton::clicked, this, &SearchPage::onHistoryClear);
    if (auto *box = qobject_cast<QBoxLayout *>(m_historyBox->layout()))
        box->addWidget(clear, 0, Qt::AlignHCenter);
    if (auto *box = qobject_cast<QBoxLayout *>(m_historyBox->layout()))
        box->addStretch(1);
}

void SearchPage::onHistoryClear()
{
    setHistory(QStringList());
    showHistory();
}

void SearchPage::onHistoryRemove(const QString &query)
{
    QStringList hist = history();
    hist.removeAll(query);
    setHistory(hist);
    showHistory();
}

void SearchPage::onHistoryQuery(const QString &query)
{
    startSearch(query);
}

QStringList SearchPage::history() const
{
    if (!m_settings)
        return QStringList();
    QString blob;
    if (!m_settings->get(QStringLiteral("search_history"), blob))
        return QStringList();
    const QJsonArray arr =
        QJsonDocument::fromJson(blob.toUtf8()).array();
    QStringList list;
    list.reserve(arr.size());
    for (const auto &v : arr)
        list << v.toString();
    return list;
}

void SearchPage::setHistory(const QStringList &queries)
{
    if (!m_settings)
        return;
    QJsonArray arr;
    for (const QString &q : queries)
        arr.append(q);
    m_settings->set(QStringLiteral("search_history"),
                    QString::fromUtf8(QJsonDocument(arr).toJson()));
}