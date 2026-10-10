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
    // Click en el "⋯" de una fila de vídeo: la página no tiene los repos, así
    // que MainWindow muestra el menú compartido.
    void rowMenuRequested(const VideoInfo &video, const QPoint &globalPos);

private slots:
    void onChannelInfo(const ChannelInfo &channel);
    void onTabVideos(const QString &channelId, const QString &tab,
                     const QVector<VideoInfo> &videos);
    void onAvatarReady(const QString &key);

private:
    void updateSubscribeButton(bool subscribed);
    // Pide la pestaña index si aún no se ha pedido (lazy): al abrir el canal
    // solo se piden los vídeos; shorts/directos al entrar en su pestaña (cada
    // respuesta pesa ~400-500 KB).
    void loadTab(int index);
    // Muestra el contenido de la pestaña index y marca su botón como
    // seleccionado (con re-polish, obligatorio al cambiar la propiedad que
    // elige el QSS).
    void showTab(int index);
    void updateTabCount();
    void applyDescriptionCap();
    void toggleDescription();
    void positionDescFade();
    void setAvatar(const QPixmap &pm);

    Yt *m_yt = nullptr;
    ThumbnailCache *m_thumbs = nullptr;
    SubscriptionsRepo *m_subs = nullptr;

    // Ficha del canal: avatar + nombre + suscriptores + acciones, con la
    // descripción debajo dentro de la misma caja.
    QFrame *m_card = nullptr;
    QLabel *m_avatar = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    QLabel *m_description = nullptr;
    QString m_descText;  // texto plano de la descripción (el QLabel lleva HTML)
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

    // Pestañas. Antes eran un QTabWidget, que es lo ÚNICO de la app sin estilo
    // propio (no hay ninguna regla QTabWidget/QTabBar en el QSS): salía con el
    // aspecto del tema del escritorio y el "pane" pintaba una columna casi negra
    // (2,3,3 medido) por debajo de la lista, más oscuro que el fondo de la
    // página. Ahora son botones dentro de una cápsula con el QSS global, y el
    // contenido son tres widgets que se muestran a mano.
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