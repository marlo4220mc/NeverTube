#include "playerwidget.h"

#include <QDateTime>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <cmath>

#include "debug.h"
#include "i18n.h"
#include "playback_backend.h"

// Playback time with no active audio track that declares the stream gated
// (YouTube audio 403) and fires the early re-resolution (checkStall).
//
// It was 10 s, and that was TOO SHORT for a slow machine: the external
// audio (the one of the "edge" mode, a separate file that the CDN serves)
// can take more than that to arrive, and the watchdog then did the WORST
// thing: it re-resolved, which throws away the URLs that were being
// downloaded and starts over with a new session. The symptom was "the
// first opening has no audio (and it is fixed when you change the video)".
//
// 25 s is still far below the ~60 s that the gated session takes to die
// with the premature EOF, so the gate keeps its purpose (re-resolving
// early instead of waiting a minute of silence).
static constexpr qint64 kAudioDetectMs = 25000;

PlayerWidget::PlayerWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_NoSystemBackground);
    // The factory picks the engine per platform: mpv on x64 (protected piece,
    // Phase 1: all mpv integration lives in MpvBackend) or FfmpegBackend on
    // the x86 (no libmpv). The rest of the widget only knows the interface.
    m_backend = createPlaybackBackend(this);

    connect(m_backend, &PlaybackBackend::fileLoaded, this,
            &PlayerWidget::onFileLoaded);
    connect(m_backend, &PlaybackBackend::streamStart, this,
            &PlayerWidget::onStreamStart);
    connect(m_backend, &PlaybackBackend::endFile, this,
            &PlayerWidget::onEndFile);
    connect(m_backend, &PlaybackBackend::positionUpdated, this,
            &PlayerWidget::onPositionUpdated);
    connect(m_backend, &PlaybackBackend::durationUpdated, this,
            &PlayerWidget::onDurationUpdated);
    connect(m_backend, &PlaybackBackend::videoWidthUpdated, this,
            &PlayerWidget::onVideoWidth);
    connect(m_backend, &PlaybackBackend::videoHeightUpdated, this,
            &PlayerWidget::onVideoHeight);
    connect(m_backend, &PlaybackBackend::pauseUpdated, this,
            &PlayerWidget::onPauseUpdated);
    connect(m_backend, &PlaybackBackend::cacheTimeUpdated, this,
            &PlayerWidget::onCacheTime);
    connect(m_backend, &PlaybackBackend::logMessage, this,
            &PlayerWidget::onLogMessage);
    // The engine asks for a repaint: drain events and force update().
    connect(m_backend, &PlaybackBackend::frameReady, this,
            &PlayerWidget::onFrameReady);

    // Watches that the position advances during playback: if it freezes for
    // > 5 s it emits streamStalled so the controller re-resolves.
    m_stallTimer = new QTimer(this);
    m_stallTimer->setInterval(1000);
    connect(m_stallTimer, &QTimer::timeout, this, &PlayerWidget::checkStall);
    // Armed by updateWatchdogTimer() when playback starts, not here: see its
    // comment in the header.
    connect(this, &PlayerWidget::stateChanged, this,
            &PlayerWidget::updateWatchdogTimer);

    // x86 (FfmpegBackend): if another window covers the video, the engine
    // stops converting and painting while it lasts (real CPU/GPU saving;
    // Win7/DWM does not report occlusion, so this slow timer detects it with
    // a z-order walk). Does not apply to mpv (occlusionSkipSupported()==false):
    // stopping the render there would accumulate frames (see AGENTS.md "Mini
    // player").
    if (m_backend->occlusionSkipSupported()) {
        m_occlusionTimer = new QTimer(this);
        m_occlusionTimer->setInterval(250);
        connect(m_occlusionTimer, &QTimer::timeout, this,
                &PlayerWidget::checkVideoCovered);
        m_occlusionTimer->start();
    }
}

