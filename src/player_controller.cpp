#include "player_controller.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QTimer>

#include "db/repositories.h"
#include "debug.h"
#include "playerwidget.h"
#include "yt.h"

namespace {
constexpr int kPositionFlushMs = 60000;
// Ventana de reproducción "estable" que reinicia el presupuesto de intentos de
// recuperación (una interrupción puntual tras un tramo largo sano no debe
// agotar lo acumulado). 5 min y NO 60 s: la sesión gateada de YouTube muere a
// los ~60 s de carga (EOF prematuro a ~1:03) y con 60 s el reinicio borraba el
// presupuesto justo antes de cada muerte → reload → 60 s → reload… bucle sin
// fin. Con 5 min el gate nunca llega a reiniciar y el presupuesto se agota
// (máx. 3 intentos → error claro); las interrupciones genuinamente raras
// (pasados 5 min de reproducción sana) siguen sin gastar presupuesto.
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
        // Reproducción estable ≥5 min seguidos: cierra el episodio de
        // recuperación (MainWindow desarma el reinicio automático por sesión
        // gateada; sin esto, tras un reinicio que funcionó, la próxima sesión
        // gateada ya no podría reiniciar).
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

void PlayerController::playIndex(int index)
{
    if (index < 0 || index >= m_queue.size())
        return;
    savePosition();
    m_index = index;
    m_seekBack = 0.0;
    m_errorMessage.clear();
    m_pendingStartMs = m_indexStartMs.value(index, 0);
    m_recoverAttempts = 0;
    m_lastRecoverMs = 0;
    setPhase(QStringLiteral("loading"));
    emit currentChanged();

    const VideoInfo &v = m_queue[index];
    if (m_history && !v.id.isEmpty())
        m_history->add(v.id, m_pendingStartMs);

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
    double from = m_seekBack;
    if (m_pendingStartMs > 0) {
        from = double(m_pendingStartMs) / 1000.0;
        m_pendingStartMs = 0;
    }
    // Consumida: el mismo índice no vuelve a arrancar desde esa posición en
    // futuras visitas (p. ej. previous() tras terminar).
    m_indexStartMs.remove(m_index);
    // La preferencia de idioma de audio se aplica justo antes de cargar la
    // fuente: sin "alang" mpv elegiría la pista con DEFAULT=YES (el original)
    // aunque el master venga reordenado con el doblaje primero.
    m_player->setAudioLanguage(m_yt->audioLanguage());
    // Reproducción directa (edge): la pista de audio es un flujo aparte que
    // mpv fusiona con --audio-file (duración exacta + seek fiable, sin el HLS
    // de segmento único). Vacío limpia la opción para el flujo clásico.
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
    if (changed)
        emit metadataChanged();
}

// Sobrescribe el título del ítem actual con la traducción del /next
// (solo si el id coincide). Actualiza también las instantáneas si el ítem
// está guardado en ellas, para que la UI de la cola/overlay muestre el título
// traducido sin reconstruir la lista.
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
}

void PlayerController::previous()
{
    if (hasPrevious())
        playIndex(m_index - 1);
}

void PlayerController::savePosition()
{
    const VideoInfo cur = current();
    const bool active = m_phase == QLatin1String("playing")
                        || m_phase == QLatin1String("paused");
    if (m_history && active && !cur.id.isEmpty())
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
    // El volumen actual puede quedar por encima del nuevo tope: capar.
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
    // Un atasco con la reproducción estabilizada el rato suficiente no cuenta
    // contra el límite de intentos (caso normal: presupuesto de URL agotado
    // tras un tramo largo).
    if (m_stableSince.isValid() && m_stableSince.elapsed() > kRecoverBudgetResetMs)
        m_recoverAttempts = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Recaída rápida (<8s desde el último intento): el micro-seek local no
    // bastó -> síntoma de red real (URL caducada/cortada/sesión gateada). Aquí
    // sí se consume el presupuesto y se re-resuelve con URLs frescas vía
    // reloadCurrent(); solo se abandona cuando el presupuesto se agota.
    const bool rapidRelapse = m_lastRecoverMs > 0
                              && now - m_lastRecoverMs < 8000;
    if (rapidRelapse) {
        if (m_recoverAttempts >= 3) {
            // Presupuesto agotado: se abandona. La app reinicia con el último
            // vídeo (lo escucha MainWindow) porque reiniciar sale de la sesión
            // gateada.
            setPhase(QStringLiteral("error"));
            m_errorMessage = QStringLiteral(
                "La reproducción se interrumpió y no se pudo recuperar.");
            emit recoveryExhausted();
            return;
        }
        ++m_recoverAttempts;
        m_lastRecoverMs = now;
        // Recaída con síntomas de sesión gateada: rotar la sesión (visitorData
        // nuevo) antes de re-resolver, o la re-resolución dentro de la misma
        // sesión vuelve a fallar (el gate es de sesión, no de URL).
        m_yt->rotateStreamSession();
        NT_DEBUG("E2E streamStalled -> reloadCurrent intento=%d pos=%.1f\n",
                 m_recoverAttempts, m_player->position());
        reloadCurrent();
        return;
    }
    // Primer atasco detectado: el audio-buffer de mpv se agotó pero el vídeo
    // sigue (HW); como el usuario comprobó, un micro-seek a la posición actual
    // revive la pista al instante sin re-resolver red (reloadCurrent() hubiera
    // tardado 8-25s re-bajando URLs). Es local y barato: no cuenta contra el
    // presupuesto de intentos de red. mpv puede emitir streamStalled repetido
    // mientras el demuxer se re-prima; si recae antes de 8s la rama anterior
    // decide que es red real y re-resuelve consumiendo presupuesto.
    m_lastRecoverMs = now;
    NT_DEBUG("E2E streamStalled -> micro-seek local pos=%.1f\n",
             m_player->position());
    m_player->seek(m_player->position());
    return;
}

// Interrupción de la reproducción (EOF prematuro por sesión de URLs gateada
// con 403, o pista de audio externa que nunca abrió): a diferencia del atasco
// (micro-seek local, barato) aquí el stream ya se terminó o va sin audio, así
// que la recuperación es rotar la sesión (el gate es de sesión, no de URL) y
// re-resolver con URLs frescas en una sesión NUEVA, reanudando en la posición
// actual. Acotado por el mismo presupuesto de intentos.
void PlayerController::onStreamInterrupted()
{
    if (m_index < 0)
        return;
    // Reproducción estable el rato suficiente: reinicia el contador de
    // intentos (una interrupción puntual cada muchos segundos no debe
    // agotar el presupuesto acumulado de cortes anteriores). 5 min, NO 60 s:
    // la sesión gateada muere a los ~60 s y con 60 s el reinicio borraba el
    // presupuesto justo antes de cada muerte → bucle de reload sin fin.
    if (m_stableSince.isValid() && m_stableSince.elapsed() > kRecoverBudgetResetMs)
        m_recoverAttempts = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Recaída rápida (<8s desde el último intento) o presupuesto agotado:
    // la sesión sigue gateada -> se abandona (MainWindow escucha la señal y
    // reinicia la app con el último vídeo: reiniciar sale de la sesión).
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
    // Rotar la sesión ANTES de re-resolver: con el visitorData nuevo la
    // re-resolución sale de la sesión gateada (si la sesión nueva tampoco
    // respira, el presupuesto se agota en ≤3 intentos y se muestra el error).
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