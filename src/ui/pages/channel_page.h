#pragma once

#include <QWidget>
#include <QVector>

#include "../../models.h"

class QLabel;
class PlayerController;
class SubscriptionsRepo;
class ThumbnailCache;
class VideoListView;
class Yt;
class QTabWidget;
class QPushButton;
class QResizeEvent;

class ChannelPage : public QWidget
{
    Q_OBJECT

public:
    explicit ChannelPage(Yt *yt, ThumbnailCache *thumbs,
                         SubscriptionsRepo *subs,
                         QWidget *parent = nullptr);

    void openChannel(const QString &channelId);
    // Re-aplica el botón Suscribirse con el acento vigente (MainWindow lo
    // llama al cambiar el color de acento en Ajustes, sin reconstruir).
    void applyAccent();

protected:
    // Re-evalúa el tope de la descripción al cambiar el ancho de la página
    // (el re-wrap puede hacer que el texto quepa o deje de caber).
    void resizeEvent(QResizeEvent *event) override;

signals:
    void backRequested();
    void openRequested(const QVector<VideoInfo> &videos, int startIndex);

private slots:
    void onChannelInfo(const ChannelInfo &channel);
    void onTabVideos(const QString &channelId, const QString &tab,
                     const QVector<VideoInfo> &videos);
    void onAvatarReady(const QString &key);

private:
    void updateSubscribeButton(bool subscribed);
    // Carga la pestaña index si aún no se ha pedido (lazy): al abrir el canal
    // solo se piden los vídeos; shorts/directos se piden al entrar en su
    // pestaña (sus respuestas pesan ~400-500 KB cada una).
    void loadTab(int index);
    // Tope vertical de la descripción: si el texto completo supera
    // kDescMaxHeight se recorta a ese alto y se ofrece "Mostrar más"
    // (toggleDescription despliega a altura natural y vuelve a colapsar).
    void applyDescriptionCap();
    void toggleDescription();

    Yt *m_yt;
    ThumbnailCache *m_thumbs;
    SubscriptionsRepo *m_subs;
    QLabel *m_avatar;
    QLabel *m_title;
    QString m_avatarKey;
    QString m_avatarUrl;
    QPushButton *m_subscribeBtn;
    QLabel *m_meta;
    QLabel *m_description;
    // Botón "Mostrar más/ menos" de la descripción y si está desplegada.
    QPushButton *m_descMore = nullptr;
    bool m_descExpanded = false;
    QTabWidget *m_tabs;
    QVector<VideoListView *> m_lists;
    QStringList m_tabKeys;
    QVector<QPushButton *> m_moreButtons;
    QString m_channelId;
    QString m_channelName;
    QVector<QVector<VideoInfo>> m_videos;
    // Pestañas ya pedidas (0/1 por índice) y si su "Mostrar más" está en curso.
    QVector<char> m_tabLoaded;
};
