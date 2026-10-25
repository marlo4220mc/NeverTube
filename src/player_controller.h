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

    // startPositionMs>0 resumes the first video from that position.
    void playQueue(const QVector<VideoInfo> &videos, int startIndex,
                   qlonglong startPositionMs = 0);
    // Appends videos at the end of the queue, skipping ids already present
    // (avoids repeating related videos already in the list). Emits
    // queueChanged() if anything was added. Does not touch the playing item.
    void appendVideos(const QVector<VideoInfo> &videos);
    // Removes a video from the queue (the "⋯" options button of "A
    // continuación"). Does NOT accept the index of the playing video:
    // removing it would leave the queue pointing at the next one with the
    // playback state of the one that left. If the removed one was before, the
    // current index and the resume positions shift along with it. Emits
    // queueChanged() if it removes something. Returns false if nothing was
    // removed.
    bool removeFromQueue(int index);
    void playIndex(int index);
    // Plays a LOCAL file (a downloaded video). It does NOT go through the
    // resolver: the file is already on the disk. The queue is replaced with
    // the single item, so the controls of the Player work as usual.
    //
    // The position is NOT saved in the history (that is about YouTube
    // videos): the caller keeps it in the downloads table.
    void playLocalFile(const QString &path, const VideoInfo &info,
                       qlonglong startPositionMs = 0);
    void next();
    // "Anterior" with YouTube's behavior: if the current video is already
    // advanced (more than kPrevRestartThreshold), it goes back to the start;
    // only if it is at the beginning does it jump to the previous video of
    // the queue. Used by the Player overlay's button.
    void previous();
    // DIRECT jump to the previous video of the queue, without restarting the
    // current one: used by the "P" keyboard shortcut, whose role is to
    // navigate the queue (not to restart).
    void previousTrack();
    // Player modes (control-row buttons). Shuffle randomly reorders ONLY the
    // queue ahead of the playing video (the current one does not move, like
    // YouTube). Repeat goes back to the first video after finishing the last
    // (with a one-video queue, "repeat one"). They are not persisted: they
    // are session state, and the UI reads them when refreshing the icons.
    void setShuffle(bool on);
    bool shuffle() const { return m_shuffle; }
    void setRepeat(bool on);
    bool repeat() const { return m_repeat; }
    void playPause();
    void toggleMute();
    // Sets the mute state without toggling it (to restore the user's volume
    // on startup). muted==true sets volume to 0, false remembers it.
    void setMuted(bool muted);
    void setVolume(double volume);
    // Slider's upper cap (mpv volume-max). Default 100; settings may raise it
    // to 150/175/200. The slider clamps HERE, NOT in the widget.
    void setVolumeMax(double maxPercent);
    int volumeMax() const { return m_volumeMax; }
    void seek(double seconds);
    void reloadCurrent();
    void stop();

    QVector<VideoInfo> queue() const { return m_queue; }
    int index() const { return m_index; }
    VideoInfo current() const;
    // Applies the detail's metadata (title, channel, thumbnail…) to the
    // current item if the id matches. Emits metadataChanged() if anything was
    // updated.
    void applyCurrentDetail(const VideoInfo &detail);
    // Overwrites the current item's title (if the id matches) with the one
    // translated from /next. Emits metadataChanged() to refresh the UI.
    void renameCurrent(const QString &videoId, const QString &title);
    bool hasNext() const { return m_index >= 0 && m_index < m_queue.size() - 1; }
    bool hasPrevious() const { return m_index > 0; }
    // The "Anterior" button is operational if there is a previous video OR if
    // the current one is already advanced (then the button restarts the
    // current video, like YouTube).
    bool previousAvailable() const
    {
        return m_index >= 0
               && (hasPrevious() || m_position > kPrevRestartThreshold);
    }
    // "Anterior" threshold (seconds): past this much advance, the button goes
    // back to the start of the current video instead of jumping to the
    // previous video of the queue. Same order of magnitude as the keyboard's
    // seek step (±5 s).
    static constexpr double kPrevRestartThreshold = 5.0;
    bool isActive() const { return m_index >= 0; }
    QString phase() const { return m_phase; }
    QString errorMessage() const { return m_errorMessage; }
    bool muted() const { return m_muted; }
    double volume() const { return m_volume; }
    bool isPaused() const { return m_phase == QLatin1String("paused"); }
    double position() const { return m_position; }
    double duration() const { return m_duration; }
    // The current stream was built by the app from a finite medium (synthetic
    // AV1 HLS: fMP4 + VOD+ENDLIST playlist) -> by construction it CANNOT be a
    // live stream. The live UI (EN VIVO, playhead at the edge) is disabled for
    // it even if the backend reports unstable durations (it reports them while
    // loading the BYTERANGE ranges). true on the rest of the flows (a YouTube
    // master, filtered or edge CAN be a live stream).
    bool currentCanBeLive() const { return m_streamCanBeLive; }
    // YouTube's player response declared the current stream as LIVE
    // (videoDetails.isLive): authoritative signal that lets us enter EN VIVO
    // mode instantly, without waiting for the "so far" duration to grow (and
    // it covers pure live streams with dur=0). false on VODs and on the
    // synthetic one.
    bool currentIsLive() const { return m_streamIsLive; }
    // Forces saving the current position to the history (does nothing if
    // there is no video in phase playing/paused). MainWindow calls it when
    // entering Home with something playing in the mini player: the "Continuar
    // viendo" row is painted at that moment and must show the real progress,
    // not the one from the last flush (or pos=0, which the filter silently
    // discards).
    void savePositionNow() { savePosition(); }

