#include "history_page.h"

#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include "../../db/repositories.h"
#include "../../i18n.h"
#include "../confirm_dialog.h"
#include "../row_menu.h"
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

    // "Volver" with the #backLink token, same as Channel, Playlist and the Player.
    auto *top = new QHBoxLayout;
    auto *back = new QPushButton(this);
    back->setObjectName(QStringLiteral("backLink"));
    nt::bindText(back, "common.back");
    back->setIcon(nt::icon(nt::Icon::Back, nt::kMuted, 13));
    connect(back, &QPushButton::clicked, this, &HistoryPage::backRequested);
    top->addWidget(back);
    top->addSpacing(4);
    top->addStretch(1);
    root->addLayout(top);

    // Title and action in the same row, with the action on the right. It is the
    // same header used on the Search page ("Borrar historial de
    // búsquedas"), and for the same two reasons documented there: at the foot of
    // the list the capsule read as a loose control of the page (the rows have no
    // box) and as a loose link it got lost, because it is the dimmest element of
    // the page and the action is destructive on top of that.
    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(8);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("sectionTitle"));
    nt::bindText(m_title, "history.title");
    titleRow->addWidget(m_title, 0, Qt::AlignVCenter);
    titleRow->addStretch(1);

    m_clearBtn = new QPushButton(this);
    m_clearBtn->setObjectName(QStringLiteral("ghost"));
    nt::bindText(m_clearBtn, "history.clear");
    m_clearBtn->setCursor(Qt::PointingHandCursor);
    // 30 and not 34: it is a header control, not the big button of the page. The
    // global padding (7px 14px) plus the 13 px line fit exactly without clipping.
    m_clearBtn->setFixedHeight(30);
    // No icon, like the one on the Search page: the "Clear" one (an X) with a
    // label that already says "borrar" was redundant, and with the long text it
    // pushed the width.
    //
    // Long label on purpose ("Borrar historial de vídeos", not "Borrar
    // todo"): the app has two histories, this one of watched videos and the one
    // of queries on the Search page. On its own, "Borrar todo" did not say which
    // one. Confirmation before wiping the whole history: it is up to kMaxHistory
    // (200) videos and there is no way back. The app's dialog (nt::confirm) and not
    // QMessageBox, for the same reason as the one on the Search page.
    connect(m_clearBtn, &QPushButton::clicked, this, [this]() {
        if (m_videos.isEmpty())
            return;
        nt::confirm(this, nt::tr("history.clear"),
                    nt::tr("history.clearBody").arg(m_videos.size()),
                    nt::tr("common.erase"), [this]() {
                        m_history->clear();
                        emit historyChanged();
                        reload();
                    });
    });
    // Member, not local, because reload() hides it along with the empty list: a
    // "borrar historial" next to the "no has visto ningún vídeo" box
    // contradicts itself (and the dialog had nothing left to confirm). With
    // videos it shows up just like before.
    titleRow->addWidget(m_clearBtn, 0, Qt::AlignVCenter);
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
    // Empty state: a history with no videos left nothing but the bare black list
    // and a line of text at the top. It is the same dotted #emptyState box used
    // by the Library, the Channel tabs and the Playlist.
    m_empty = new QLabel(
        nt::emptyStateHtml(nt::tr("history.empty")), this);
    m_empty->setObjectName(QStringLiteral("emptyState"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setFixedHeight(116);
    m_empty->hide();

    // List and empty box in a container with stretch 1, for the same reason as
    // in PlaylistPage: loose in the page layout, hiding the list leaves the
    // vertical leftover distributed as EXTRA spacing and the header sinks
    // (measured: the title dropped from y=59 to y=195). With the container the
    // header does not move. The empty box goes with stretch 1 and no alignment,
    // so it takes the full width (with Qt::AlignHCenter the layout only gives it
    // its sizeHint and it comes out narrow, measured 174 px with the text cut
    // off); with the fixed height of 96 it stays centered in the area (measured:
    // 909x96 at y=493 of an area spanning y=124..956).
    m_area = new QWidget(this);
    auto *areaLay = new QVBoxLayout(m_area);
    areaLay->setContentsMargins(0, 0, 0, 0);
    areaLay->setSpacing(0);
    areaLay->addWidget(m_list, 1);
    areaLay->addWidget(m_empty, 1);
    root->addWidget(m_area, 1);
}

void HistoryPage::showMoreMenu(const VideoInfo &v, const QPoint &globalPos)
{
    QMenu menu(this);

    // Own action of this page: the rest of the menu is the one shared by every
    // surface with rows (nt::addRowMenuActions), with the same texts and in the
    // same order.
    QAction *rem = menu.addAction(nt::tr("history.remove"));
    connect(rem, &QAction::triggered, this, [this, v]() {
        // In the next loop, with the menu already closed: removing the video
        // erases the row of the list the popup was born in.
        QTimer::singleShot(0, this, [this, v]() {
            m_history->remove(v.id);
            emit historyChanged();
            reload();
        });
    });

    nt::RowMenuHooks hooks;
    hooks.onNewPlaylist = [this](const QString &videoId) {
        QString title;
        if (nt::inputText(this, nt::tr("playlist.newName"),
                          nt::tr("playlist.nameField"),
                          QString(), nt::tr("playlist.create"), &title)) {
            if (title.isEmpty())
                return;
            const QString id = m_playlists->create(title);
            if (!id.isEmpty())
                m_playlists->addVideo(id, videoId);
        }
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
    m_meta->setText(nt::tr("common.videos").arg(m_videos.size()));
    // List or empty box, never both (see the constructor's comment).
    m_empty->setVisible(m_videos.isEmpty());
    m_list->setVisible(!m_videos.isEmpty());
    // "Borrar historial de vídeos" does not show up without videos either: there
    // is nothing to clear and the empty-state box already says so.
    m_clearBtn->setVisible(!m_videos.isEmpty());
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
