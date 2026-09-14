#pragma once

#include <QHash>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

#include "../../models.h"

class QLabel;
class QPushButton;
class QVBoxLayout;
class QHBoxLayout;
class QGridLayout;
class QScrollArea;
class QFrame;
class QLineEdit;
class QAction;
class QMenu;
class FavoritesRepo;
class HistoryRepo;
class StarredRepo;
class WatchLaterRepo;
class PlaylistsRepo;
class SubscriptionsRepo;
class ThumbnailCache;
class VideoCacheRepo;
class Yt;
class ChannelRow;

struct PlaylistItem;

class LibraryPage : public QWidget
{
    Q_OBJECT

public:
    explicit LibraryPage(Yt *yt, FavoritesRepo *favorites, StarredRepo *starred,
                         WatchLaterRepo *watchLater, HistoryRepo *history,
                         SubscriptionsRepo *subs, VideoCacheRepo *cache,
                         ThumbnailCache *thumbs, PlaylistsRepo *playlists,
                         QWidget *parent = nullptr);

    void reload();

signals:
    void channelRequested(const QString &channelId, const QString &channelName);
    void openRequested(const QVector<VideoInfo> &videos, int startIndex);
    void resumeRequested(const QVector<VideoInfo> &videos, int startIndex,
                         qlonglong startPositionMs);
    void playlistRequested(const QString &playlistId, const QString &title);
    void historyAllRequested();

private:
    // Una sección de lista de vídeos (fila con miniatura, título y menú).
    struct VideoSection
    {
        QWidget *host = nullptr;
        QVBoxLayout *lay = nullptr;
        QLabel *empty = nullptr;
        QPushButton *allBtn = nullptr;
        QVector<VideoInfo> videos;
        bool showAll = false;
    };

    void rebuildSubscriptions();
    void rebuildFavorites();
    void rebuildWatchLater();
    void rebuildHistory();
    void rebuildPlaylists();
    void onChannelInfo(const ChannelInfo &channel);
    void addStandardMenuActions(QMenu *menu, const VideoInfo &v,
                                bool withWatchLater);

    void createPlaylist();
    void setupCreatePlaylistPopup();
    void openCreatePlaylistPopup();
    void openRenamePopup(const PlaylistItem &playlist);
    void closeCreatePlaylistPopup();
    void submitCreatePlaylist();
    void centerPopup(QFrame *popup);

    void setupManageSubsPopup();
    void openManageSubs();
    void closeManageSubs();
    void rebuildManageSubs();
    void layoutCards(QGridLayout *grid, const QVector<QWidget *> &cards);
    QString subMetaFor(const QString &channelId) const;
    void updateSubsRowGeometry();
    void buildRows(QVBoxLayout *lay, const QVector<VideoInfo> &videos, int visible,
                   const std::function<void(int)> &activate,
                   const std::function<void(QMenu *, const VideoInfo &)> &fillMenu);

    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

    Yt *m_yt;
    FavoritesRepo *m_favs;
    StarredRepo *m_starred;
    WatchLaterRepo *m_watchLater;
    HistoryRepo *m_history;
    SubscriptionsRepo *m_subs;
    VideoCacheRepo *m_cache;
    ThumbnailCache *m_thumbs;
    PlaylistsRepo *m_playlists;

    QScrollArea *m_scroll = nullptr;
    QWidget *m_body = nullptr;

    // Suscripciones
    QLabel *m_channelHint = nullptr;
    QScrollArea *m_subScroll = nullptr;
    QWidget *m_subHost = nullptr;
    QHBoxLayout *m_subRowLay = nullptr;
    QHash<QString, QString> m_subTitles;   // channelId → título del canal
    QHash<QString, ChannelRow *> m_subMeta;    // channelId → tarjeta de suscripción
    QHash<QString, long long> m_subCounts; // channelId → suscriptores (caché)

    QLabel *m_playlistEmpty = nullptr;
    QGridLayout *m_playlistGrid = nullptr;
    QVector<QWidget *> m_playlistCards;
    QVector<PlaylistItem> m_playlistMeta;

    VideoSection m_fav;
    VideoSection m_later;
    VideoSection m_historySec;

    QFrame *m_createPopup = nullptr;
    QLabel *m_popupTitle = nullptr;
    QLineEdit *m_createName = nullptr;
    QPushButton *m_popupOk = nullptr;
    QString m_renameId;         // playlistId en edición; vacío = modo "Nueva"
    QString m_pendingVideoId;   // vídeo pendiente de añadir a la playlist nueva

    QFrame *m_managePopup = nullptr;
    QVBoxLayout *m_manageLay = nullptr;
};