PlayerWidget::~PlayerWidget()
{
    makeCurrent();
    if (m_backend) {
        m_backend->destroyRenderContext();
        m_backend->shutdown();
    }
    doneCurrent();
}

QString PlayerWidget::hwdec() const
{
    return m_backend ? m_backend->configuredHwdec() : QString();
}

double PlayerWidget::speed() const
{
    return m_backend ? m_backend->speed() : 1.0;
}

QString PlayerWidget::hwdecCurrent() const
{
    return m_backend ? m_backend->hwdecCurrent() : QString();
}

QString PlayerWidget::videoCodec() const
{
    return m_backend ? m_backend->videoCodec() : QString();
}

void PlayerWidget::initializeGL()
{
    NT_DEBUG("E2E initializeGL w=%d h=%d rctx=%p\n",
             width(), height(), m_backend->renderContextHandle());
    if (!m_backend->initGL()) {
        qWarning("player: fallo mpv_render_context_create");
        return;
    }
    // Event-driven redraw (AGENTS.md): the engine notifies when there is a
    // new frame to paint (mpv's update callback installed in
    // MpvBackend::initGL / per-frame frameReady of the FfmpegBackend) and
    // onFrameReady() forces QWidget::update(). No repaint poll: at rest
    // (pause, buffering, covered) nothing repaints. Replaces the old 33 ms
    // timer, redundant since the event path exists.
}

void PlayerWidget::paintGL()
{
    if (!m_backend->hasRenderContext()) {
        NT_DEBUG("E2E paintGL no-rctx\n");
        return;
    }
    // x86 (FfmpegBackend): if another window fully covers the video, do not
    // render — the engine already dropped the frames (avoids uploading
    // textures and painting under the window; on uncovering the most recent
    // frame is shown, no burst). Does NOT apply to mpv
    // (occlusionSkipSupported=false): its render must keep going so as not to
    // accumulate frames (see AGENTS.md mini player).
    if (m_backend->occlusionSkipSupported() && m_videoOccluded)
        return;
    QOpenGLFunctions *gl = QOpenGLContext::currentContext()->functions();
    GLint fbo = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    const int dpr = devicePixelRatio();
    // PROTECTED (AGENTS.md): direct render to the FBO in paintGL (current GL
    // context). The backend runs mpv_render_context_render with these same
    // parameters synchronously in this paint; the FBO and the context are the
    // widget's.
    m_backend->renderGL(fbo, width() * dpr, height() * dpr);
}

void PlayerWidget::playUrl(const QString &url, double startAt)
{
    if (url.isEmpty() || !m_backend->isValid())
        return;
    m_position = 0.0;
    m_duration = 0.0;
    // If the backend consumed the external audio at load (audio-files), the
    // pending URL is no longer needed; otherwise it is kept for the
    // attachAudio fallback at fileLoaded.
    if (m_backend->playUrl(url, startAt, m_pendingAudioFile)
        && !m_pendingAudioFile.isEmpty())
        m_pendingAudioFile.clear();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    m_lastProgressMs = nowMs;
    m_videoLoadMs = nowMs;
    // The clock of the audio starts with the load: the external audio (a
    // separate file) can take a while to arrive on a slow machine and the
    // watchdog must not fire before that.
    m_audioLoadMs = nowMs;
    m_cacheOk = false;
    emit positionChanged();
    emit durationChanged();
}

void PlayerWidget::stop()
{
    if (!m_backend->isValid())
        return;
    m_backend->stop();
}

void PlayerWidget::setPaused(bool paused)
{
    if (!m_backend->isValid())
        return;
    m_backend->setPaused(paused);
    if (!paused)
        m_lastProgressMs = QDateTime::currentMSecsSinceEpoch();
    m_paused = paused;
    updateWatchdogTimer();
}

