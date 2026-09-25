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
#include "playback_backend.h"

// Tiempo de reproducción sin pista de audio activa que declara el stream
// gateado (audio 403 de YouTube) y dispara la re-resolución temprana
// (checkStall). ~10 s por intento en lugar de esperar el EOF prematuro de los
// ~60 s del HLS maestro. Ver PlayerWidget::checkStall.
static constexpr qint64 kAudioDetectMs = 10000;

PlayerWidget::PlayerWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    // La fábrica elige el motor por plataforma: mpv en x64 (pieza protegida,
    // Fase 1: toda la integración mpv vive en MpvBackend) o FfmpegBackend en
    // la x86 (sin libmpv). El resto del widget solo conoce la interfaz.
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
    // El motor pide repintar: drenar eventos y forzar update().
    connect(m_backend, &PlaybackBackend::frameReady, this,
            &PlayerWidget::onFrameReady);

    // Vigila que la posición avance durante la reproducción: si se congela
    // > 5 s emite streamStalled para que el controlador re-resuelva.
    m_stallTimer = new QTimer(this);
    m_stallTimer->setInterval(1000);
    connect(m_stallTimer, &QTimer::timeout, this, &PlayerWidget::checkStall);
    m_stallTimer->start();

    // x86 (FfmpegBackend): si otra ventana cubre el vídeo, el motor deja de
    // convertir y de pintar mientras dure (ahorro real de CPU/GPU; Win7/DWM no
    // avisa de oclusión, así que lo detecta este timer lento con un recorrido
    // del z-order). En mpv no aplica (occlusionSkipSupported()==false): parar
    // el render ahí acumularía frames (ver AGENTS.md "Mini player").
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
    // Redibujado dirigido por eventos (AGENTS.md): el motor notifica cuando
    // hay un frame nuevo que pintar (update callback de mpv instalado en
    // MpvBackend::initGL / frameReady por frame del FfmpegBackend) y
    // onFrameReady() fuerza QWidget::update(). Sin poll de repintado: en
    // reposo (pausa, buffering, cubierto) no se repinta nada. Sustituye al
    // antiguo timer de 33 ms, redundante desde que existe la ruta de eventos.
}

void PlayerWidget::paintGL()
{
    if (!m_backend->hasRenderContext()) {
        NT_DEBUG("E2E paintGL no-rctx\n");
        return;
    }
    // x86 (FfmpegBackend): si otra ventana cubre por completo el vídeo, no
    // renderizar — el motor ya descartó los frames (evita subir texturas y
    // pintar debajo de la ventana; al destaparse se muestra el frame más
    // reciente, sin ráfaga). En mpv NO aplica (occlusionSkipSupported=false):
    // su render debe seguir para no acumular frames (ver AGENTS.md mini
    // player).
    if (m_backend->occlusionSkipSupported() && m_videoOccluded)
        return;
    QOpenGLFunctions *gl = QOpenGLContext::currentContext()->functions();
    GLint fbo = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    const int dpr = devicePixelRatio();
    // PROTEGIDO (AGENTS.md): render directo al FBO en paintGL (contexto GL
    // actual). El backend ejecuta mpv_render_context_render con estos mismos
    // parámetros de forma síncrona en este paint; el FBO y el contexto son los
    // del widget.
    m_backend->renderGL(fbo, width() * dpr, height() * dpr);
}

void PlayerWidget::playUrl(const QString &url, double startAt)
{
    if (url.isEmpty() || !m_backend->isValid())
        return;
    m_position = 0.0;
    m_duration = 0.0;
    // Si el backend consumió el audio externo en la carga (audio-files), la
    // URL pendiente ya no hace falta; si no, se conserva para el respaldo de
    // attachAudio en fileLoaded.
    if (m_backend->playUrl(url, startAt, m_pendingAudioFile)
        && !m_pendingAudioFile.isEmpty())
        m_pendingAudioFile.clear();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    m_lastProgressMs = nowMs;
    m_videoLoadMs = nowMs;
    m_cacheOk = false;
    emit positionChanged();
    emit durationChanged();
}

void PlayerWidget::playYtdl(const QString &watchUrl, int maxHeight)
{
    if (watchUrl.isEmpty())
        return;
    const QString fmt = QStringLiteral("bestvideo[height<=%1]+bestaudio/best[height<=%1]")
        .arg(maxHeight);
    m_backend->setYtdlFormat(fmt);
    playUrl(watchUrl);
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
}

void PlayerWidget::seek(double seconds)
{
    if (!m_backend->isValid())
        return;
    // Gracia para el nuevo seek: la posición no avanza hasta que llega el
    // nuevo fragmento; no debe contar como atasco.
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
    // Flujo clásico: nada que añadir; además limpia la pista externa
    // de un vídeo anterior con audio-file merodeando en el motor.
    if (url.isEmpty())
        m_backend->dropExternalAudio();
    // El audio externo se adjunta EN el próximo playUrl (loadfile con la opción
    // per-file "audio-files"), no con un audio-add suelto aquí: añadirlo antes
    // del loadfile engancharía la pista al stream ANTERIOR aún cargado y en el
    // restablecimiento (re-resolver) podía acabar mudo. El respaldo de
    // attachAudio en onFileLoaded() sigue para cuando playUrl no consumió la
    // URL.
}

