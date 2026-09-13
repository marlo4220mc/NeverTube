#pragma once

#include <QVector>
#include <QWidget>

#include "../../models.h"

class HistoryRepo;
class VideoCacheRepo;
class VideoListView;
class ThumbnailCache;

class QLabel;
class QPushButton;

class HistoryPage : public QWidget
{
    Q_OBJECT

public:
    explicit HistoryPage(HistoryRepo *history, VideoCacheRepo *cache,
                         ThumbnailCache *thumbs, QWidget *parent = nullptr);

    void reload();

signals:
    void backRequested();
    void historyChanged();
    void resumeRequested(const QVector<VideoInfo> &videos, int startIndex,
                         qlonglong startPositionMs);

private:
    // Pinta la barrita de progreso (position_ms) sobre las miniaturas de los
    // vídeos a medio ver, igual que en "Continuar viendo".
    void applyProgress();

    HistoryRepo *m_history;
    VideoCacheRepo *m_cache;
    ThumbnailCache *m_thumbs;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    VideoListView *m_list = nullptr;
    QVector<VideoInfo> m_videos;
};