#pragma once

#include <QWidget>
#include <QVector>

#include <QStringList>

#include "../../models.h"

class QFrame;
class QLabel;
class QPixmap;
class QPushButton;
class QResizeEvent;
class SubscriptionsRepo;
class ThumbnailCache;
class VideoListView;
class Yt;

class ChannelPage : public QWidget
{
    Q_OBJECT

public:
    explicit ChannelPage(Yt *yt, ThumbnailCache *thumbs,
                         SubscriptionsRepo *subs,
                         QWidget *parent = nullptr);

    void openChannel(const QString &channelId);
    // Re-applies the "Subscribe" button with the current accent (MainWindow
    // calls it when the accent color changes in Settings, without rebuilding).
    void applyAccent();

protected:
    // Re-evaluates the description cap when the page width changes
    // (the re-wrap may make the text fit or no longer fit).
    void resizeEvent(QResizeEvent *event) override;

signals:
    void backRequested();
    void openRequested(const QVector<VideoInfo> &videos, int startIndex);
    // Click on the "⋯" of a video row: the page does not have the repos, so
    // MainWindow builds the shared menu.
    void rowMenuRequested(const VideoInfo &video, const QPoint &globalPos);

private slots:
    void onChannelInfo(const ChannelInfo &channel);
    void onTabVideos(const QString &channelId, const QString &tab,
                     const QVector<VideoInfo> &videos);
    void onAvatarReady(const QString &key);

private:
    void updateSubscribeButton(bool subscribed);
    // Requests the index tab if it has not been requested yet (lazy): when the
    // channel is opened only the videos are requested; shorts/live on entering
    // their tab (each response weighs ~400-500 KB).
    void loadTab(int index);
    // Shows the content of the index tab and marks its button as
    // selected (with re-polish, mandatory when changing the property the QSS
    // picks).
    void showTab(int index);
    void updateTabCount();
    void applyDescriptionCap();
    void toggleDescription();
    void positionDescFade();
    void setAvatar(const QPixmap &pm);

    Yt *m_yt = nullptr;
    ThumbnailCache *m_thumbs = nullptr;
    SubscriptionsRepo *m_subs = nullptr;

    // Channel card: avatar + name + subscribers + actions, with the
    // description below inside the same box.
    QFrame *m_card = nullptr;
    QLabel *m_avatar = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    QLabel *m_description = nullptr;
    QString m_descText;  // plain text of the description (the QLabel carries HTML)
    QFrame *m_descFade = nullptr;
    QPushButton *m_descToggle = nullptr;
    QPushButton *m_subscribeBtn = nullptr;
    QPushButton *m_shareBtn = nullptr;
    QString m_avatarKey;
    QString m_avatarUrl;
    QString m_channelId;
    QString m_channelName;
    bool m_descExpanded = false;
    bool m_descCollapsible = false;

    // Tabs. They used to be a QTabWidget, the ONLY thing in the app without
    // styling (there is no QTabWidget/QTabBar rule in the QSS): it came out with
    // the desktop theme's look and the "pane" painted a nearly black column
    // (2,3,3 measured) under the list, darker than the page
    // background. Now they are buttons inside a capsule with the global QSS, and
    // the content is three widgets shown by hand.
    QWidget *m_tabsBox = nullptr;
    QLabel *m_tabCount = nullptr;
    QVector<QPushButton *> m_tabButtons;
    QVector<QWidget *> m_pages;
    QVector<VideoListView *> m_lists;
    QVector<QPushButton *> m_moreButtons;
    QVector<QLabel *> m_emptyLabels;
    QStringList m_tabKeys;
    QVector<QVector<VideoInfo>> m_videos;
    QVector<char> m_tabLoaded;
    int m_currentTab = 0;
};