signals:
    void phaseChanged(const QString &phase);
    void positionChanged(double position, double duration);
    // The medium's real duration is reported by the backend on load (mpv or
    // the FfmpegBackend's audio clock). It is the authoritative datum: /next
    // and the origin renderers (search/home/channel) sometimes do not bring it
    // and leave durationSeconds=0, and "Continuar viendo" discards videos
    // without duration in the cache. It is emitted only when it CHANGES (not
    // per position tick, which arrives ~1/s) so as not to write to the DB
    // without reason.
    void durationChanged(double duration);
    void currentChanged();
    void queueChanged();
    void metadataChanged();
    // Gated-session recovery EXHAUSTED (≥3 attempts): the stream could not be
    // recovered within the same session. MainWindow listens to it to restart
    // the app with the last video (restarting DOES escape the gate: new
    // process state). Emitted ONLY when this recovery is exhausted, not on
    // any other failure.
    void recoveryExhausted();
    // Stable playback for ≥5 min: the recovery episode is over. MainWindow
    // listens to it to disarm the automatic restart (which must not repeat
    // without another recovery having been exhausted first).
    void recoveryHealthy();
    // Volume or mute changed (MainWindow's keyboard, mute button, overlay
    // slider, setting). The UIs listen to it to sync on the fly. BEFORE there
    // was no such notification: the overlay slider only refreshed when the bar
    // was shown again (updateOverlayIcons), so raising/lowering with the
    // arrows was not visible until the overlay re-appeared.
    void volumeChanged();

private slots:
    void onPlayerState();
    void onPlayerPosition();
    void onPlayerDuration();
    void onPlayerError(const QString &message);
    // Playback stall (position not advancing) during playback: re-resolves
    // with fresh signed URLs and resumes at the current position.
    void onStreamStalled();
    // Interruption: premature EOF or external audio that never opened
    // (YouTube-gated URL session, 403). Re-resolves with fresh URLs, capped
    // by the same attempt budget as the stall.
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
    // Last position already written to the history (and its video), so the
    // row is not rewritten when the kPositionFlushMs flush fires with the
    // video stopped. playIndex updates them when creating the new video's row.
    QString m_lastSavedId;
    qlonglong m_lastSavedMs = -1;
    // Last duration already notified with durationChanged (-1 = none). It is
    // reset on every playIndex so the notice always fires once per video,
    // even if the position ticks updated m_duration earlier.
    double m_reportedDuration = -1.0;
    bool m_muted = false;
    // canBeLive of the current stream (StreamInfo). true until the resolved
    // stream arrives; the synthetic AV1 HLS brings it as false (it is never a
    // live stream).
    bool m_streamCanBeLive = true;
    // videoDetails.isLive of the current stream (StreamInfo), the
    // authoritative live signal of the player response. false until the
    // resolved stream arrives.
    bool m_streamIsLive = false;
    double m_volume = 100.0;
    int m_volumeMax = 100;
    // Playback modes (shuffle/repeat). Session memory, with no signal of
    // their own: the UI reads them in updateOverlayIcons(), which is when
    // they change.
    bool m_shuffle = false;
    bool m_repeat = false;
    int m_maxHeight = 720;
    int m_resolveGen = 0;
    double m_seekBack = 0.0;
    // Resume position associated with ONE specific queue index (the video the
    // app was opened with, resuming). It only applies to that index and is
    // consumed a single time: when advancing in the queue (end of video /
    // next) the following ones ALWAYS start from the beginning.
    QHash<int, qlonglong> m_indexStartMs;
    // True when what is playing is a local file (a download): it does NOT
    // go to the history.
    bool m_isLocalFile = false;
    qlonglong m_pendingStartMs = 0;
    QTimer *m_flushTimer = nullptr;
    // Stall recovery: avoids loops (few, spaced attempts) and resets when
    // playback stays stable for a while.
    int m_recoverAttempts = 0;
    qint64 m_lastRecoverMs = 0;
    QElapsedTimer m_stableSince;
    // Cadence of the positionChanged notification for the UI (see kPositionUiMs).
    QElapsedTimer m_positionUiClock;
    QMetaObject::Connection m_stallConn;
    QMetaObject::Connection m_interruptConn;
    QMetaObject::Connection m_resolveConn;
    QMetaObject::Connection m_resolveErrConn;
    QMetaObject::Connection m_stateConn;
    QMetaObject::Connection m_posConn;
    QMetaObject::Connection m_durConn;
    QMetaObject::Connection m_errConn;
};