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
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "message-level", "all=warn,vo=info,vd=info");
    mpv_set_option_string(m_mpv, "cache", "yes");
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "64MiB");
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
    const QByteArray u = url.toUtf8();
    const QByteArray opt = startAt > 0.0
        ? QByteArray("+") + QByteArray::number(startAt, 'f', 2)
        : QByteArray();
    mpv_set_option_string(m_mpv, "start", opt.constData());
    if (startAt > 0.0)
        NT_DEBUG("E2E playUrl start=%s\n", opt.constData());
    const char *cmd[] = { "loadfile", u.constData(), nullptr };
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
    case MPV_EVENT_PLAYBACK_RESTART:
    case MPV_EVENT_START_FILE:
        if (m_state != QLatin1String("playing")) {
            m_state = QStringLiteral("playing");
            emit stateChanged();
        }
        break;
    case MPV_EVENT_END_FILE: {
        mpv_event_end_file *end = static_cast<mpv_event_end_file *>(event->data);
        QString s;
        switch (end->reason) {
        case MPV_END_FILE_REASON_QUIT:
            return;
        case MPV_END_FILE_REASON_ERROR:
            s = QStringLiteral("error");
            break;
        default:
            s = QStringLiteral("ended");
            break;
        }
        if (m_state != s) {
            m_state = s;
            emit stateChanged();
        }
        if (end->reason == MPV_END_FILE_REASON_ERROR)
            emit mediaError(QString::fromUtf8(mpv_error_string(end->error)));
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
        } else if (std::strcmp(prop->name, "pause") == 0) {
            // Consume el cambio para no dejar eventos pendientes.
        } else if (std::strcmp(prop->name, "volume") == 0) {
            // Consume el cambio para no dejar eventos pendientes.
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