void PlayerWidget::updateWatchdogTimer()
{
    const bool watching = m_backend && m_backend->isValid()
        && m_state == QLatin1String("playing") && !m_paused;
    if (watching) {
        if (!m_stallTimer->isActive())
            m_stallTimer->start();
    } else if (m_stallTimer->isActive()) {
        m_stallTimer->stop();
    }
}

void PlayerWidget::seek(double seconds)
{
    if (!m_backend->isValid())
        return;
    // Grace for the new seek: the position does not advance until the new
    // fragment arrives; it must not count as a stall.
    m_lastProgressMs = QDateTime::currentMSecsSinceEpoch();
    m_backend->seek(seconds);
}

void PlayerWidget::setVolume(double volume)
{
    if (!m_backend->isValid())
        return;
    m_backend->setVolume(volume);
}

void PlayerWidget::setVolumeMax(double maxPercent)
{
    if (!m_backend->isValid())
        return;
    m_backend->setVolumeMax(maxPercent);
}

void PlayerWidget::setSubVisibility(bool visible)
{
    if (!m_backend->isValid())
        return;
    m_backend->setSubVisibility(visible);
}

void PlayerWidget::setAudioFile(const QString &url)
{
    m_pendingAudioFile = url;
    if (!m_backend->isValid())
        return;
    // Classic flow: nothing to add; it also clears the external track
    // of a previous video with an audio-file lingering in the engine.
    if (url.isEmpty())
        m_backend->dropExternalAudio();
    // The external audio is attached IN the next playUrl (loadfile with the
    // per-file "audio-files" option), not with a loose audio-add here: adding
    // it before the loadfile would hook the track to the still-loaded
    // PREVIOUS stream and on re-resolution it could end up muted. The
    // attachAudio fallback in onFileLoaded() remains for when playUrl did not
    // consume the URL.
}

void PlayerWidget::setAudioLanguage(const QString &lang)
{
    m_backend->setAudioLanguage(lang);
}

void PlayerWidget::setInitialBufferSecs(double secs)
{
    m_initialBufferSecs = secs;
    // Do NOT destroy an ACTIVE buffer timer: the profile probe may adjust the
    // wait while the first video is already starting, and destroying the
    // timer would leave the video paused (setPaused(true)) with nobody to
    // resume it. If active, let it finish; the new wait applies to the next
    // start.
    if (m_bufferTimer && !m_bufferTimer->isActive()) {
        m_bufferTimer->stop();
        m_bufferTimer->deleteLater();
        m_bufferTimer = nullptr;
    }
}

void PlayerWidget::onFrameReady()
{
    m_backend->pollEvents();
    update();
}

void PlayerWidget::onFileLoaded()
{
    // External audio fallback: if playUrl did not consume it (old mpv without
    // the "audio-files" option), attach it now that the main file loaded
    // ("no file loaded" no longer applies).
    if (!m_pendingAudioFile.isEmpty() && m_backend->isValid())
        m_backend->attachAudio(m_pendingAudioFile);
    // It is also the first moment when track-list has the real tracks. We
    // query it once so WatchPage can filter the resolution menu: if the video
    // only reaches 720p, 1080p+ are not offered (the engine could not select
    // them and they would remain "inert clicks").
    m_videoMaxH = m_backend->probeMaxVideoHeight();
    NT_DEBUG("E2E qualityLimits max=%d\n", m_videoMaxH);
    emit qualityLimitsChanged(m_videoMaxH);
}

void PlayerWidget::onStreamStart()
{
    // Every new stream gives another chance to report codec/hwdec (for the
    // codec profile's safety net).
    m_hwdecProbed = false;
    m_hwdecLogSoftware = false;
    if (m_hwdecRetry)
        m_hwdecRetry->stop();
    m_lastProgressMs = QDateTime::currentMSecsSinceEpoch();
    if (m_state != QLatin1String("playing")) {
        m_state = QStringLiteral("playing");
        emit stateChanged();
    }
}

