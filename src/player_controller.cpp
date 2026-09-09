#include "player_controller.h"

#include <cstdio>

#include "db/repositories.h"
#include "playerwidget.h"
#include "yt.h"

PlayerController::PlayerController(Yt *yt, PlayerWidget *player,
                                   HistoryRepo *history,
                                   QObject *parent)
    : QObject(parent)
    , m_yt(yt)
    , m_player(player)
    , m_history(history)
{
    ensurePlayerConnections();
}

void PlayerController::ensurePlayerConnections()
{
    if (!m_player)
        return;
    disconnect(m_stateConn);
    disconnect(m_posConn);
    disconnect(m_durConn);
    disconnect(m_errConn);
    m_stateConn = connect(m_player, &PlayerWidget::stateChanged, this,
                          &PlayerController::onPlayerState);
    m_posConn = connect(m_player, &PlayerWidget::positionChanged, this,
                        &PlayerController::onPlayerPosition);
    m_durConn = connect(m_player, &PlayerWidget::durationChanged, this,
                        &PlayerController::onPlayerPosition);
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
                                 int startIndex)
{
    if (videos.isEmpty() || startIndex < 0 || startIndex >= videos.size())
        return;
    m_queue = videos;
    emit queueChanged();
    playIndex(startIndex);
}

void PlayerController::playIndex(int index)
{
    if (index < 0 || index >= m_queue.size())
        return;
    savePosition();
    m_index = index;
    m_seekBack = 0.0;
    m_errorMessage.clear();
    setPhase(QStringLiteral("loading"));
    emit currentChanged();

    const VideoInfo &v = m_queue[index];
    if (m_history && !v.id.isEmpty())
        m_history->add(v.id, 0);

    if (v.id.isEmpty()) {
        setPhase(QStringLiteral("error"));
        return;
    }
    startResolve();
}

void PlayerController::startResolve()
{
    disconnect(m_resolveConn);
    disconnect(m_resolveErrConn);
    const int gen = ++m_resolveGen;
    m_resolveConn = connect(m_yt, &Yt::streamReady, this,
                            [this, gen](const StreamInfo &s) {
                                if (gen == m_resolveGen)
                                    onStreamResolved(s);
                            });
    m_resolveErrConn = connect(m_yt, &Yt::error, this,
                               [this, gen](const QString &message) {
                                   if (gen == m_resolveGen)
                                       resolveFailed(message);
                               });
    m_yt->resolveStream(m_queue[m_index].id, m_maxHeight);
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
    m_player->playUrl(stream.url);
}

void PlayerController::resolveFailed(const QString &message)
{
    m_errorMessage = message;
    setPhase(QStringLiteral("error"));
}

void PlayerController::onPlayerState()
{
    const QString s = m_player->state();
    if (s == QLatin1String("playing")) {
        setPhase(QStringLiteral("playing"));
        if (m_seekBack > 2.0) {
            m_player->seek(m_seekBack);
            m_seekBack = 0.0;
        }
    } else if (s == QLatin1String("ended")) {
        if (hasNext()) {
            savePosition();
            playIndex(m_index + 1);
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

void PlayerController::onPlayerError(const QString &message)
{
    m_errorMessage = message;
    setPhase(QStringLiteral("error"));
}

void PlayerController::setPhase(const QString &phase)
{
    if (m_phase == phase)
        return;
    m_phase = phase;
    fprintf(stderr, "E2E phase=%s\n", phase.toUtf8().constData());
    emit phaseChanged(m_phase);
}

VideoInfo PlayerController::current() const
{
    if (m_index >= 0 && m_index < m_queue.size())
        return m_queue.at(m_index);
    return VideoInfo();
}

void PlayerController::next()
{
    if (hasNext())
        playIndex(m_index + 1);
}

void PlayerController::previous()
{
    if (hasPrevious())
        playIndex(m_index - 1);
}

void PlayerController::savePosition()
{
    const VideoInfo cur = current();
    if (m_history && !cur.id.isEmpty() && m_phase != QLatin1String("stopped"))
        m_history->add(cur.id, qRound(m_position * 1000.0));
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
    m_player->setVolume(m_muted ? 0.0 : 100.0);
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
    m_player->stop();
    setPhase(QStringLiteral("loading"));
    startResolve();
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