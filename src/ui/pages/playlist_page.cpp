#include "playlist_page.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "../../db/repositories.h"
#include "../../i18n.h"
#include "../confirm_dialog.h"
#include "../icons.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

PlaylistPage::PlaylistPage(PlaylistsRepo *playlists, VideoCacheRepo *cache,
                           ThumbnailCache *thumbs, QWidget *parent)
    : QWidget(parent)
    , m_playlists(playlists)
    , m_cache(cache)
    , m_thumbs(thumbs)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    // "Volver" (Back) with the #backLink token, same as Channel, History and Player.
    auto *top = new QHBoxLayout;
    auto *back = new QPushButton(this);
    back->setObjectName(QStringLiteral("backLink"));
    nt::bindText(back, "common.back");
    back->setIcon(nt::icon(nt::Icon::Back, nt::kMuted, 13));
    back->setCursor(Qt::PointingHandCursor);
    connect(back, &QPushButton::clicked, this,
            &PlaylistPage::backRequested);
    top->addWidget(back);
    top->addSpacing(4);
    top->addStretch(1);
    root->addLayout(top);

    // --- Playlist card ------------------------------------------------------
    // The SAME box as the channel card (#channelCard, #channelName,
    // #channelMeta): name and count in one column, the four
    // actions on the right. Before, everything sat loose on the page background
    // in three rows (title, meta, actions), and the only box on the surface
    // was the list, so the header did not read as a card.
    // The tokens are the same on purpose: it is a single shape for both
    // detail surfaces, not two similar things.
    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("channelCard"));
    m_card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *cardLay = new QVBoxLayout(m_card);
    cardLay->setContentsMargins(16, 14, 16, 14);

    auto *head = new QHBoxLayout;
    head->setSpacing(14);

    auto *titleCol = new QVBoxLayout;
    titleCol->setContentsMargins(0, 0, 0, 0);
    titleCol->setSpacing(3);
    m_title = new QLabel(nt::tr("playlist.title"), m_card);
    m_title->setObjectName(QStringLiteral("channelName"));
    m_title->setWordWrap(true);
    titleCol->addWidget(m_title);
    m_meta = new QLabel(m_card);
    m_meta->setObjectName(QStringLiteral("channelMeta"));
    titleCol->addWidget(m_meta);
    head->addLayout(titleCol, 1);

    // Play is the page's main action (#primary) and the other three are
    // list management, not playback: that is why they go as ghosts
    // beside it and not as four buttons of equal weight.
    auto *playBtn = new QPushButton(m_card);
    playBtn->setObjectName(QStringLiteral("primary"));
    nt::bindText(playBtn, "playlist.play");
    playBtn->setCursor(Qt::PointingHandCursor);
    playBtn->setFixedHeight(38);
    connect(playBtn, &QPushButton::clicked, this, [this]() {
        if (!m_videos.isEmpty())
            emit openRequested(m_videos, 0);
    });
    head->addWidget(playBtn, 0, Qt::AlignVCenter);

    // The button removes the MARKED row, not a "selected" one: the list is
    // NoSelection and the only state is the active row (m_currentRow, the one from the
    // last click, which is also the one being played). Saying
    // "selected" promised something that does not exist, so the label says what it
    // actually does. It still starts disabled: with no marked row there is nothing to
    // remove, and with the short label the button does not compete with "Reproducir".
    m_removeBtn = new QPushButton(m_card);
    m_removeBtn->setObjectName(QStringLiteral("ghost"));
    nt::bindText(m_removeBtn, "playlist.remove");
    m_removeBtn->setCursor(Qt::PointingHandCursor);
    m_removeBtn->setFixedHeight(38);
    m_removeBtn->setEnabled(false);
    nt::bindTooltip(m_removeBtn, "playlist.removeTip");
    connect(m_removeBtn, &QPushButton::clicked, this,
            &PlaylistPage::onRemoveMarked);
    head->addWidget(m_removeBtn, 0, Qt::AlignVCenter);

    m_renameBtn = new QPushButton(m_card);
    m_renameBtn->setObjectName(QStringLiteral("ghost"));
    nt::bindText(m_renameBtn, "playlist.rename");
    m_renameBtn->setCursor(Qt::PointingHandCursor);
    m_renameBtn->setFixedHeight(38);
    connect(m_renameBtn, &QPushButton::clicked, this, &PlaylistPage::onRename);
    head->addWidget(m_renameBtn, 0, Qt::AlignVCenter);

    m_deleteBtn = new QPushButton(m_card);
    m_deleteBtn->setObjectName(QStringLiteral("ghost"));
    nt::bindText(m_deleteBtn, "common.delete");
    m_deleteBtn->setCursor(Qt::PointingHandCursor);
    m_deleteBtn->setFixedHeight(38);
    connect(m_deleteBtn, &QPushButton::clicked, this,
            &PlaylistPage::onDelete);
    head->addWidget(m_deleteBtn, 0, Qt::AlignVCenter);

    cardLay->addLayout(head);
    root->addWidget(m_card);

    m_list = new VideoListView(m_thumbs, this);
    connect(m_list, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                m_list->setCurrentRow(index);
                m_removeBtn->setEnabled(index >= 0 && index < m_videos.size());
                emit openRequested(m_videos, index);
            });
    // Per-row options button, same as Search/Channel/History: the page
    // asks for the video and MainWindow shows the shared menu.
    m_list->setRowMoreVisible(true);
    connect(m_list, &VideoListView::moreRequested, this,
            [this](int, const VideoInfo &v, const QPoint &pos) {
                emit rowMenuRequested(v, pos);
            });
    // Empty state: a playlist with no videos left nothing but the bare black
    // list. It is the same dotted #emptyState box as Library, Channel
    // tabs and History, with a FIXED height of 96 px so it does not
    // stretch to the full height of the area.
    // No U+22EE glyph (three vertical dots) in the text: it is not in the
    // app's font and the missing-glyph marker would show up. Hence the text
    // says "el menú" (the menu) and not "el ⋮"; --selftest-glyphs warns about this.
    m_empty = new QLabel(
        nt::emptyStateHtml(nt::tr("playlist.empty")),
        this);
    m_empty->setObjectName(QStringLiteral("emptyState"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setFixedHeight(116);
    m_empty->hide();

    // List and empty box go in a CONTAINER with stretch 1, not loose in the
    // page layout. Measured reason: hiding the list makes a QVBoxLayout
    // distribute the leftover vertical space as EXTRA spacing between its items, so
    // the header slid down (with an empty list, "Volver" fell to
    // y=195 instead of y=16 and the card to y=417 instead of y=59). With the container
    // it keeps the leftover space itself and the header does not move: the list and the
    // empty box always start at the same place.
    m_area = new QWidget(this);
    auto *areaLay = new QVBoxLayout(m_area);
    areaLay->setContentsMargins(0, 0, 0, 0);
    areaLay->setSpacing(0);
    areaLay->addWidget(m_list, 1);
    // The empty box goes with stretch 1 and NO alignment, on purpose: with
    // Qt::AlignHCenter the layout gives it only the sizeHint and the box comes out narrow
    // (measured: 174 px) with the text cut off, and the Expanding policy does not
    // change that. With no alignment it takes the full width of the area and, since the height is
    // fixed at 96, Qt centers it in the space it gets (measured: 909x96 at
    // y=500 inside the area y=141..956), same as the Channel tabs.
    areaLay->addWidget(m_empty, 1);
    root->addWidget(m_area, 1);
}

void PlaylistPage::openPlaylist(const QString &id, const QString &title)
{
    m_playlistId = id;
    m_titleText = title;
    m_title->setText(title.isEmpty() ? nt::tr("playlist.title") : title);
    reload();
}

// Called by MainWindow when the language changes. The buttons are live-bound;
// the title fallback, the "N vídeos" counter and the empty state (wrapped by
// emptyStateHtml, which adds the logo) are re-applied here.
void PlaylistPage::retranslate()
{
    m_title->setText(m_titleText.isEmpty() ? nt::tr("playlist.title")
                                           : m_titleText);
    m_empty->setText(nt::emptyStateHtml(nt::tr("playlist.empty")));
    if (!m_playlistId.isEmpty())
        m_meta->setText(nt::tr("common.videos").arg(m_videos.size()));
}

void PlaylistPage::reload()
{
    if (m_playlistId.isEmpty())
        return;
    const QStringList ids = m_playlists->videoIds(m_playlistId);
    m_videos = m_cache->getMany(ids);
    m_list->showVideos(m_videos, VideoListView::ListMode);
    m_list->setCurrentRow(-1);
    m_removeBtn->setEnabled(false);
    m_meta->setText(nt::tr("common.videos").arg(m_videos.size()));
    // List or empty box, never both (see the constructor's comment).
    m_empty->setVisible(m_videos.isEmpty());
    m_list->setVisible(!m_videos.isEmpty());
}

// Removes from the playlist the MARKED row (currentRow, the one from the last
// click). The list is NoSelection: there is no multi-selection, so this removes ONE
// video, the one the button shows marked with the accent border.
void PlaylistPage::onRemoveMarked()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_videos.size())
        return;
    const QString id = m_videos.at(row).id;
    m_playlists->removeVideo(m_playlistId, id);
    reload();
}

