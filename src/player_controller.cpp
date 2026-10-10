#include "player_controller.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QRandomGenerator>
#include <QTimer>

#include "db/repositories.h"
#include "debug.h"
#include "playerwidget.h"
#include "yt.h"

namespace {
// How often the position is saved to history. 20 s (was 60 s): that is what
// caused the delay of the "Continuar viendo" progress bar — with 60 s
// the user saw progress up to a minute stale. The cost per write is
// ONE INSERT OR REPLACE of a row in local SQLite, and savePosition() discards
// the write if the position has not advanced (video paused), so lowering the
// interval is noticeable neither in CPU nor on disk. The rest of the save points (pause, video
// change, aboutToQuit) still save instantly.
constexpr int kPositionFlushMs = 20000;
// Minimum advance (ms) to treat the position to save as new. Avoids rewriting
// the same row when the flush fires with the video stopped.
constexpr qlonglong kPositionFlushMinDeltaMs = 2000;
// "Stable" playback window that resets the recovery attempt
// budget (a one-off interruption after a long healthy stretch must not
// drain what was accumulated). 5 min and NOT 60 s: YouTube's gated session dies at
// ~60 s of loading (premature EOF at ~1:03) and with 60 s the restart wiped the
// budget right before each death → reload → 60 s → reload… endless
// loop. With 5 min the gate never gets to restart and the budget is drained
// instead (max. 3 attempts → clear error); genuinely rare interruptions
// (past 5 min of healthy playback) still do not spend budget.
constexpr qint64 kRecoverBudgetResetMs = 300000;
}

PlayerController::PlayerController(Yt *yt, PlayerWidget *player,
                                   HistoryRepo *history,
                                   QObject *parent)
    : QObject(parent)
    , m_yt(yt)
    , m_player(player)
    , m_history(history)
{
    ensurePlayerConnections();
    m_flushTimer = new QTimer(this);
    m_flushTimer->setInterval(kPositionFlushMs);
    connect(m_flushTimer, &QTimer::timeout, this, [this] {
        savePosition();
        // Stable playback ≥5 min in a row: closes the recovery
        // episode (MainWindow disarms the gated-session auto-restart; without
        // this, after a restart that worked, the next gated session could no
        // longer restart).
        if (m_phase == QLatin1String("playing") && m_stableSince.isValid()
            && m_stableSince.elapsed() > kRecoverBudgetResetMs) {
            m_stableSince.restart();
            emit recoveryHealthy();
        }
    });
    if (QCoreApplication *app = QCoreApplication::instance())
        connect(app, &QCoreApplication::aboutToQuit, this,
                [this] { savePosition(); });
}

void PlayerController::ensurePlayerConnections()
{
    if (!m_player)
        return;
    disconnect(m_stateConn);
    disconnect(m_posConn);
    disconnect(m_durConn);
    disconnect(m_errConn);
    disconnect(m_stallConn);
    disconnect(m_interruptConn);
    m_stallConn = connect(m_player, &PlayerWidget::streamStalled, this,
                          &PlayerController::onStreamStalled);
    m_interruptConn = connect(m_player, &PlayerWidget::streamInterrupted, this,
                              &PlayerController::onStreamInterrupted);
    m_stateConn = connect(m_player, &PlayerWidget::stateChanged, this,
                          &PlayerController::onPlayerState);
    m_posConn = connect(m_player, &PlayerWidget::positionChanged, this,
                        &PlayerController::onPlayerPosition);
    m_durConn = connect(m_player, &PlayerWidget::durationChanged, this,
                        &PlayerController::onPlayerDuration);
    m_errConn = connect(m_player, &PlayerWidget::mediaError, this,
                        &PlayerController::onPlayerError);
}

void PlayerController::setPlayer(PlayerWidget *player)
{
    m_player = player;
    ensurePlayerConnections();
}

void PlayerController::setMaxHeight(int height)
{
    m_maxHeight = height;
}

