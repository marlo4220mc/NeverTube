#include "playerwidget.h"

#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QTimer>

#include <clocale>
#include <cstring>

#include <mpv/render_gl.h>

#include "debug.h"

static void *getProcAddress(void *ctx, const char *name)
{
    Q_UNUSED(ctx);
    QOpenGLContext *glc = QOpenGLContext::currentContext();
    return glc ? reinterpret_cast<void *>(glc->getProcAddress(name)) : nullptr;
}

PlayerWidget::PlayerWidget(QWidget *parent)
    : QOpenGLWidget(parent)
{
    std::setlocale(LC_NUMERIC, "C");

    m_mpv = mpv_create();
    if (!m_mpv)
        return;

    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "hwdec", "auto");
    mpv_set_option_string(m_mpv, "keep-open", "no");
    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "message-level",
                          "all=warn,vo=info,vd=info,cache=info,stream=info,network=info");
    mpv_set_option_string(m_mpv, "cache", "yes");
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "64MiB");
    // Amortiguación al arranque: sin esto, con el throttle de YouTube la primera
    // decena de segundos reproduce "en seco" y mpv hace micro-pausas a los ~6 y
    // ~10 s hasta que la descarga paralela (http_multiple) se estabiliza.
    mpv_set_option_string(m_mpv, "cache-initial", "3MiB");
    mpv_set_option_string(m_mpv, "demuxer-readahead-secs", "20");
    mpv_set_option_string(m_mpv, "vd-lavc-threads", "2");
    mpv_set_option_string(m_mpv, "audio-buffer", "0.5");
    mpv_set_option_string(m_mpv, "video-sync", "audio");

    if (mpv_initialize(m_mpv) < 0) {
        qWarning("player: mpv_initialize fallo");
        return;
    }

    char *hwdec = nullptr;
    if (mpv_get_property(m_mpv, "hwdec", MPV_FORMAT_STRING, &hwdec) >= 0 && hwdec) {
        m_hwdec = QString::fromUtf8(hwdec);
        mpv_free(hwdec);
    }

    mpv_observe_property(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "video-params/w", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 0, "video-params/h", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 0, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "demuxer-cache-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "network-bytes-per-second", MPV_FORMAT_INT64);
    mpv_request_log_messages(m_mpv, "info");
    mpv_set_wakeup_callback(m_mpv, PlayerWidget::mpvWakeup, this);
}

PlayerWidget::~PlayerWidget()
{
    makeCurrent();
    if (m_rctx) {
        mpv_render_context_set_update_callback(m_rctx, nullptr, nullptr);
        mpv_render_context_free(m_rctx);
        m_rctx = nullptr;
    }
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
    doneCurrent();
}

double PlayerWidget::speed() const
{
    double value = 1.0;
    if (m_mpv)
        mpv_get_property(m_mpv, "speed", MPV_FORMAT_DOUBLE, &value);
    return value;
}

QString PlayerWidget::hwdecCurrent() const
{
    if (!m_mpv)
        return QString();
    char *value = nullptr;
    if (mpv_get_property(m_mpv, "hwdec-current", MPV_FORMAT_STRING, &value) < 0)
        return QString();
    QString out = QString::fromUtf8(value ? value : "");
    mpv_free(value);
    return out;
}

QString PlayerWidget::videoCodec() const
{
    if (!m_mpv)
        return QString();
    char *value = nullptr;
    if (mpv_get_property(m_mpv, "video-codec", MPV_FORMAT_STRING, &value) < 0)
        return QString();
    QString out = QString::fromUtf8(value ? value : "");
    mpv_free(value);
    return out;
}

void PlayerWidget::initializeGL()
{
    NT_DEBUG("E2E initializeGL w=%d h=%d rctx=%p\n",
             width(), height(), (void *)m_rctx);
    mpv_opengl_init_params init{ getProcAddress, nullptr };
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL) },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &init },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    if (mpv_render_context_create(&m_rctx, m_mpv, params) < 0) {
        qWarning("player: fallo mpv_render_context_create");
        return;
    }
    mpv_render_context_set_update_callback(m_rctx, PlayerWidget::mpvUpdate, this);

    auto redrawTimer = new QTimer(this);
    redrawTimer->setInterval(33);
    connect(redrawTimer, &QTimer::timeout, this,
            qOverload<>(&QWidget::update));
    redrawTimer->start();
    m_redrawTimerActive = true;
}