void PlayerWidget::onEndFile(PlaybackBackend::EndReason reason,
                             const QString &errorText)
{
    switch (reason) {
    case PlaybackBackend::EndReason::Quit:
        return;
    case PlaybackBackend::EndReason::Stop:
        // Intentional stop (stop()/replacement loadfile). Not end of
        // playback: it must NOT auto-advance. m_state is reset so that the
        // next START_FILE re-emits "playing" to the controller.
        m_state.clear();
        // No stateChanged() is emitted here (the listeners treat it as the
        // end of the stream), so the watchdog is disarmed by hand.
        updateWatchdogTimer();
        break;
    case PlaybackBackend::EndReason::Error:
        if (m_state != QLatin1String("error")) {
            m_state = QStringLiteral("error");
            emit stateChanged();
        }
        emit mediaError(errorText);
        return;
    default: {
        // EOF. Distinguish a natural end (position ~ duration) from a network
        // interruption mid-video.
        const bool naturalEnd =
            m_duration > 0.0 && m_position >= m_duration - 3.0;
        if (!naturalEnd) {
            if (m_duration > 0.0) {
                // Premature EOF with known duration: YouTube gated the
                // session (403 on audio and/or segments) and the demuxer ran
                // out of data. The controller re-resolves with fresh URLs and
                // resumes at the position (streamInterrupted). Without a
                // reliable duration (live), the classic error is kept.
                if (m_state != QLatin1String("error")) {
                    m_state = QStringLiteral("error");
                    emit stateChanged();
                }
                NT_DEBUG("E2E EOF prematuro pos=%.2f dur=%.2f\n",
                         m_position, m_duration);
                emit streamInterrupted();
                return;
            }
            if (m_state != QLatin1String("error")) {
                m_state = QStringLiteral("error");
                emit stateChanged();
            }
            emit mediaError(
                nt::tr("error.network"));
            return;
        }
        if (m_state != QLatin1String("ended")) {
            m_state = QStringLiteral("ended");
            emit stateChanged();
        }
        return;
    }
    }
}

void PlayerWidget::onPositionUpdated(double seconds)
{
    if (std::abs(seconds - m_position) > 0.05) {
        m_position = seconds;
        m_lastProgressMs = QDateTime::currentMSecsSinceEpoch();
        emit positionChanged();
    }
}

void PlayerWidget::onDurationUpdated(double seconds)
{
    m_duration = seconds;
    emit durationChanged();
}

void PlayerWidget::onVideoWidth(qint64 width)
{
    m_videoW = width;
    emitAspectIfReady();
}

void PlayerWidget::onVideoHeight(qint64 height)
{
    m_videoH = height;
    if (m_videoW > 0)
        NT_DEBUG("E2E mpv video-params %lldx%lld\n",
                 static_cast<long long>(m_videoW),
                 static_cast<long long>(m_videoH));
    emitAspectIfReady();
    if (m_videoW > 0 && m_videoH > 0)
        emit videoSizeChanged(int(m_videoW), int(m_videoH));
    if (!m_hwdecProbed && m_videoW > 0) {
        m_hwdecProbed = true;
        // The engine's log rules: if it already said "Using software
        // decoding", the profile is software even if hwdec-current reports a
        // backend (software devices on old GPUs). If it did not speak,
        // hwdec-current arrives "empty" until the engine activates the
        // decoder, and "no" when it is software; a real backend confirms HW.
        // If it comes empty, retry with a QTimer.
        if (m_hwdecLogSoftware) {
            emit streamDecodeInfoReady(videoCodec(), QStringLiteral("no"));
        } else if (hwdecCurrent().isEmpty()) {
            if (!m_hwdecRetry) {
                m_hwdecRetry = new QTimer(this);
                m_hwdecRetry->setInterval(150);
                connect(m_hwdecRetry, &QTimer::timeout, this,
                        &PlayerWidget::retryHwdec);
            }
            m_hwdecRetryAttempts = 0;
            m_hwdecRetry->start();
        } else {
            emit streamDecodeInfoReady(videoCodec(), hwdecCurrent());
        }
    }
    // Initial buffer wait at high resolution (>=1080p): here we already know
    // the stream's real height (at FILE_LOADED it is still 0). Starting with
    // a few seconds of cached content removes the micro-pauses of the first
    // seconds (closing of YouTube's fast-start after the http_multiple ramp).
    if (m_initialBufferSecs > 0.0 && m_videoH >= 1000
        && !(m_bufferTimer && m_bufferTimer->isActive())) {
        if (!m_bufferTimer) {
            m_bufferTimer = new QTimer(this);
            m_bufferTimer->setInterval(250);
            connect(m_bufferTimer, &QTimer::timeout, this,
                    &PlayerWidget::checkInitialBuffer);
        }
        m_bufferStart.start();
        setPaused(true);
        m_bufferTimer->start();
        NT_DEBUG("E2E buffer wait start secs=%.1f h=%lld\n",
                 m_initialBufferSecs,
                 static_cast<long long>(m_videoH));
    }
}

