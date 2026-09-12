#pragma once

#include <QMetaObject>
#include <QObject>
#include <QVector>

#include "models.h"

class HistoryRepo;
class PlayerWidget;
class Yt;
struct StreamInfo;

class PlayerController : public QObject
{
    Q_OBJECT

public:
    explicit PlayerController(Yt *yt, PlayerWidget *player,
                              HistoryRepo *history,
                              QObject *parent = nullptr);

    void setPlayer(PlayerWidget *player);
    void setMaxHeight(int height);
    int maxHeight() const;

    void playQueue(const QVector<VideoInfo> &videos, int startIndex);
    // Añade vídeos al final de la cola, omitiendo ids ya presentes (evita
    // repetir relacionados que ya estaban en la lista). Emite queueChanged()
    // si algo fue añadido. No toca el ítem en reproducción.
    void appendVideos(const QVector<VideoInfo> &videos);
    void playIndex(int index);
    void next();
    void previous();
    void playPause();
    void toggleMute();
    void setVolume(double volume);
    void seek(double seconds);
    void reloadCurrent();
    void stop();

    QVector<VideoInfo> queue() const { return m_queue; }
    int index() const { return m_index; }
    VideoInfo current() const;
    // Aplica los metadatos (título, canal, miniatura…) del detalle al ítem
    // actual si el id coincide. Emite metadataChanged() si algo fue actualizado.
    void applyCurrentDetail(const VideoInfo &detail);
    bool hasNext() const { return m_index >= 0 && m_index < m_queue.size() - 1; }
    bool hasPrevious() const { return m_index > 0; }
    bool isActive() const { return m_index >= 0; }
    QString phase() const { return m_phase; }
    QString errorMessage() const { return m_errorMessage; }
    bool muted() const { return m_muted; }
    double volume() const { return m_volume; }
    bool isPaused() const { return m_phase == QLatin1String("paused"); }
    double position() const { return m_position; }
    double duration() const { return m_duration; }

signals:
    void phaseChanged(const QString &phase);
    void positionChanged(double position, double duration);
    void currentChanged();
    void queueChanged();
    void metadataChanged();

private slots:
    void onPlayerState();
    void onPlayerPosition();
    void onPlayerError(const QString &message);

private:
    void startResolve();
    void setPhase(const QString &phase);
    void ensurePlayerConnections();
    void onStreamResolved(const StreamInfo &stream);
    void resolveFailed(const QString &message);
    void savePosition();

    Yt *m_yt;
    PlayerWidget *m_player;
    HistoryRepo *m_history;

    QVector<VideoInfo> m_queue;
    int m_index = -1;
    QString m_phase = QStringLiteral("stopped");
    QString m_errorMessage;
    double m_position = 0.0;
    double m_duration = 0.0;
    bool m_muted = false;
    double m_volume = 100.0;
    int m_maxHeight = 720;
    int m_resolveGen = 0;
    double m_seekBack = 0.0;
    QMetaObject::Connection m_resolveConn;
    QMetaObject::Connection m_resolveErrConn;
    QMetaObject::Connection m_stateConn;
    QMetaObject::Connection m_posConn;
    QMetaObject::Connection m_durConn;
    QMetaObject::Connection m_errConn;
};