void PlayerWidget::paintGL()
{
    if (!m_rctx) {
        NT_DEBUG("E2E paintGL no-rctx\n");
        return;
    }
    QOpenGLFunctions *gl = QOpenGLContext::currentContext()->functions();
    GLint fbo = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
    const int dpr = devicePixelRatio();
    mpv_opengl_fbo mpfbo{ fbo, width() * dpr, height() * dpr, 0 };
    int flipY = 1;
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo },
        { MPV_RENDER_PARAM_FLIP_Y, &flipY },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    const int res = mpv_render_context_render(m_rctx, params);
    if (res < 0)
        qWarning("player: render error: %s", mpv_error_string(res));
    else if ((++m_logFrames % 90) == 1)
        NT_DEBUG("E2E paintGL ok frames=%d w=%d h=%d res=%d\n",
                 m_logFrames, width(), height(), res);
}

void PlayerWidget::playUrl(const QString &url, double startAt)
{
    if (url.isEmpty() || !m_mpv)
        return;
    m_position = 0.0;
    m_duration = 0.0;
    // Preferencia de idioma de audio ("alang"): solo con preferencia explícita
    // mpv elige un doblaje (aunque tenga DEFAULT=NO) en lugar de la pista
    // marcada DEFAULT=YES por YouTube (normalmente el audio original).
    const QByteArray lang = m_audioLang.toUtf8();
    mpv_set_option_string(m_mpv, "alang", lang.constData());
    // YouTube estrangula la descarga continua de la URL directa a ~initcwndbps
    // (~1,4 Mbps), insuficiente para 1080p: pausas de buffering a los 6-10 s.
    // ffmpeg descarga por rangos en paralelo ("http_multiple") y los Range
    // cortos no están estrangulados (35 MB/s) -> acaba con el throttle.
    // Solo para URLs externas: el master HLS local no soporta Range requests.
    if (url.startsWith(QLatin1String("http://127.0.0.1"))
        || url.startsWith(QLatin1String("http://localhost")))
        mpv_set_option_string(m_mpv, "stream-lavf-o", "");
    else
        mpv_set_option_string(m_mpv, "stream-lavf-o", "http_multiple=1");
    const QByteArray u = url.toUtf8();
    // "start" SIEMPRE con valor concreto: mpv ignora/reserva el valor anterior si
    // se le pasa una cadena vacía, y un vídeo reanudado (start=X) dejaría ese X
    // como salida para TODOS los siguientes (cada vídeo de la cola arrancaría
    // desde la posición del primero). Valor absoluto, sin el prefijo "+".
    const QByteArray opt = QByteArray::number(startAt > 0.0 ? startAt : 0.0, 'f', 2);
    mpv_set_option_string(m_mpv, "start", opt.constData());
    if (startAt > 0.0)
        NT_DEBUG("E2E playUrl start=%s\n", opt.constData());
    const char *cmd[] = { "loadfile", u.constData(), nullptr };
    // Cada vídeo nuevo arranca reproduciendo: mpv conserva el estado de pausa
    // entre loadfile, y un vídeo anterior dejado en pausa dejaba el siguiente
    // en pausa ("vídeo que clickeo empieza pausado").
    int paused = 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &paused);
    NT_DEBUG("E2E playUrl len=%d http=%d\n", int(u.size()),
             url.startsWith(QLatin1String("http")) ? 1 : 0);
    mpv_command_async(m_mpv, 0, cmd);
    emit positionChanged();
    emit durationChanged();
}

void PlayerWidget::playYtdl(const QString &watchUrl, int maxHeight)
{
    if (watchUrl.isEmpty())
        return;
    const QString fmt = QStringLiteral("bestvideo[height<=%1]+bestaudio/best[height<=%1]")
        .arg(maxHeight);
    mpv_set_option_string(m_mpv, "ytdl", "yes");
    mpv_set_option_string(m_mpv, "ytdl-format", fmt.toUtf8().constData());
    playUrl(watchUrl);
}

void PlayerWidget::stop()
{
    if (!m_mpv)
        return;
    const char *cmd[] = { "stop", nullptr };
    mpv_command_async(m_mpv, 0, cmd);
}

void PlayerWidget::setPaused(bool paused)
{
    if (!m_mpv)
        return;
    int flag = paused ? 1 : 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void PlayerWidget::seek(double seconds)
{
    if (!m_mpv)
        return;
    mpv_set_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &seconds);
}