int PlayerController::maxHeight() const
{
    return m_maxHeight;
}

void PlayerController::playQueue(const QVector<VideoInfo> &videos,
                                 int startIndex, qlonglong startPositionMs)
{
    if (videos.isEmpty() || startIndex < 0 || startIndex >= videos.size())
        return;
    m_indexStartMs.clear();
    if (startPositionMs > 0)
        m_indexStartMs.insert(startIndex, startPositionMs);
    m_queue = videos;
    emit queueChanged();
    playIndex(startIndex);
}

void PlayerController::appendVideos(const QVector<VideoInfo> &videos)
{
    bool added = false;
    for (const VideoInfo &v : videos) {
        if (v.id.isEmpty())
            continue;
        bool dup = false;
        for (const VideoInfo &q : m_queue) {
            if (q.id == v.id) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            m_queue.append(v);
            added = true;
        }
    }
    if (added)
        emit queueChanged();
}

bool PlayerController::removeFromQueue(int index)
{
    if (index < 0 || index >= m_queue.size())
        return false;
    // The video being played is not removed: doing so would leave the queue
    // pointing at the next one while carrying the playback state of the one
    // that left, and "A continuación" would mark as playing one the user did
    // not choose.
    if (index == m_index)
        return false;

    m_queue.remove(index);
    // Everything behind the removed one shifts one index back, so the resume
    // position table must be reindexed: otherwise a "continue watching" would
    // start at another video's position.
    QHash<int, qlonglong> shifted;
    for (auto it = m_indexStartMs.constBegin();
         it != m_indexStartMs.constEnd(); ++it) {
        if (it.key() == index)
            continue; // the removed video
        shifted.insert(it.key() > index ? it.key() - 1 : it.key(), it.value());
    }
    m_indexStartMs = shifted;
    if (index < m_index)
        --m_index;
    emit queueChanged();
    return true;
}

void PlayerController::playIndex(int index)
{
    if (index < 0 || index >= m_queue.size())
        return;
    savePosition();
    m_index = index;
    m_reportedDuration = -1.0;
    m_seekBack = 0.0;
    m_errorMessage.clear();
    m_pendingStartMs = m_indexStartMs.value(index, 0);
    m_recoverAttempts = 0;
    m_lastRecoverMs = 0;
    // New stream: by default it can be a live stream until streamReady
    // arrives (the AV1 synthetic will bring it as false), and it is NOT a
    // declared live stream (the authoritative signal arrives with streamReady,
    // true only if the player response marked it as a current live broadcast).
    m_streamCanBeLive = true;
    m_streamIsLive = false;
    setPhase(QStringLiteral("loading"));
    emit currentChanged();

    const VideoInfo &v = m_queue[index];
    if (m_history && !v.id.isEmpty()) {
        m_history->add(v.id, m_pendingStartMs);
        // The new video's row was just written with the resume
        // position: the baseline for discarding redundant writes is
        // that one, not the previous video's.
        m_lastSavedId = v.id;
        m_lastSavedMs = m_pendingStartMs;
    }

    if (v.id.isEmpty()) {
        setPhase(QStringLiteral("error"));
        return;
    }
    startResolve(/*forceRefresh*/ true);
}

void PlayerController::startResolve(bool forceRefresh)
{
    disconnect(m_resolveConn);
    disconnect(m_resolveErrConn);
    const int gen = ++m_resolveGen;
    m_resolveConn = connect(m_yt, &Yt::streamReady, this,
                            [this, gen](const StreamInfo &s) {
                                if (gen == m_resolveGen)
                                    onStreamResolved(s);
                            });
    m_resolveErrConn = connect(m_yt, &Yt::resolveError, this,
                               [this, gen](const QString &message) {
                                   if (gen == m_resolveGen)
                                       resolveFailed(message);
                               });
    m_yt->resolveStream(m_queue[m_index].id, m_maxHeight, forceRefresh);
}