void PlayerWidget::onPauseUpdated(bool paused)
{
    // Tracked to know whether the user paused: on pause the position does not
    // advance and must not be interpreted as a stall.
    if (!paused && m_paused)
        m_lastProgressMs = QDateTime::currentMSecsSinceEpoch();
    m_paused = paused;
}

void PlayerWidget::onCacheTime(double seconds)
{
    if (seconds > 0.2)
        m_cacheOk = true; // there is data: the video track does load
    emit cacheTimeUpdated(seconds);
}

void PlayerWidget::onLogMessage(const QString &prefix, const QString &text)
{
    Q_UNUSED(prefix);
    if (text.contains(QLatin1String("Using software decoding"))) {
        m_hwdecLogSoftware = true;
        // Late correction: the HW verdict may have been emitted before this
        // line arrived (hwdec-current reported a bogus backend). Re-emit "no"
        // so the safety net downgrades the profile.
        if (m_hwdecProbed) {
            if (m_hwdecRetry)
                m_hwdecRetry->stop();
            emit streamDecodeInfoReady(videoCodec(), QStringLiteral("no"));
        }
    } else if (text.contains(
                   QLatin1String("Using hardware decoding"))) {
        m_hwdecLogSoftware = false;
    }
}

void PlayerWidget::retryHwdec()
{
    // The engine's log already ruled software: no backend to wait for.
    if (m_hwdecLogSoftware) {
        if (m_hwdecRetry)
            m_hwdecRetry->stop();
        emit streamDecodeInfoReady(videoCodec(), QStringLiteral("no"));
        return;
    }
    // hwdec-current was empty at the first video-params: retry until the
    // engine resolves it (or a safety cap of ~6 s).
    if (++m_hwdecRetryAttempts >= 40) {
        if (m_hwdecRetry)
            m_hwdecRetry->stop();
        emit streamDecodeInfoReady(videoCodec(), hwdecCurrent());
        return;
    }
    const QString hwd = hwdecCurrent();
    if (hwd.isEmpty())
        return;
    m_hwdecRetry->stop();
    NT_DEBUG("E2E hwdec probe retry codec=%s hw=%s\n",
             videoCodec().toUtf8().constData(), hwd.toUtf8().constData());
    emit streamDecodeInfoReady(videoCodec(), hwd);
}

