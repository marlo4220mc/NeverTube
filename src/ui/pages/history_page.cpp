#include "history_page.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "../../db/repositories.h"
#include "../icons.h"
#include "../theme.h"
#include "../video_list_view.h"

namespace {

// Historial completo: tope alto para no cargar listas enormes en el arranque.
const int kMaxHistory = 200;
const qreal kHistoryScale = 1.21;

} // namespace

HistoryPage::HistoryPage(HistoryRepo *history, VideoCacheRepo *cache,
                         ThumbnailCache *thumbs, QWidget *parent)
    : QWidget(parent)
    , m_history(history)
    , m_cache(cache)
    , m_thumbs(thumbs)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *top = new QHBoxLayout;
    auto *back = new QPushButton(QStringLiteral("Volver"), this);
    back->setObjectName(QStringLiteral("ghost"));
    connect(back, &QPushButton::clicked, this, &HistoryPage::backRequested);
    top->addWidget(back);
    top->addStretch(1);
    auto *clearAllBtn = new QPushButton(QStringLiteral("Borrar todo"), this);
    clearAllBtn->setObjectName(QStringLiteral("ghost"));
    clearAllBtn->setIcon(nt::icon(nt::Icon::Clear, nt::kText, 15));
    connect(clearAllBtn, &QPushButton::clicked, this, [this]() {
        m_history->clear();
        emit historyChanged();
        reload();
    });
    top->addWidget(clearAllBtn);
    root->addLayout(top);

    m_title = new QLabel(QStringLiteral("Historial"), this);
    m_title->setObjectName(QStringLiteral("sectionTitle"));
    root->addWidget(m_title);

    m_meta = new QLabel(this);
    m_meta->setObjectName(QStringLiteral("sectionSub"));
    root->addWidget(m_meta);

    m_list = new VideoListView(m_thumbs, this);
    m_list->setScale(kHistoryScale);
    connect(m_list, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                if (index < 0 || index >= m_videos.size())
                    return;
                m_list->setCurrentRow(index);
                const VideoInfo &v = m_videos.at(index);
                emit resumeRequested(
                    m_videos, index,
                    HistoryRepo::resumeStartMs(
                        v, m_history->positionOf(v.id)));
            });
    m_list->setRowRemoveVisible(true);
    connect(m_list, &VideoListView::removeRequested, this,
            [this](int, const VideoInfo &v) {
                if (v.id.isEmpty())
                    return;
                m_history->remove(v.id);
                emit historyChanged();
                reload();
            });
    root->addWidget(m_list, 1);
}

void HistoryPage::reload()
{
    const QStringList ids = m_history->recentIds(kMaxHistory);
    m_videos = m_cache->getMany(ids);
    m_list->showVideos(m_videos, VideoListView::ListMode);
    m_list->setCurrentRow(-1);
    m_meta->setText(QStringLiteral("%1 vídeos").arg(m_videos.size()));
    applyProgress();
}

void HistoryPage::applyProgress()
{
    for (const VideoInfo &v : m_videos) {
        const qlonglong pos = m_history->positionOf(v.id);
        if (pos <= 0 || v.durationSeconds <= 0)
            continue;
        if (HistoryRepo::isCompleted(v, pos))
            continue;
        const double frac = double(pos) / (double(v.durationSeconds) * 1000.0);
        if (frac > 0.0 && frac < 1.0)
            m_list->setProgress(v.id, frac);
    }
}