void PlayerWidget::setAudioLanguage(const QString &lang)
{
    m_backend->setAudioLanguage(lang);
}

void PlayerWidget::setInitialBufferSecs(double secs)
{
    m_initialBufferSecs = secs;
    // NO destruir un timer de buffer ACTIVO: el sondeo de perfil puede ajustar
    // la espera mientras el primer vídeo ya arranca, y destruir el timer dejaría
    // el vídeo pausado (setPaused(true)) sin nadie que lo reanude. Si está
    // activo se deja terminar; la nueva espera aplica al siguiente arranque.
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
    // Respaldo de audio externo: si playUrl no lo consumió (mpv viejo sin la
    // opción "audio-files"), se adjunta ahora que el archivo principal cargó
    // ("no file loaded" ya no aplica).
    if (!m_pendingAudioFile.isEmpty() && m_backend->isValid())
        m_backend->attachAudio(m_pendingAudioFile);
    // También es el primer momento en que track-list tiene las pistas reales.
    // Lo consultamos una vez para que WatchPage filtre el menú de
    // resoluciones: si el vídeo solo llega a 720p, no se ofrecen 1080p+ (que
    // el motor no podría seleccionar y quedarían como "clicks inertes").
    m_videoMaxH = m_backend->probeMaxVideoHeight();
    NT_DEBUG("E2E qualityLimits max=%d\n", m_videoMaxH);
    emit qualityLimitsChanged(m_videoMaxH);
}

