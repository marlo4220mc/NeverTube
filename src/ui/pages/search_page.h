#pragma once

#include <QVector>
#include <QWidget>

#include "../../models.h"

class QJsonDocument;
class QLineEdit;
class QTimer;
class VideoListView;
class ThumbnailCache;
class SettingsRepo;
class Yt;

class SearchPage : public QWidget
{
    Q_OBJECT

public:
    explicit SearchPage(Yt *yt, SettingsRepo *settings,
                        ThumbnailCache *thumbs, QWidget *parent = nullptr);

    void startSearch(const QString &query);

signals:
    void openRequested(const QVector<VideoInfo> &videos, int startIndex);

private slots:
    void doSearch();
    void onSearchReady(const QVector<VideoInfo> &videos);
    void onHistoryClear();
    void onHistoryQuery(const QString &query);

private:
    void showHistory();
    QStringList history() const;
    void setHistory(const QStringList &queries);

    Yt *m_yt;
    SettingsRepo *m_settings;
    ThumbnailCache *m_thumbs;

    QLineEdit *m_field;
    QTimer *m_debounce;
    QWidget *m_historyBox;
    QWidget *m_resultBox;
    VideoListView *m_list;

    QVector<VideoInfo> m_results;
    QString m_error;
    bool m_searching = false;
};