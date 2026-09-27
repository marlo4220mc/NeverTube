#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMetaObject>
#include <QObject>
#include <QVector>

#include "models.h"

class HistoryRepo;
class PlayerWidget;
class QTimer;
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

    // startPositionMs>0 reanuda el primer vídeo desde esa posición.
    void playQueue(const QVector<VideoInfo> &videos, int startIndex,
                   qlonglong startPositionMs = 0);
    // Añade vídeos al final de la cola, omitiendo ids ya presentes (evita
    // repetir relacionados que ya estaban en la lista). Emite queueChanged()
    // si algo fue añadido. No toca el ítem en reproducción.
    void appendVideos(const QVector<VideoInfo> &videos);
    void playIndex(int index);
    void next();
    void previous();
    void playPause();
    void toggleMute();
    // Fija el estado de silencio sin alternarlo (para restaurar el volumen del
    // usuario al arrancar). muted==true pone el volumen a 0, false lo recuerda.
    void setMuted(bool muted);
    void setVolume(double volume);
    // Tope superior del slider (mpv volume-max). Default 100; ajustes puede
    // subirlo a 150/175/200. El slider se clamp aquí, NO en el widget.
    void setVolumeMax(double maxPercent);
    int volumeMax() const { return m_volumeMax; }
    void seek(double seconds);
    void reloadCurrent();
    void stop();

    QVector<VideoInfo> queue() const { return m_queue; }
    int index() const { return m_index; }
    VideoInfo current() const;
    // Aplica los metadatos (título, canal, miniatura…) del detalle al ítem
    // actual si el id coincide. Emite metadataChanged() si algo fue actualizado.
    void applyCurrentDetail(const VideoInfo &detail);
    // Sobrescribe el título del ítem actual (si el id coincide) con el
    // traducido del /next. Emite metadataChanged() para refrescar la UI.
    void renameCurrent(const QString &videoId, const QString &title);
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
    // Fuerza el guardado de la posición actual en el historial (no hace nada
    // si no hay vídeo en fase playing/paused). Lo llama MainWindow al entrar al
    // Home con algo reproduciéndose en el mini player: la fila "Continuar
    // viendo" se pinta en ese momento y debe salir con el progreso real, no con
    // el del último flush (o con pos=0, que el filtro descarta en silencio).
    void savePositionNow() { savePosition(); }

signals:
    void phaseChanged(const QString &phase);
    void positionChanged(double position, double duration);
    // La duración real del medio la reporta el backend al cargar (mpv o el
    // reloj de audio del FfmpegBackend). Es el dato autoritativo: el /next y los
    // renderizadores de origen (search/home/canal) a veces no la traen y
    // dejan durationSeconds=0, y "Continuar viendo" descarta los vídeos sin
    // duración en la caché. Se emite solo cuando CAMBIA (no por tick de
    // posición, que llega ~1/s) para no escribir en la BD sin motivo.
    void durationChanged(double duration);
    void currentChanged();
    void queueChanged();
    void metadataChanged();
    // Recuperación de la sesión gateada AGOTADA (≥3 intentos): el stream no se
    // pudo recuperar en la misma sesión. MainWindow la escucha para reiniciar
    // la app con el último vídeo (reiniciar SÍ sale del gate: estado del
    // proceso nuevo). Se emite SOLO al agotar esta recuperación, no en otro
    // fallo.
    void recoveryExhausted();
    // Reproducción estable ≥5 min: el episodio de recuperación terminó.
    // MainWindow la escucha para desarmar el reinicio automático (que no debe
    // repetirse sin agotar antes otra recuperación).
    void recoveryHealthy();
    // El volumen o el silencio cambió (teclado de MainWindow, botón de
    // silencio, slider del overlay, ajuste). Las UIs la escuchan para
    // sincronizar en caliente. ANTES no existía ninguna notificación: el
    // slider del overlay solo se refrescaba al volver a mostrar la barra
    // (updateOverlayIcons), por eso subir/bajar con las flechas no se veía
    // hasta que el overlay se re-mostraba.
    void volumeChanged();

private slots:
    void onPlayerState();
    void onPlayerPosition();
    void onPlayerDuration();
    void onPlayerError(const QString &message);
    // Atasco de reproducción (sin avance de posición) durante la reproducción:
    // re-resuelve con URLs firmadas frescas y reanuda en la posición actual.
    void onStreamStalled();
    // Interrupción: EOF prematuro o audio externo que nunca abrió (sesión de
    // URLs gateada por YouTube, 403). Re-resuelve con URLs frescas, acotado
    // por el mismo presupuesto de intentos que el atasco.
    void onStreamInterrupted();

private:
    void startResolve(bool forceRefresh);
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
    // Última posición ya escrita en el historial (y su vídeo), para no
    // reescribir la fila cuando el flush de kPositionFlushMs cae con el vídeo
    // parado. playIndex los actualiza al crear la fila del vídeo nuevo.
    QString m_lastSavedId;
    qlonglong m_lastSavedMs = -1;
    // Última duración ya notificada con durationChanged (-1 = ninguna). Se
    // reinicia en cada playIndex para que el aviso salga siempre una vez por
    // vídeo, aunque los ticks de posición hayan actualizado m_duration antes.
    double m_reportedDuration = -1.0;
    bool m_muted = false;
    double m_volume = 100.0;
    int m_volumeMax = 100;
    int m_maxHeight = 720;
    int m_resolveGen = 0;
    double m_seekBack = 0.0;
    // Posición de reanudación asociada a UN índice concreto de la cola (el
    // vídeo con el que se abrió con reanudación). Solo se aplica a ese índice
    // y se consume una sola vez: al avanzar en la cola (fin de vídeo / next)
    // los siguientes arrancan SIEMPRE desde el principio.
    QHash<int, qlonglong> m_indexStartMs;
    qlonglong m_pendingStartMs = 0;
    QTimer *m_flushTimer = nullptr;
    // Recuperación por atasco: evita bucles (pocos intentos, espaciados) y se
    // reinicia cuando la reproducción se mantiene estable un rato.
    int m_recoverAttempts = 0;
    qint64 m_lastRecoverMs = 0;
    QElapsedTimer m_stableSince;
    QMetaObject::Connection m_stallConn;
    QMetaObject::Connection m_interruptConn;
    QMetaObject::Connection m_resolveConn;
    QMetaObject::Connection m_resolveErrConn;
    QMetaObject::Connection m_stateConn;
    QMetaObject::Connection m_posConn;
    QMetaObject::Connection m_durConn;
    QMetaObject::Connection m_errConn;
};