void PlayerController::onStreamResolved(const StreamInfo &stream)
{
    if (m_index < 0)
        return;
    if (stream.url.isEmpty()) {
        setPhase(QStringLiteral("error"));
        m_errorMessage = QStringLiteral("No se obtuvo URL de streaming.");
        return;
    }
    // The AV1 synthetic (built from a finite fMP4) is never a live
    // stream; the other streams can be.
    m_streamCanBeLive = stream.canBeLive;
    m_streamIsLive = stream.isLive;
    double from = m_seekBack;
    if (m_pendingStartMs > 0) {
        from = double(m_pendingStartMs) / 1000.0;
        m_pendingStartMs = 0;
    }
    // Consumed: the same index does not start again from that position on
    // future visits (e.g. previous() after finishing).
    m_indexStartMs.remove(m_index);
    // The audio language preference is applied right before loading the
    // source: without "alang" mpv would pick the track with DEFAULT=YES (the original)
    // even if the master comes reordered with the dub first.
    m_player->setAudioLanguage(m_yt->audioLanguage());
    // Direct playback (edge): the audio track is a separate stream that
    // mpv merges with --audio-file (exact duration + reliable seek, without the single-segment
    // HLS). Empty clears the option for the classic stream.
    m_player->setAudioFile(stream.audioUrl);
    NT_DEBUG("E2E playIndex=%d from=%.1f resumeMs=0 audioUrl=%d\n", m_index,
             from, int(!stream.audioUrl.isEmpty()));
    m_player->playUrl(stream.url, from);
    m_seekBack = 0.0;
    m_stableSince.start();
}

void PlayerController::resolveFailed(const QString &message)
{
    m_errorMessage = message;
    m_pendingStartMs = 0;
    m_indexStartMs.remove(m_index);
    NT_DEBUG("E2E resolveFailed msg=%s\n", message.toUtf8().constData());
    setPhase(QStringLiteral("error"));
}

void PlayerController::onPlayerState()
{
    const QString s = m_player->state();
    if (s == QLatin1String("playing")) {
        setPhase(QStringLiteral("playing"));
        if (m_seekBack > 2.0) {
            NT_DEBUG("E2E seekBack aplicado=%.1f\n", m_seekBack);
            m_player->seek(m_seekBack);
            m_seekBack = 0.0;
        }
    } else if (s == QLatin1String("ended")) {
        if (hasNext()) {
            savePosition();
            playIndex(m_index + 1);
        } else if (m_repeat && !m_queue.isEmpty()) {
            // Repeat the whole queue: when the last one ends, it goes back
            // to the first (with a one-video queue, this is "repeat one").
            savePosition();
            playIndex(0);
        } else {
            setPhase(QStringLiteral("paused"));
        }
    } else if (s == QLatin1String("error")) {
        setPhase(QStringLiteral("error"));
    }
}

void PlayerController::onPlayerPosition()
{
    m_position = m_player->position();
    m_duration = m_player->duration();
    emit positionChanged(m_position, m_duration);
}

// The media duration really changed (the backend already has it). It is notified
// separately from positionChanged because the latter arrives ~1/s during playback and
// writing to the video cache on every tick would be useless DB work.
void PlayerController::onPlayerDuration()
{
    const double d = m_player->duration();
    if (d <= 0 || qAbs(d - m_reportedDuration) < 0.5)
        return;
    // Compared against the ALREADY NOTIFIED duration, not m_duration: the
    // position ticks also update it and could "consume" the notification
    // before the signal arrives (the video would go un-cached).
    m_reportedDuration = d;
    m_duration = d;
    emit durationChanged(d);
}

void PlayerController::onPlayerError(const QString &message)
{
    m_errorMessage = message;
    NT_DEBUG("E2E playerError msg=%s\n", message.toUtf8().constData());
    setPhase(QStringLiteral("error"));
}