// x86: FPS drop when opening a floating window above the video on Win7. DWM
// does not tell the app it is covered (it keeps receiving paint events and
// painting frames nobody sees, burning the GMA 950 right when the compositor
// competes — hence the drop with low app CPU/RAM: the bottleneck is the
// presentation, not the computation). This z-order walk (GetWindow up to our
// top-level) detects whether a visible, non-transparent window other than
// ours fully covers the video rect.
#ifdef Q_OS_WIN
bool PlayerWidget::occludedByTopWindow()
{
    QWidget *top = window();
    if (!top)
        return false;
    // WId on Qt5 Windows is quintptr, not HWND: the right cast is
    // reinterpret_cast (MSVC rejects static_cast between integer and pointer).
    const HWND topHwnd = reinterpret_cast<HWND>(top->winId());
    const HWND vidHwnd = reinterpret_cast<HWND>(winId());
    if (!topHwnd || !vidHwnd)
        return false;
    RECT vr;
    if (!GetWindowRect(vidHwnd, &vr))
        return false;
    // The GetWindow chain only lists top-level windows (child widgets, like
    // this QOpenGLWidget, do not appear). The windows ABOVE ours in the
    // z-order are the ones that come before topHwnd.
    // GetWindow(GW_HWNDFIRST) does not exist with an argument: the first
    // top-level of the z-order is obtained with GetTopWindow(NULL).
    HWND h = GetTopWindow(nullptr);
    for (; h && h != topHwnd; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsWindowVisible(h) || IsIconic(h))
            continue;
        // Transparent/layered windows (tooltips, decorative): they do not
        // cover real content.
        const LONG_PTR ex = GetWindowLongPtr(h, GWL_EXSTYLE);
        if (ex & WS_EX_LAYERED)
            continue;
        RECT wr;
        if (!GetWindowRect(h, &wr))
            continue;
        if (wr.left <= vr.left && wr.top <= vr.top &&
            wr.right >= vr.right && wr.bottom >= vr.bottom)
            return true;
    }
    return false;
}
#endif

void PlayerWidget::checkVideoCovered()
{
    if (!m_backend || !m_backend->occlusionSkipSupported())
        return;
    const bool covered =
#ifdef Q_OS_WIN
        occludedByTopWindow();
#else
        false;
#endif
    if (covered != m_videoOccluded) {
        m_videoOccluded = covered;
        m_backend->setVideoOccluded(covered);
        NT_DEBUG("E2E vídeo %s (pipeline %s)\n",
                 covered ? "cubierto" : "visible",
                 covered ? "pausado" : "reanudado");
    }
}

void PlayerWidget::checkInitialBuffer()
{
    if (!m_backend->isValid() || !m_bufferTimer) {
        if (m_bufferTimer)
            m_bufferTimer->stop();
        return;
    }
    // Reasonable wait cap: do not leave the user staring at an endless spinner
    // if the network cannot fill the target. 25 s maximum wait.
    if (m_bufferStart.elapsed() > 25000) {
        NT_DEBUG("E2E buffer wait timeout (secs=%.1f)\n", m_initialBufferSecs);
        setPaused(false);
        m_bufferTimer->stop();
        return;
    }
    const double cacheDur = m_backend->cacheDuration();
    // cache-idle: the download already finished (everything downloadable is):
    // we can start already even if cacheDur is low (from a local URL, e.g.).
    const bool cacheIdle = m_backend->cacheIdle();
    if (m_initialBufferSecs <= 0.0) {
        setPaused(false);
        m_bufferTimer->stop();
        return;
    }
    // More resolution = more headroom needed: 1080p asks for `secs`, higher
    // heights scale proportionally (e.g. 2160p asks for double).
    const double target = m_initialBufferSecs
                          * qMax(1.0, double(m_videoH) / 1080.0);
    NT_DEBUG("E2E buffer wait cache=%.1f target=%.1f idle=%d\n", cacheDur,
             target, cacheIdle ? 1 : 0);
    if (cacheDur >= target || cacheIdle) {
        NT_DEBUG("E2E buffer wait done (cache=%.1f target=%.1f)\n", cacheDur,
                 target);
        setPaused(false);
        m_bufferTimer->stop();
    }
}