void PlaylistPage::onRename()
{
    QString newTitle;
    if (nt::inputText(this, nt::tr("playlist.renameTitle"),
                      nt::tr("playlist.renameField"),
                      m_title->text(), nt::tr("playlist.save"), &newTitle)) {
        if (newTitle.isEmpty())
            return;
        m_playlists->rename(m_playlistId, newTitle);
        m_title->setText(newTitle);
    }
}

void PlaylistPage::onDelete()
{
    // Confirmation before deleting the whole playlist: it is all that is left of
    // it, and the videos can in fact be re-added one by one. The app's dialog
    // (nt::confirm) and not QMessageBox, for the same reason as the one on the
    // Search page. The body carries the playlist's name and its count, which
    // is what says what will be lost.
    if (m_playlistId.isEmpty())
        return;
    const QString name = m_title->text().trimmed();
    const QString where = name.isEmpty()
        ? nt::tr("playlist.this")
        : QStringLiteral("«%1»").arg(name);
    nt::confirm(this, nt::tr("playlist.deleteTitle"),
                nt::tr("playlist.deleteBody")
                    .arg(where, QStringLiteral("%1").arg(m_videos.size())),
                nt::tr("common.delete"), [this]() {
                    m_playlists->deletePlaylist(m_playlistId);
                    m_playlistId.clear();
                    reload();
                    emit backRequested();
                });
}