void PlayerWidget::setVolume(double volume)
{
    if (!m_mpv)
        return;
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &volume);
}

void PlayerWidget::setSubVisibility(bool visible)
{
    if (!m_mpv)
        return;
    int flag = visible ? 1 : 0;
    mpv_set_property(m_mpv, "sub-visibility", MPV_FORMAT_FLAG, &flag);
}

void PlayerWidget::setAudioFile(const QString &url)
{
    m_pendingAudioFile = url;
    if (!m_mpv)
        return;
    // Flujo clásico/yt-dlp: nada que añadir; además limpia la pista externa
    // de un vídeo anterior con audio-file merodeando en mpv.
    if (url.isEmpty()) {
        const char *drop[] = { "audio-file-remove", "auto", nullptr };
        if (mpv_command(m_mpv, drop) < 0 && !m_audioAdded) {
            // Sin pista externa previa no hay nada que quitar.
        }
        m_audioAdded = false;
        return;
    }
    // Si el archivo todavía no está cargado, el comando se aplicará en
    // MPV_EVENT_FILE_LOADED para garantizar que el owner está listo.
    if (m_state.isEmpty())
        return;
    addAudioFileNow();
}

void PlayerWidget::addAudioFileNow()
{
    if (m_pendingAudioFile.isEmpty() || !m_mpv)
        return;
    const QByteArray u = m_pendingAudioFile.toUtf8();
    // audio-add select: añade la pista y la selecciona como audio actual.
    const char *cmd[] = { "audio-add", u.constData(), "select", nullptr };
    const int r = mpv_command(m_mpv, cmd);
    m_audioAdded = r >= 0;
    // La pista vieja (oído de un vídeo anterior) que mpv conserve se descarta
    // si hubo reemplazo: cada loadfile solo debe tener su audio propio.
    NT_DEBUG("E2E audio-add res=%d len=%d\n", r, int(u.size()));
}

void PlayerWidget::setAudioLanguage(const QString &lang)
{
    m_audioLang = lang;
    if (!m_mpv)
        return;
    const QByteArray b = m_audioLang.toUtf8();
    mpv_set_option_string(m_mpv, "alang", b.constData());
}

void PlayerWidget::setInitialBufferSecs(double secs)
{
    m_initialBufferSecs = secs;
    if (m_bufferTimer) {
        m_bufferTimer->stop();
        m_bufferTimer->deleteLater();
        m_bufferTimer = nullptr;
    }
}

void PlayerWidget::checkInitialBuffer()
{
    if (!m_mpv || !m_bufferTimer) {
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
    double cacheDur = 0.0;
    mpv_get_property(m_mpv, "demuxer-cache-duration", MPV_FORMAT_DOUBLE,
                     &cacheDur);
    // cache-idle: el download ya terminó (todo lo descargable lo está): se
    // puede empezar ya aunque cacheDur sea bajo (desde URL local, p.ej.).
    int cacheIdle = 0;
    mpv_get_property(m_mpv, "demuxer-cache-idle", MPV_FORMAT_FLAG, &cacheIdle);
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
             target, cacheIdle);
    if (cacheDur >= target || cacheIdle) {
        NT_DEBUG("E2E buffer wait done (cache=%.1f target=%.1f)\n", cacheDur,
                 target);
        setPaused(false);
        m_bufferTimer->stop();
    }
}

void PlayerWidget::mpvWakeup(void *ctx)
{
    PlayerWidget *self = static_cast<PlayerWidget *>(ctx);
    QMetaObject::invokeMethod(self, "poll", Qt::QueuedConnection);
}

void PlayerWidget::mpvUpdate(void *ctx)
{
    PlayerWidget *self = static_cast<PlayerWidget *>(ctx);
    QMetaObject::invokeMethod(self, "onFrameReady", Qt::QueuedConnection);
}

void PlayerWidget::poll()
{
    if (!m_mpv)
        return;
    while (true) {
        mpv_event *event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        handleEvent(event);
    }
}

