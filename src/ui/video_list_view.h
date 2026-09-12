#pragma once

#include <QListView>
#include <QVector>

#include "../models.h"

class QStandardItemModel;
class ThumbnailCache;

class VideoListView : public QListView
{
    Q_OBJECT

public:
    enum Mode { GridMode, ListMode };

    explicit VideoListView(ThumbnailCache *thumbs, QWidget *parent = nullptr);

    void showVideos(const QVector<VideoInfo> &videos, Mode mode);
    void appendVideos(const QVector<VideoInfo> &videos);
    void clearVideos();
    // Fila del vídeo en reproducción: se dibuja con un recuadro de marca.
    // -1 (o un índice fuera de rango) oculta la marca.
    void setCurrentRow(int row);
    int currentRow() const { return m_currentRow; }
    bool isEmpty() const;
    QVector<VideoInfo> videos() const;
    void refreshThumbnails();

signals:
    void videoActivated(int index, const VideoInfo &video);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;

private slots:
    void onThumbReady(const QString &key);

private:
    ThumbnailCache *m_thumbs;
    QStandardItemModel *m_model;
    QVector<VideoInfo> m_videos;
    Mode m_mode = ListMode;
    int m_currentRow = -1;
};