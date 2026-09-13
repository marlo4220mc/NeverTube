#include "playlist_page.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "../../db/repositories.h"
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

    auto *top = new QHBoxLayout;
    auto *back = new QPushButton(QStringLiteral("Volver"), this);
    back->setObjectName(QStringLiteral("ghost"));
    connect(back, &QPushButton::clicked, this,
            &PlaylistPage::backRequested);
    top->addWidget(back);
    top->addStretch(1);
    root->addLayout(top);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("sectionTitle"));
    root->addWidget(m_title);

    m_meta = new QLabel(this);
    m_meta->setObjectName(QStringLiteral("sectionSub"));
    root->addWidget(m_meta);

    auto *actions = new QHBoxLayout;
    auto *playBtn = new QPushButton(QStringLiteral("Reproducir"), this);
    playBtn->setObjectName(QStringLiteral("primary"));
    playBtn->setCursor(Qt::PointingHandCursor);
    connect(playBtn, &QPushButton::clicked, this, [this]() {
        if (!m_videos.isEmpty())
            emit openRequested(m_videos, 0);
    });
    actions->addWidget(playBtn);

    m_removeBtn = new QPushButton(QStringLiteral("Quitar seleccionado"), this);
    m_removeBtn->setObjectName(QStringLiteral("ghost"));
    m_removeBtn->setCursor(Qt::PointingHandCursor);
    m_removeBtn->setEnabled(false);
    connect(m_removeBtn, &QPushButton::clicked, this,
            &PlaylistPage::onRemoveSelected);
    actions->addWidget(m_removeBtn);

    m_renameBtn = new QPushButton(QStringLiteral("Renombrar"), this);
    m_renameBtn->setObjectName(QStringLiteral("ghost"));
    m_renameBtn->setCursor(Qt::PointingHandCursor);
    connect(m_renameBtn, &QPushButton::clicked, this, &PlaylistPage::onRename);
    actions->addWidget(m_renameBtn);

    m_deleteBtn = new QPushButton(QStringLiteral("Eliminar"), this);
    m_deleteBtn->setObjectName(QStringLiteral("ghost"));
    m_deleteBtn->setCursor(Qt::PointingHandCursor);
    connect(m_deleteBtn, &QPushButton::clicked, this,
            &PlaylistPage::onDelete);
    actions->addWidget(m_deleteBtn);

    actions->addStretch(1);
    root->addLayout(actions);

    m_list = new VideoListView(m_thumbs, this);
    connect(m_list, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                m_list->setCurrentRow(index);
                m_removeBtn->setEnabled(index >= 0 && index < m_videos.size());
                emit openRequested(m_videos, index);
            });
    root->addWidget(m_list, 1);
}

void PlaylistPage::openPlaylist(const QString &id, const QString &title)
{
    m_playlistId = id;
    m_title->setText(title);
    reload();
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
    m_meta->setText(QStringLiteral("%1 vídeos").arg(m_videos.size()));
}

void PlaylistPage::onRemoveSelected()
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
    bool ok = false;
    const QString newTitle =
        QInputDialog::getText(this, QStringLiteral("Renombrar playlist"),
                              QStringLiteral("Nuevo nombre:"),
                              QLineEdit::Normal, m_title->text(), &ok);
    if (!ok || newTitle.trimmed().isEmpty())
        return;
    m_playlists->rename(m_playlistId, newTitle.trimmed());
    m_title->setText(newTitle.trimmed());
}

void PlaylistPage::onDelete()
{
    m_playlists->deletePlaylist(m_playlistId);
    m_playlistId.clear();
    reload();
    emit backRequested();
}