void PlayerWidget::handleEvent(mpv_event *event)
{
    switch (event->event_id) {
    case MPV_EVENT_FILE_LOADED:
        // El archivo principal terminó de cargar: ahora sí se puede adjuntar
        // el audio externo (audio-add) sin "no file loaded".
        addAudioFileNow();
        break;
    case MPV_EVENT_PLAYBACK_RESTART:
    case MPV_EVENT_START_FILE:
        // Cada stream nuevo vuelve a dar oportunidad de reportar códec/hwdec
        // (para la auto-corrección del perfil del master edge).
        m_hwdecProbed = false;
        if (m_state != QLatin1String("playing")) {
            m_state = QStringLiteral("playing");
            emit stateChanged();
        }
        break;
    case MPV_EVENT_END_FILE: {
        mpv_event_end_file *end = static_cast<mpv_event_end_file *>(event->data);
        switch (end->reason) {
        case MPV_END_FILE_REASON_QUIT:
            return;
        case MPV_END_FILE_REASON_STOP:
            // Parada intencional (stop()/loadfile de reemplazo). No es fin de
            // reproducción: NO debe auto-avanzar. Se resetea m_state para que
            // el próximo START_FILE reemita "playing" al controlador.
            m_state.clear();
            break;
        case MPV_END_FILE_REASON_ERROR:
            if (m_state != QLatin1String("error")) {
                m_state = QStringLiteral("error");
                emit stateChanged();
            }
            emit mediaError(QString::fromUtf8(mpv_error_string(end->error)));
            return;
        default: {
            // EOF. Distinguir final natural (position ~ duration) de una
            // interrupción de red a mitad del vídeo.
            const bool naturalEnd =
                m_duration > 0.0 && m_position >= m_duration - 3.0;
            if (!naturalEnd) {
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
        break;
    }
    case MPV_EVENT_PROPERTY_CHANGE: {
        mpv_event_property *prop = static_cast<mpv_event_property *>(event->data);
        if (std::strcmp(prop->name, "time-pos") == 0 && prop->data) {
            double v = *static_cast<double *>(prop->data);
            if (std::abs(v - m_position) > 0.05) {
                m_position = v;
                emit positionChanged();
            }
        } else if (std::strcmp(prop->name, "duration") == 0 && prop->data) {
            m_duration = *static_cast<double *>(prop->data);
            emit durationChanged();
        } else if (std::strcmp(prop->name, "video-params/w") == 0 && prop->data) {
            m_videoW = *static_cast<int64_t *>(prop->data);
            emitAspectIfReady();
        } else if (std::strcmp(prop->name, "video-params/h") == 0 && prop->data) {
            m_videoH = *static_cast<int64_t *>(prop->data);
            if (m_videoW > 0)
NT_DEBUG("E2E mpv video-params %lldx%lld\n",
                     static_cast<long long>(m_videoW),
                     static_cast<long long>(m_videoH));
            emitAspectIfReady();
            if (m_videoW > 0 && m_videoH > 0)
                emit videoSizeChanged(int(m_videoW), int(m_videoH));
            if (!m_hwdecProbed && m_videoW > 0) {
                m_hwdecProbed = true;
                emit streamDecodeInfoReady(videoCodec(), hwdecCurrent());
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
        } else if (std::strcmp(prop->name, "pause") == 0) {
            // Consume el cambio para no dejar eventos pendientes.
        } else if (std::strcmp(prop->name, "volume") == 0) {
            // Consume el cambio para no dejar eventos pendientes.
        } else if (std::strcmp(prop->name, "paused-for-cache") == 0 && prop->data) {
            const int v = *static_cast<int *>(prop->data);
            NT_DEBUG("E2E mpv paused-for-cache=%d\n", v);
        } else if (std::strcmp(prop->name, "demuxer-cache-time") == 0 && prop->data) {
            const double v = *static_cast<double *>(prop->data);
            NT_DEBUG("E2E mpv cache-time=%.2f\n", v);
        } else if (std::strcmp(prop->name, "network-bytes-per-second") == 0 && prop->data) {
            const qint64 v = *static_cast<qint64 *>(prop->data);
            NT_DEBUG("E2E mpv bps=%lld\n", static_cast<long long>(v));
        }
        break;
    }
    case MPV_EVENT_LOG_MESSAGE: {
        mpv_event_log_message *log =
            static_cast<mpv_event_log_message *>(event->data);
        NT_DEBUG("[mpv:%s] %s", log->prefix, log->text);
        break;
    }
    default:
        break;
    }
}

void PlayerWidget::onFrameReady()
{
    poll();
    update();
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