void PlayerController::setPhase(const QString &phase)
{
    if (m_phase == phase)
        return;
    m_phase = phase;
    if (m_flushTimer) {
        if (phase == QLatin1String("playing"))
            m_flushTimer->start();
        else
            m_flushTimer->stop();
    }
    NT_DEBUG("E2E phase=%s\n", phase.toUtf8().constData());
    emit phaseChanged(m_phase);
}

VideoInfo PlayerController::current() const
{
    if (m_index >= 0 && m_index < m_queue.size())
        return m_queue.at(m_index);
    return VideoInfo();
}

void PlayerController::applyCurrentDetail(const VideoInfo &detail)
{
    if (m_index < 0 || m_index >= m_queue.size())
        return;
    VideoInfo &cur = m_queue[m_index];
    if (cur.id != detail.id)
        return;
    bool changed = false;
    if (cur.title.isEmpty() && !detail.title.isEmpty()) {
        cur.title = detail.title;
        changed = true;
    }
    if (cur.channel.isEmpty() && !detail.channel.isEmpty()) {
        cur.channel = detail.channel;
        changed = true;
    }
    if (cur.channelId.isEmpty() && !detail.channelId.isEmpty()) {
        cur.channelId = detail.channelId;
        changed = true;
    }
    if (cur.thumbnailUrl.isEmpty() && !detail.thumbnailUrl.isEmpty()) {
        cur.thumbnailUrl = detail.thumbnailUrl;
        changed = true;
    }
    if (cur.viewCount == 0 && detail.viewCount > 0) {
        cur.viewCount = detail.viewCount;
        changed = true;
    }
    // The detail's date (/next dateText) is applied too. It was missing: the
    // queue item stayed without a date even if the detail brought one, and its
    // "A continuación" row (and the page meta) came out without it.
    if (cur.publishedAt.isEmpty() && !detail.publishedAt.isEmpty()) {
        cur.publishedAt = detail.publishedAt;
        changed = true;
    }
    if (changed)
        emit metadataChanged();
}

// Overwrites the current item's title with the /next translation
// (only if the id matches). Also updates the snapshots if the item
// is stored in them, so the queue/overlay UI shows the translated
// title without rebuilding the list.
void PlayerController::renameCurrent(const QString &videoId,
                                     const QString &title)
{
    if (m_index < 0 || m_index >= m_queue.size())
        return;
    if (title.isEmpty())
        return;
    VideoInfo &cur = m_queue[m_index];
    if (cur.id != videoId || cur.title == title)
        return;
    cur.title = title;
    emit metadataChanged();
}

void PlayerController::next()
{
    if (hasNext())
        playIndex(m_index + 1);
    else if (m_repeat && !m_queue.isEmpty())
        playIndex(0);
}

void PlayerController::previous()
{
    // YouTube behavior: if the current video is already ahead (past the
    // threshold), "Anterior" restarts the current video; only if it is at the
    // beginning does it jump to the previous video in the queue.
    if (m_position > kPrevRestartThreshold) {
        seek(0.0);
        return;
    }
    previousTrack();
}

void PlayerController::previousTrack()
{
    if (hasPrevious())
        playIndex(m_index - 1);
}

// Shuffle: randomly reorders ONLY the queue ahead of the video being
// played; the current one does not move (like YouTube). Fisher-Yates with
// QRandomGenerator::global. Resume positions are keyed by INDEX and the
// reordering invalidates those of the indices that move, so all are
// discarded except the current one (which stays where it was). Re-enabling the mode
// shuffles whatever remains.
void PlayerController::setShuffle(bool on)
{
    if (m_shuffle == on)
        return;
    m_shuffle = on;
    const int tail = (m_index >= 0) ? m_queue.size() - (m_index + 1) : 0;
    if (!on || tail <= 1)
        return;
    QVector<VideoInfo> cola = m_queue.mid(m_index + 1);
    for (int i = cola.size() - 1; i > 0; --i) {
        const int j = QRandomGenerator::global()->bounded(i + 1);
        if (j != i)
            cola.swapItemsAt(i, j);
    }
    for (int i = m_index + 1; i < m_queue.size(); ++i)
        m_queue[i] = cola.at(i - m_index - 1);
    for (auto it = m_indexStartMs.begin(); it != m_indexStartMs.end();) {
        if (it.key() != m_index)
            it = m_indexStartMs.erase(it);
        else
            ++it;
    }
    emit queueChanged();
}

