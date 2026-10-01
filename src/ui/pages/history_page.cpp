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
    root->addLayout(top);

    // Título y acción en la misma fila, con la acción a la derecha. Es la misma
    // cabecera que se puso en la página Buscar ("Borrar historial de
    // búsquedas"), y por los mismos dos motivos que se documentan allí: al pie
    // de la lista la cápsula se leía como un control suelto de la página (las
    // filas no tienen caja) y como enlace suelto se perdía, porque es el
    // elemento más apagado de la página y la acción además es destructiva.
    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(8);

    m_title = new QLabel(QStringLiteral("Historial"), this);
    m_title->setObjectName(QStringLiteral("sectionTitle"));
    titleRow->addWidget(m_title, 0, Qt::AlignVCenter);
    titleRow->addStretch(1);

    auto *clearAllBtn = new QPushButton(QStringLiteral("Borrar historial de vídeos"),
                                        this);
    clearAllBtn->setObjectName(QStringLiteral("ghost"));
    clearAllBtn->setCursor(Qt::PointingHandCursor);
    // 30 y no 34: es un control de cabecera, no el botón grande de la página. El
    // padding global (7px 14px) más 13 px de línea entran justos sin recortar.
    clearAllBtn->setFixedHeight(30);
    // Sin icono, como el de la página Buscar: el de "Clear" (una X) con una
    // etiqueta que ya dice "borrar" era redundante, y con el texto largo
    // empujaba el ancho.
    //
    // Etiqueta larga a propósito ("Borrar historial de vídeos", no "Borrar
    // todo"): la app tiene dos historiales, este de vídeos vistos y el de
    // consultas en la página Buscar. A solas, "Borrar todo" no decía cuál.
    // Confirmación antes de tirar el historial entero: son hasta kMaxHistory
    // (200) vídeos y no hay vuelta atrás. El diálogo de la app (nt::confirm) y no
    // QMessageBox, por el mismo motivo que el de la página Buscar.
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
    titleRow->addWidget(clearAllBtn, 0, Qt::AlignVCenter);
    root->addLayout(titleRow);

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