void PlayerWidget::onStreamStart()
{
    // Cada stream nuevo vuelve a dar oportunidad de reportar códec/hwdec
    // (para la red de seguridad del perfil de códec).
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
        // Parada intencional (stop()/loadfile de reemplazo). No es fin de
        // reproducción: NO debe auto-avanzar. Se resetea m_state para que
        // el próximo START_FILE reemita "playing" al controlador.
        m_state.clear();
        break;
    case PlaybackBackend::EndReason::Error:
        if (m_state != QLatin1String("error")) {
            m_state = QStringLiteral("error");
            emit stateChanged();
        }
        emit mediaError(errorText);
        return;
    default: {
        // EOF. Distinguir final natural (position ~ duration) de una
        // interrupción de red a mitad del vídeo.
        const bool naturalEnd =
            m_duration > 0.0 && m_position >= m_duration - 3.0;
        if (!naturalEnd) {
            if (m_duration > 0.0) {
                // EOF prematuro con duración conocida: YouTube gateó la
                // sesión (403 en audio y/o segmentos) y el demuxer se
                // quedó sin datos. El controlador re-resuelve con URLs
                // frescas y retoma en la posición (streamInterrupted).
                // Sin duración fiable (live) se mantiene el error clásico.
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
                QStringLiteral("La reproducción se interrumpió (red)."));
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
        // El log del motor manda: si ya dijo "Using software decoding", el
        // perfil es software aunque hwdec-current reporte un backend
        // (dispositivos de software en GPUs antiguas). Si no habló,
        // hwdec-current llega "vacío" hasta que el motor activa el
        // decodificador, y "no" cuando es software; un backend real confirma
        // HW. Si viene vacío, reintentar con un QTimer.
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
    // Espera inicial de buffer en alta resolución (>=1080p): aquí ya
    // conocemos la altura real del stream (en FILE_LOADED aún es 0).
    // Arrancar con unos segundos de contenido cacheado elimina las
    // micro-pausas de los primeros segundos (cierre del fast-start de
    // YouTube tras el ramp de http_multiple).
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
    // Se sigue para saber si el usuario pausó: en pausa la posición no
    // avanza y no debe interpretarse como atasco.
    if (!paused && m_paused)
        m_lastProgressMs = QDateTime::currentMSecsSinceEpoch();
    m_paused = paused;
}

void PlayerWidget::onCacheTime(double seconds)
{
    if (seconds > 0.2)
        m_cacheOk = true; // hay datos: la pista de vídeo sí carga
}

void PlayerWidget::onLogMessage(const QString &prefix, const QString &text)
{
    Q_UNUSED(prefix);
    if (text.contains(QLatin1String("Using software decoding"))) {
        m_hwdecLogSoftware = true;
        // Corrección tardía: el veredicto HW pudo emitirse antes de que
        // llegara esta línea (hwdec-current reportaba un backend falso).
        // Reemitir "no" para que la red de seguridad degrade el perfil.
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
    // El log del motor ya dictó software: no hay backend que esperar.
    if (m_hwdecLogSoftware) {
        if (m_hwdecRetry)
            m_hwdecRetry->stop();
        emit streamDecodeInfoReady(videoCodec(), QStringLiteral("no"));
        return;
    }
    // hwdec-current quedó vacío en el primer video-params: reintentar hasta
    // que el motor lo resuelva (o un tope de seguridad de ~6 s).
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

// x86: drop de FPS al abrir una ventana flotante encima del vídeo en Win7. El
// DWM no le avisa a la app de que la tapan (sigue recibiendo paint events y
// pintando frames que nadie ve, quemando la GMA 950 justo cuando el compositor
// compite — por eso el drop con CPU/RAM de la app bajos: el cuello de botella
// es la presentación, no el cómputo). Este recorrido del z-order
// (GetWindow hasta nuestra top-level) detecta si una ventana visible, no
// transparente y distinta de la nuestra cubre el rect del vídeo por completo.
#ifdef Q_OS_WIN
bool PlayerWidget::occludedByTopWindow()
{
    QWidget *top = window();
    if (!top)
        return false;
    const HWND topHwnd = static_cast<HWND>(top->winId());
    const HWND vidHwnd = static_cast<HWND>(winId());
    if (!topHwnd || !vidHwnd)
        return false;
    RECT vr;
    if (!GetWindowRect(vidHwnd, &vr))
        return false;
    // La cadena de GetWindow solo lista top-level windows (los child widgets,
    // como este QOpenGLWidget, no aparecen). Las ventanas por ENCIMA de la
    // nuestra en el z-order son las que llegan antes de topHwnd.
    HWND h = GetWindow(GW_HWNDFIRST);
    for (; h && h != topHwnd; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsWindowVisible(h) || IsIconic(h))
            continue;
        // Ventanas transparentes/layered (tooltips, decorativas): no cubren
        // contenido real.
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
    // Tope de espera razonable: no dejar al usuario mirando un spinner eterno
    // si la red no da para llenar el objetivo. 25 s de espera máxima.
    if (m_bufferStart.elapsed() > 25000) {
        NT_DEBUG("E2E buffer wait timeout (secs=%.1f)\n", m_initialBufferSecs);
        setPaused(false);
        m_bufferTimer->stop();
        return;
    }
    const double cacheDur = m_backend->cacheDuration();
    // cache-idle: el download ya terminó (todo lo descargable lo está): se
    // puede empezar ya aunque cacheDur sea bajo (desde URL local, p.ej.).
    const bool cacheIdle = m_backend->cacheIdle();
    if (m_initialBufferSecs <= 0.0) {
        setPaused(false);
        m_bufferTimer->stop();
        return;
    }
    // Más resolución = más margen necesario: 1080p pide `secs`, las alturas
    // superiores escalan proporcionalmente (p.ej. 2160p pide el doble).
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
        return; // espera inicial de buffer en curso: no es un atasco
    if (m_duration <= 0.0 || m_position >= m_duration - 2.0)
        return; // sin duración fiable o casi en el final (EOF inminente)
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // (a) La pista de vídeo no carga: el demuxer no cachea nada. Como el audio
    // va por archivo externo, la posición avanza igual y la vigilancia por
    // posición no lo ve; el usuario ve congelado el último frame anterior.
    if (!m_cacheOk && m_videoLoadMs > 0 && now - m_videoLoadMs > 8000) {
        m_videoLoadMs = now; // no disparar en bucle mientras el controlador recupera
        NT_DEBUG("E2E streamStalled sin-video pos=%.2f dur=%.2f\n", m_position,
                 m_duration);
        emit streamStalled();
        return;
    }
    // (a.2) Ninguna pista de audio activa tras ~10 s de reproducción: YouTube
    // rechazó el audio (403 de una sesión gateada) y el vídeo va mudo. No
    // depende del modo (audio externo o muxed): con HLS muxed el gate también
    // deja al motor sin audio (audio=eof) y antes solo el audio EXTERNO se
    // vigilaba aquí, así que el HLS agotaba ~60 s mudos por intento hasta el
    // EOF prematuro. Re-resolver temprano (con URLs frescas) en vez de esperar
    // un minuto; con duración >0 (todas arriba) los streams sin audio real
    // (silencio) no existen en la práctica.
    if (!m_backend->hasActiveAudio() && now - m_videoLoadMs > kAudioDetectMs
        && m_position > 2.0) {
        m_videoLoadMs = now; // no disparar en bucle mientras se recupera
        NT_DEBUG("E2E streamInterrupted sin-audio pos=%.2f dur=%.2f\n",
                 m_position, m_duration);
        emit streamInterrupted();
        return;
    }
    // (b) Posición congelada reproduciendo.
    if (m_lastProgressMs <= 0 || now - m_lastProgressMs < 5000)
        return;
    // Refresca para no disparar en bucle mientras el controlador re-resuelve.
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