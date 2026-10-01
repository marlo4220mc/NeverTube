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
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include "../../db/repositories.h"
#include "../confirm_dialog.h"
#include "../icons.h"
#include "../row_menu.h"
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
    // Confirmación antes de tirar el historial entero: son hasta kMaxHistory
    // (200) vídeos y no hay vuelta atrás. El diálogo de la app (nt::confirm) y no
    // QMessageBox, por el mismo motivo que el de la página Buscar. El botón
    // decía "Borrar todo" y el diálogo "historial de vídeos": aquí ya hay dos
    // historiales en juego, así que el cuerpo los distingue.
    connect(clearAllBtn, &QPushButton::clicked, this, [this]() {
        if (m_videos.isEmpty())
            return;
        nt::confirm(this, QStringLiteral("Borrar historial de vídeos"),
                    QStringLiteral("Se borrarán los %1 vídeos de tu historial. "
                                   "Esta acción no se puede deshacer.")
                        .arg(m_videos.size()),
                    QStringLiteral("Borrar"), [this]() {
                        m_history->clear();
                        emit historyChanged();
                        reload();
                    });
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
            [this](int index, const VideoInfo &v, const QPoint &pos) {
                if (index < 0 || index >= m_videos.size())
                    return;
                showMoreMenu(v, pos);
            });
    root->addWidget(m_list, 1);
}

void HistoryPage::showMoreMenu(const VideoInfo &v, const QPoint &globalPos)
{
    QMenu menu(this);

    // Acción propia de esta página: el resto del menú es el compartido por
    // todas las superficies con fila (nt::addRowMenuActions), con sus mismos
    // textos y en el mismo orden.
    QAction *rem = menu.addAction(QStringLiteral("Quitar del historial"));
    connect(rem, &QAction::triggered, this, [this, v]() {
        // En el bucle siguiente, con el menú ya cerrado: quitar el vídeo borra
        // la fila de la lista de la que nació el popup.
        QTimer::singleShot(0, this, [this, v]() {
            m_history->remove(v.id);
            emit historyChanged();
            reload();
        });
    });

    nt::RowMenuHooks hooks;
    hooks.onNewPlaylist = [this](const QString &videoId) {
        bool ok = false;
        const QString title = QInputDialog::getText(
            this, QStringLiteral("Nueva playlist"),
            QStringLiteral("Nombre:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || title.trimmed().isEmpty())
            return;
        const QString id = m_playlists->create(title.trimmed());
        if (!id.isEmpty())
            m_playlists->addVideo(id, videoId);
    };
    hooks.onChannel = [this](const QString &channelId, const QString &) {
        emit channelRequested(channelId);
    };
    nt::addRowMenuActions(&menu, v, m_watchLater, m_playlists, hooks);

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
        const double frac =
            HistoryRepo::progressFraction(v, m_history->positionOf(v.id));
        if (frac > 0.0)
            m_list->setProgress(v.id, frac);
    }
}