void PlayerController::setRepeat(bool on)
{
    if (m_repeat == on)
        return;
    m_repeat = on;
    NT_DEBUG("E2E repeat=%d\n", m_repeat ? 1 : 0);
}

void PlayerController::savePosition()
{
    const VideoInfo cur = current();
    const bool active = m_phase == QLatin1String("playing")
                        || m_phase == QLatin1String("paused");
    if (!m_history || !active || cur.id.isEmpty())
        return;
    const qlonglong posMs = qRound(m_position * 1000.0);
    // Discards the write if it is the SAME video and the position barely
    // moved (flush with the video paused, or identical position). With the
    // short interval (20 s) this is what keeps the cost at one INSERT
    // per 2 s of real playback.
    if (cur.id == m_lastSavedId
        && qAbs(posMs - m_lastSavedMs) < kPositionFlushMinDeltaMs)
        return;
    m_lastSavedId = cur.id;
    m_lastSavedMs = posMs;
    m_history->add(cur.id, posMs);
}

void PlayerController::playPause()
{
    if (m_index < 0)
        return;
    if (m_phase == QLatin1String("playing")) {
        m_player->setPaused(true);
        setPhase(QStringLiteral("paused"));
        savePosition();
    } else {
        m_player->setPaused(false);
        m_seekBack = m_position;
        setPhase(QStringLiteral("playing"));
    }
}

void PlayerController::toggleMute()
{
    m_muted = !m_muted;
    m_player->setVolume(m_muted ? 0.0 : m_volume);
    emit volumeChanged();
}

void PlayerController::setMuted(bool muted)
{
    m_muted = muted;
    m_player->setVolume(muted ? 0.0 : m_volume);
    emit volumeChanged();
}

void PlayerController::setVolume(double volume)
{
    m_volume = qBound(0.0, volume, double(m_volumeMax));
    if (m_volume > 0.0)
        m_muted = false;
    const int muted = m_muted ? 1 : 0;
    m_player->setProperty("volume_muted", muted);
    m_player->setVolume(m_volume);
    emit volumeChanged();
}

void PlayerController::setVolumeMax(double maxPercent)
{
    m_volumeMax = qBound(100, int(qRound(maxPercent)), 200);
    // The current volume may sit above the new cap: clamp it.
    if (m_volume > double(m_volumeMax))
        m_volume = double(m_volumeMax);
    m_player->setVolumeMax(double(m_volumeMax));
    m_player->setVolume(m_volume);
    emit volumeChanged();
}

void PlayerController::seek(double seconds)
{
    m_player->seek(seconds);
}

void PlayerController::reloadCurrent()
{
    if (m_index < 0)
        return;
    const double target = m_position;
    savePosition();
    m_seekBack = target;
    NT_DEBUG("E2E reloadCurrent target=%.1f seekBack=%.1f\n", target, m_seekBack);
    m_player->stop();
    setPhase(QStringLiteral("loading"));
    startResolve(/*forceRefresh*/ true);
}

