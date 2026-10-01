#pragma once

#include <QVector>
#include <QWidget>

#include "../../models.h"

class PlaylistsRepo;
class VideoCacheRepo;
class VideoListView;
class ThumbnailCache;

class QFrame;
class QLabel;
class QPushButton;

class PlaylistPage : public QWidget
{
    Q_OBJECT

public:
    explicit PlaylistPage(PlaylistsRepo *playlists, VideoCacheRepo *cache,
                          ThumbnailCache *thumbs, QWidget *parent = nullptr);

    void openPlaylist(const QString &id, const QString &title);

signals:
    void backRequested();
    void openRequested(const QVector<VideoInfo> &videos, int startIndex);
    // Click en el "⋯" de una fila de la playlist: MainWindow muestra el menú
    // compartido.
    void rowMenuRequested(const VideoInfo &video, const QPoint &globalPos);

private:
    void reload();
    void onRemoveMarked();
    void onRename();
    void onDelete();

    PlaylistsRepo *m_playlists;
    VideoCacheRepo *m_cache;
    ThumbnailCache *m_thumbs;
    QString m_playlistId;
    QFrame *m_card = nullptr;
    // Contenedor con stretch que comparten la lista y la caja de estado vacío
    // (ver el comentario del constructor).
    QWidget *m_area = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    VideoListView *m_list = nullptr;
    QLabel *m_empty = nullptr;
    QPushButton *m_removeBtn = nullptr;
    QPushButton *m_renameBtn = nullptr;
    QPushButton *m_deleteBtn = nullptr;
    QVector<VideoInfo> m_videos;
};