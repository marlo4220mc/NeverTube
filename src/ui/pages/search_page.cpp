#include "search_page.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include "../../debug.h"
#include "../../db/repositories.h"
#include "../../models.h"
#include "../../yt.h"
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
    outer->setSpacing(10);

    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    m_field = new QLineEdit(this);
    m_field->setPlaceholderText(QStringLiteral("Buscar en YouTube…"));
    m_field->setClearButtonEnabled(true);
    m_field->setObjectName(QStringLiteral("search"));
    auto *searchBtn = new QPushButton(this);
    searchBtn->setIcon(nt::icon(nt::Icon::Search, nt::kText, 18));
    row->addWidget(m_field, 1);
    row->addWidget(searchBtn);
    outer->addLayout(row);

    m_historyBox = new QWidget(this);
    auto *hBox = new QVBoxLayout(m_historyBox);
    hBox->setContentsMargins(0, 8, 0, 0);
    hBox->setSpacing(2);
    outer->addWidget(m_historyBox);

    m_resultBox = new QWidget(this);
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
    m_list = new VideoListView(m_thumbs, this);
    m_list->showVideos(QVector<VideoInfo>(), VideoListView::ListMode);
    rBox->addWidget(spinner);
    rBox->addWidget(empty);
    rBox->addWidget(m_list);
    outer->addWidget(m_resultBox);
    m_resultBox->hide();

    connect(m_field, &QLineEdit::textChanged, this,
            [this](const QString &) { m_debounce->start(); });
    connect(m_field, &QLineEdit::returnPressed, this, &SearchPage::doSearch);
    connect(searchBtn, &QPushButton::clicked, this, &SearchPage::doSearch);
    connect(m_yt, &Yt::searchFinished, this, &SearchPage::onSearchReady);

    connect(m_list, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                emit openRequested(m_results, index);
            });

    showHistory();
}

void SearchPage::startSearch(const QString &query)
{
    m_field->setText(query);
    doSearch();
}

void SearchPage::doSearch()
{
    const QString q = m_field->text().trimmed();
    if (q.isEmpty()) {
        m_resultBox->hide();
        showHistory();
        return;
    }
    m_searching = true;
    m_list->clearVideos();
    m_historyBox->hide();
    m_resultBox->show();
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

void SearchPage::onSearchReady(const QVector<VideoInfo> &videos)
{
    m_searching = false;
    NT_DEBUG("E2E searchReady count=%d\n", (int)videos.size());
    for (auto *child : m_resultBox->findChildren<QLabel *>()) {
        if (child->text().startsWith(QStringLiteral("Buscando")))
            child->hide();
    }
    m_results = videos;
    if (m_results.isEmpty()) {
        m_list->clearVideos();
        for (auto *child : m_resultBox->findChildren<QLabel *>()) {
            if (child->text().startsWith(QStringLiteral("Sin")))
                child->show();
        }
    } else {
        for (auto *child : m_resultBox->findChildren<QLabel *>()) {
            if (child->text().startsWith(QStringLiteral("Sin")))
                child->hide();
        }
        m_list->showVideos(m_results, VideoListView::ListMode);
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
        return;
    }
    m_historyBox->show();
    auto *hTitle = new QLabel(QStringLiteral("Búsquedas recientes"), this);
    hTitle->setObjectName(QStringLiteral("sectionSub"));
    m_historyBox->layout()->addWidget(hTitle);

    for (const QString &q : hist) {
        auto *btn = new QPushButton(QStringLiteral("  %1").arg(q), this);
        btn->setIcon(nt::icon(nt::Icon::Search, nt::kMuted, 14));
        btn->setStyleSheet(
            QStringLiteral("QPushButton { text-align:left; background:transparent; "
                           "border:none; color:#D6D6D6; padding:9px 4px; }"
                           "QPushButton:hover { background:#1F1F1F; border-radius:6px; }"));
        connect(btn, &QPushButton::clicked, this,
                [this, q]() { onHistoryQuery(q); });
        m_historyBox->layout()->addWidget(btn);
    }
    auto *clear = new QPushButton(QStringLiteral("  Borrar historial"), this);
    clear->setIcon(nt::icon(nt::Icon::Clear, nt::kMuted, 14));
    clear->setStyleSheet(
        QStringLiteral("QPushButton { text-align:left; background:transparent; "
                       "border:none; color:#8F8F8F; padding:9px 4px; font-size:12px; }"
                       "QPushButton:hover { color:#ECECEC; background:#1F1F1F; "
                       "border-radius:6px; }"));
    connect(clear, &QPushButton::clicked, this, &SearchPage::onHistoryClear);
    m_historyBox->layout()->addWidget(clear);
}

void SearchPage::onHistoryClear()
{
    setHistory(QStringList());
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