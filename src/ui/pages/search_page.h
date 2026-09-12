#pragma once

#include <QVector>
#include <QHash>
#include <QWidget>

#include "../../models.h"

class QJsonDocument;
class QLineEdit;
class QTimer;
class VideoListView;
class ThumbnailCache;
class SettingsRepo;
class Yt;
class QPushButton;

class SearchPage : public QWidget
{
    Q_OBJECT

public:
    explicit SearchPage(Yt *yt, SettingsRepo *settings,
                        ThumbnailCache *thumbs, QWidget *parent = nullptr);

    void startSearch(const QString &query);

signals:
    void openRequested(const QVector<VideoInfo> &videos, int startIndex);
    void channelRequested(const QString &channelId);

private slots:
    void doSearch();
    void onSearchReady(const QString &query, const QVector<VideoInfo> &videos);
    void onChannelInfo(const ChannelInfo &channel);
    void onHistoryClear();
    void onHistoryQuery(const QString &query);

private:
    void showHistory();
    void buildChannelButtons();
    QStringList history() const;
    void setHistory(const QStringList &queries);

    Yt *m_yt;
    SettingsRepo *m_settings;
    ThumbnailCache *m_thumbs;

    QLineEdit *m_field;
    QTimer *m_debounce;
    QWidget *m_historyBox;
    QWidget *m_resultBox;
    QWidget *m_channelBox;
    VideoListView *m_list;
    QPushButton *m_moreButton;

    QVector<VideoInfo> m_results;
    QVector<ChannelInfo> m_channels;
    QHash<QString, QPushButton *> m_channelButtons;
    QString m_error;
    bool m_searching = false;
    QString m_activeQuery;
};