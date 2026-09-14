#include "history_page.h"

#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QToolTip>
#include <QVBoxLayout>

#include "../../db/repositories.h"
#include "../icons.h"
#include "../theme.h"
#include "../video_list_view.h"

namespace {

const int kMaxHistory = 200;
const qreal kHistoryScale = 1.21;

} // namespace

HistoryPage::HistoryPage(HistoryRepo *history, VideoCacheRepo *cache,
                         ThumbnailCache *thumbs, WatchLaterRepo *watchLater,
                         PlaylistsRepo *playlists, QWidget *parent)
    : QWidget(parent)
    , m_history(history)
    , m_cache(cache)
    , m_thumbs(thumbs)
    , m_watchLater(watchLater)
    , m_playlists(playlists)
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
    m_list->setRowMoreVisible(true);
    connect(m_list, &VideoListView::moreRequested, this,
            [this](int index, const VideoInfo &v) {
                if (index < 0 || index >= m_videos.size())
                    return;
                const QPoint pos = m_list->mapToGlobal(
                    m_list->visualRect(m_list->model()->index(index, 0))
                        .bottomRight());
                showMoreMenu(v, pos);
            });
    root->addWidget(m_list, 1);
}

void HistoryPage::showMoreMenu(const VideoInfo &v, const QPoint &globalPos)
{
    QMenu menu(this);

    menu.addAction(QStringLiteral("Quitar del historial"));
    connect(menu.actions().last(), &QAction::triggered, this, [this, v]() {
        m_history->remove(v.id);
        emit historyChanged();
        reload();
    });

    menu.addAction(QStringLiteral("Guardar en Ver más tarde"));
    connect(menu.actions().last(), &QAction::triggered, this, [this, v]() {
        if (!m_watchLater->isWatchLater(v.id))
            m_watchLater->toggle(v.id);
    });

    QMenu *plMenu = new QMenu(&menu);
    plMenu->setTitle(QStringLiteral("Añadir a playlist"));
    for (const PlaylistItem &p : m_playlists->all()) {
        QAction *a = plMenu->addAction(p.title);
        connect(a, &QAction::triggered, this,
                [this, pid = p.id, vid = v.id]() {
                    m_playlists->addVideo(pid, vid);
                });
    }
    plMenu->addSeparator();
    QAction *newAct = plMenu->addAction(QStringLiteral("+ Nueva playlist"));
    connect(newAct, &QAction::triggered, this, [this, v]() {
        bool ok = false;
        const QString title = QInputDialog::getText(
            this, QStringLiteral("Nueva playlist"),
            QStringLiteral("Nombre:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || title.trimmed().isEmpty())
            return;
        const QString id = m_playlists->create(title.trimmed());
        if (!id.isEmpty())
            m_playlists->addVideo(id, v.id);
    });
    menu.addMenu(plMenu);

    menu.addSeparator();

    QAction *shareAct =
        menu.addAction(QStringLiteral("Compartir"));
    connect(shareAct, &QAction::triggered, this, [v]() {
        const QString url =
            QStringLiteral("https://www.youtube.com/watch?v=%1").arg(v.id);
        QGuiApplication::clipboard()->setText(url);
        QToolTip::showText(QCursor::pos(),
                           QStringLiteral("Enlace copiado"));
    });

    if (!v.channelId.isEmpty()) {
        QAction *chanAct = menu.addAction(QStringLiteral("Ver canal"));
        connect(chanAct, &QAction::triggered, this,
                [this, v]() { emit channelRequested(v.channelId); });
    }

    menu.exec(globalPos);
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