void PlayerController::onStreamStalled()
{
    if (m_index < 0)
        return;
    // A stall with playback stabilized long enough does not count
    // against the attempt limit (normal case: URL budget drained
    // after a long stretch).
    if (m_stableSince.isValid() && m_stableSince.elapsed() > kRecoverBudgetResetMs)
        m_recoverAttempts = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Rapid relapse (<8s since the last attempt): the local micro-seek was
    // not enough -> symptom of real network trouble (URL expired/cut/gated session). Here
    // the budget IS spent and it re-resolves with fresh URLs via
    // reloadCurrent(); it only gives up when the budget is exhausted.
    const bool rapidRelapse = m_lastRecoverMs > 0
                              && now - m_lastRecoverMs < 8000;
    if (rapidRelapse) {
        if (m_recoverAttempts >= 3) {
            // Budget exhausted: give up. The app restarts with the last
            // video (MainWindow listens for it) because restarting gets out of
            // the gated session.
            setPhase(QStringLiteral("error"));
            m_errorMessage = QStringLiteral(
                "La reproducción se interrumpió y no se pudo recuperar.");
            emit recoveryExhausted();
            return;
        }
        ++m_recoverAttempts;
        m_lastRecoverMs = now;
        // Relapse with gated-session symptoms: rotate the session (new
        // visitorData) before re-resolving, or the re-resolution inside the
        // same session fails again (the gate is per session, not per URL).
        m_yt->rotateStreamSession();
        NT_DEBUG("E2E streamStalled -> reloadCurrent intento=%d pos=%.1f\n",
                 m_recoverAttempts, m_player->position());
        reloadCurrent();
        return;
    }
    // First stall detected: mpv's audio buffer ran dry but the video
    // keeps going (HW); as the user verified, a micro-seek to the current
    // position revives the track instantly without re-resolving the network (reloadCurrent() would have
    // taken 8-25s re-downloading URLs). It is local and cheap: it does not count against the
    // network attempt budget. mpv can emit streamStalled repeatedly
    // while the demuxer re-primes; if it relapses before 8s the previous branch
    // decides it is real network trouble and re-resolves spending budget.
    m_lastRecoverMs = now;
    NT_DEBUG("E2E streamStalled -> micro-seek local pos=%.1f\n",
             m_player->position());
    m_player->seek(m_player->position());
    return;
}

// Playback interruption (premature EOF from a gated URL session with 403, or
// an external audio track that never opened): unlike the stall
// (local micro-seek, cheap), here the stream is already finished or running without audio, so
// the recovery is to rotate the session (the gate is per session, not per URL) and
// re-resolve with fresh URLs in a NEW session, resuming at the current
// position. Bounded by the same attempt budget.
void PlayerController::onStreamInterrupted()
{
    if (m_index < 0)
        return;
    // Stable playback long enough: resets the attempt
    // counter (a one-off interruption every many seconds must not
    // drain the budget accumulated from earlier cuts). 5 min, NOT 60 s:
    // the gated session dies at ~60 s and with 60 s the restart wiped the
    // budget right before each death → endless reload loop.
    if (m_stableSince.isValid() && m_stableSince.elapsed() > kRecoverBudgetResetMs)
        m_recoverAttempts = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Rapid relapse (<8s since the last attempt) or budget exhausted:
    // the session is still gated -> give up (MainWindow listens for the signal and
    // restarts the app with the last video: restarting gets out of the session).
    if (m_recoverAttempts >= 3 || (m_lastRecoverMs > 0
                                   && now - m_lastRecoverMs < 8000)) {
        setPhase(QStringLiteral("error"));
        m_errorMessage = QStringLiteral(
            "La reproducción se interrumpió y no se pudo recuperar.");
        emit recoveryExhausted();
        return;
    }
    m_lastRecoverMs = now;
    ++m_recoverAttempts;
    // Rotate the session BEFORE re-resolving: with the new visitorData the
    // re-resolution leaves the gated session (if the new session does not
    // breathe either, the budget is exhausted in ≤3 attempts and the error is shown).
    m_yt->rotateStreamSession();
    NT_DEBUG("E2E streamInterrupted -> reloadCurrent intento=%d pos=%.1f\n",
             m_recoverAttempts, m_player->position());
    reloadCurrent();
}

void PlayerController::stop()
{
    savePosition();
    m_player->stop();
    m_index = -1;
    m_queue.clear();
    m_seekBack = 0.0;
    setPhase(QStringLiteral("stopped"));
    emit currentChanged();
    emit queueChanged();
}