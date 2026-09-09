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
    void clearVideos();
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
};