void PlayerWidget::checkStall()
{
    if (!m_backend->isValid() || m_state != QLatin1String("playing"))
        return;
    if (m_paused)
        return;
    if (m_bufferTimer && m_bufferTimer->isActive())
        return; // initial buffer wait in progress: not a stall
    if (m_duration <= 0.0 || m_position >= m_duration - 2.0)
        return; // no reliable duration or near the end (imminent EOF)
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // (a) The video track does not load: the demuxer caches nothing. Since
    // the audio goes through an external file, the position advances anyway
    // and the position watchdog does not see it; the user sees the last
    // previous frame frozen.
    if (!m_cacheOk && m_videoLoadMs > 0 && now - m_videoLoadMs > 8000) {
        m_videoLoadMs = now; // do not fire in a loop while the controller recovers
        NT_DEBUG("E2E streamStalled sin-video pos=%.2f dur=%.2f\n", m_position,
                 m_duration);
        emit streamStalled();
        return;
    }
    // (a.2) No active audio track after ~10 s of playback: YouTube rejected
    // the audio (403 of a gated session) and the video goes mute. It does not
    // depend on the mode (external or muxed audio): with muxed HLS the gate
    // also leaves the engine without audio (audio=eof) and before, only the
    // EXTERNAL audio was watched here, so HLS exhausted ~60 s muted per
    // attempt until the premature EOF. Re-resolve early (with fresh URLs)
    // instead of waiting a minute; with duration >0 (all of the above),
    // streams without real audio (silence) do not exist in practice.
      // The diagnosis of the audio: it is reported ONCE per stream, so the
      // log says what mpv has (how many tracks, how many of them are audio,
      // which one is selected and its format) without filling the log.
      if (m_state == QLatin1String("playing") && m_position > 2.0
          && !m_audioProbed) {
          m_audioProbed = true;
          const bool hasAudio = m_backend->hasActiveAudio();
          qint64 nTracks = -1, nAudio = -1, selAudio = -1;
          QString fmt;
          m_backend->probeAudioState(&nTracks, &nAudio, &selAudio, &fmt);
          NT_DEBUG("E2E audio state has=%d tracks=%lld audio=%lld sel=%lld "
                   "fmt=%s\n", int(hasAudio), nTracks, nAudio, selAudio,
                   qPrintable(fmt));
      }
    if (!m_backend->hasActiveAudio() && m_audioLoadMs > 0 && now - m_audioLoadMs > kAudioDetectMs
        && m_position > 2.0) {
        // BEFORE re-resolving (which throws away the URLs that are being
        // downloaded and starts a new session): if the audio track ARRIVED
        // and it is simply not selected, it is selected here. That is the
        // case of the external audio (the "edge" mode) on a slow machine:
        // the track takes a while to arrive and mpv does not always select
        // it, and the app used to throw away a perfectly good download.
        if (m_backend->ensureAudioSelected()) {
            m_audioLoadMs = now;   // give it a new margin
            NT_DEBUG("E2E audio: la pista llego y se ha seleccionado\n");
            return;
        }
        m_audioLoadMs = now; // do not fire in a loop while it recovers
        NT_DEBUG("E2E streamInterrupted sin-audio pos=%.2f dur=%.2f\n",
                 m_position, m_duration);
        emit streamInterrupted();
        return;
    }
    // (b) Position frozen while playing.
    if (m_lastProgressMs <= 0 || now - m_lastProgressMs < 5000)
        return;
    // Refresh so as not to fire in a loop while the controller re-resolves.
    m_lastProgressMs = now;
    NT_DEBUG("E2E streamStalled pos=%.2f dur=%.2f\n", m_position, m_duration);
    emit streamStalled();
}

void PlayerWidget::emitAspectIfReady()
{
    if (m_videoW > 0 && m_videoH > 0) {
        const double aspect = double(m_videoW) / double(m_videoH);
        if (qAbs(aspect - m_aspect) > 0.001) {
            m_aspect = aspect;
            emit videoAspectChanged(aspect);
        }
    }
}