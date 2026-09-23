#include "playerwidget.h"

#include <QDateTime>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QTimer>

#include <clocale>
#include <cstdlib>
#include <cstring>

#include <mpv/render_gl.h>

#include "debug.h"

static void *getProcAddress(void *ctx, const char *name)
{
    Q_UNUSED(ctx);
    QOpenGLContext *glc = QOpenGLContext::currentContext();
    return glc ? reinterpret_cast<void *>(glc->getProcAddress(name)) : nullptr;
}

// Construye un mpv_node de tipo string con el valor indicado. El buffer lo
// asigna el llamador y lo libera con mpv_nodeFreeStr().
static void mpvNodeSetStr(mpv_node *n, const QByteArray &s)
{
    n->format = MPV_FORMAT_STRING;
    char *buf = static_cast<char *>(malloc(size_t(s.size()) + 1));
    std::memcpy(buf, s.constData(), size_t(s.size()));
    buf[s.size()] = '\0';
    n->u.string = buf;
}

static void mpvNodeFreeStr(mpv_node *n)
{
    if (n->format == MPV_FORMAT_STRING) {
        free(n->u.string);
        n->u.string = nullptr;
    }
    n->format = MPV_FORMAT_NONE;
}

// loadfile con la opción per-file "audio-files": el vídeo y el audio externo se
// abren y se sincronizan en la misma carga. Sin esto mpv arranca solo con el
// vídeo y el audio entra después (vídeo mudo inicial + dropfps del re-sync; y
// en re-resoluciones el audio puede no llegar). Devuelve el código del comando.
int PlayerWidget::loadFileWithAudio(const QByteArray &videoUrl)
{
    if (videoUrl.isEmpty() || m_pendingAudioFile.isEmpty())
        return MPV_ERROR_INVALID_PARAMETER;

    mpv_node args{};
    mpv_node options{};
    mpv_node_list argList{};
    mpv_node_list optList{};
    mpv_node argv[5]{};
    mpv_node optv[1]{};

    mpvNodeSetStr(&argv[0], QByteArrayLiteral("loadfile"));
    mpvNodeSetStr(&argv[1], videoUrl);
    mpvNodeSetStr(&argv[2], QByteArrayLiteral("replace"));

    optList.num = 1;
    optList.keys = static_cast<char **>(calloc(1, sizeof(char *)));
    {
        const QByteArray k(QByteArrayLiteral("audio-files"));
        char *kbuf = static_cast<char *>(malloc(size_t(k.size()) + 1));
        std::memcpy(kbuf, k.constData(), size_t(k.size()));
        kbuf[k.size()] = '\0';
        optList.keys[0] = kbuf;
    }
    optList.values = optv;
    options.format = MPV_FORMAT_NODE_MAP;
    options.u.list = &optList;
    // mpv interpreta audio-files como una lista de strings separada por ':';
    // un URL https://... se parte en trozos (https, //host, etc.). Escapamos
    // cada ':' con '\:' para que el valor llegue íntegro como una sola entrada.
    QByteArray audioEsc = m_pendingAudioFile.toUtf8();
    audioEsc.replace(':', "\\:");
    mpvNodeSetStr(&optv[0], audioEsc);

    // Firma moderna (mpv >= 0.38): loadfile url replace index=<int> options.
    // Firma vieja (mpv 0.33-0.37, p. ej. bundle Windows): sin el argumento
    // "index", las opciones van en cuarta posición. Se prueba la moderna y, si
    // el mpv no la entiende (INVALID_PARAMETER), se reintenta con la vieja.
    int r = MPV_ERROR_INVALID_PARAMETER;
    for (int pass = 0; pass < 2 && r == MPV_ERROR_INVALID_PARAMETER; ++pass) {
        if (pass == 0) {
            argList.num = 5;
            argList.values = argv;
            argv[3].format = MPV_FORMAT_INT64; // índice (ignorado con replace)
            argv[3].u.int64 = -1;
            argv[4] = options;
        } else {
            argList.num = 4;
            argList.values = argv;
            argv[3] = options;
        }
        args.format = MPV_FORMAT_NODE_ARRAY;
        args.u.list = &argList;
        mpv_node result{};
        r = mpv_command_node(m_mpv, &args, &result);
        if (r >= 0)
            mpv_free_node_contents(&result);
    }

    mpvNodeFreeStr(&argv[0]);
    mpvNodeFreeStr(&argv[1]);
    mpvNodeFreeStr(&argv[2]);
    mpvNodeFreeStr(&optv[0]);
    free(optList.keys);
    NT_DEBUG("E2E loadfile+audio-files res=%d len=%d\n", r,
             int(m_pendingAudioFile.size()));
    return r;
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
    // vd=v: el veredicto real del decodificador ("Using hardware/software
    // decoding") es verbose y es lo que valida la red de seguridad del perfil.
    mpv_set_option_string(m_mpv, "message-level",
                          "all=warn,vo=info,vd=v,cache=info,stream=info,network=info");
    mpv_set_option_string(m_mpv, "cache", "yes");
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "64MiB");
    // Amortiguación al arranque: sin esto, con el throttle de YouTube la primera
    // decena de segundos reproduce "en seco" y mpv hace micro-pausas a los ~6 y
    // ~10 s hasta que la descarga paralela (http_multiple) se estabiliza.
    mpv_set_option_string(m_mpv, "cache-initial", "3MiB");
    mpv_set_option_string(m_mpv, "demuxer-readahead-secs", "20");
    mpv_set_option_string(m_mpv, "vd-lavc-threads", "2");
    mpv_set_option_string(m_mpv, "audio-buffer", "2.0");
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
    mpv_observe_property(m_mpv, 0, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 0, "demuxer-cache-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "network-bytes-per-second", MPV_FORMAT_INT64);
    mpv_request_log_messages(m_mpv, "v");
    mpv_set_wakeup_callback(m_mpv, PlayerWidget::mpvWakeup, this);

    // Vigila que la posición avance durante la reproducción: si se congela
    // > 5 s emite streamStalled para que el controlador re-resuelva.
    m_stallTimer = new QTimer(this);
    m_stallTimer->setInterval(1000);
    connect(m_stallTimer, &QTimer::timeout, this, &PlayerWidget::checkStall);
    m_stallTimer->start();
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
    // El stream actual espera audio externo (audio-files aparte): con este
    // flag checkStall detecta un audio rechazado (403) aunque la posición
    // avance, y dispara la re-resolución temprana.
    m_expectsExternalAudio = !m_pendingAudioFile.isEmpty();
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
    m_paused = false;
    NT_DEBUG("E2E playUrl len=%d http=%d audio=%d\n", int(u.size()),
             url.startsWith(QLatin1String("http")) ? 1 : 0,
             int(!m_pendingAudioFile.isEmpty()));
    // Con audio externo (edge/AV1-HLS) el vídeo y el audio se abren en el MISMO
    // loadfile (opción per-file "audio-files"): mpv los sincroniza desde el
    // arranque, sin el vídeo mudo inicial y el dropfps del re-sync posterior.
    // Si el comando falla (mpv viejo sin la opción), se conserva el loadfile
    // simple y FILE_LOADED hará el audio-add de antes.
    if (!m_pendingAudioFile.isEmpty()) {
        const int r = loadFileWithAudio(u);
        if (r >= 0) {
            m_pendingAudioFile.clear();
        } else {
            NT_DEBUG("E2E loadfile+audio-files fallo (%s); fallback audio-add\n",
                     mpv_error_string(r));
            mpv_command_async(m_mpv, 0, cmd);
        }
    } else {
        mpv_command_async(m_mpv, 0, cmd);
    }
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
    mpv_set_option_string(m_mpv, "ytdl", "yes");
    mpv_set_option_string(m_mpv, "ytdl-format", fmt.toUtf8().constData());
    playUrl(watchUrl);
}

void PlayerWidget::stop()
{
    m_expectsExternalAudio = false;
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
    if (!paused)
        m_lastProgressMs = QDateTime::currentMSecsSinceEpoch();
    m_paused = paused;
}

void PlayerWidget::seek(double seconds)
{
    if (!m_mpv)
        return;
    // Gracia para el nuevo seek: la posición no avanza hasta que llega el
    // nuevo fragmento; no debe contar como atasco.
    m_lastProgressMs = QDateTime::currentMSecsSinceEpoch();
    mpv_set_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &seconds);
}

void PlayerWidget::setVolume(double volume)
{
    if (!m_mpv)
        return;
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &volume);
}

void PlayerWidget::setVolumeMax(double maxPercent)
{
    if (!m_mpv)
        return;
    const QByteArray v = QByteArray::number(maxPercent, 'f', 1);
    mpv_set_option_string(m_mpv, "volume-max", v.constData());
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
    // Flujo clásico: nada que añadir; además limpia la pista externa
    // de un vídeo anterior con audio-file merodeando en mpv.
    if (url.isEmpty()) {
        const char *drop[] = { "audio-file-remove", "auto", nullptr };
        if (mpv_command(m_mpv, drop) < 0 && !m_audioAdded) {
            // Sin pista externa previa no hay nada que quitar.
        }
        m_audioAdded = false;
        return;
    }
    // El audio externo se adjunta EN el próximo playUrl (loadfile con la opción
    // per-file "audio-files"), no con un audio-add suelto aquí: añadirlo antes
    // del loadfile engancharía la pista al stream ANTERIOR aún cargado y en el
    // restablecimiento (re-resolver) podía acabar mudo. addAudioFileNow() sigue
    // como respaldo si playUrl no consumió la URL.
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

void PlayerWidget::retryHwdec()
{
    // El log de mpv ya dictó software: no hay backend que esperar.
    if (m_hwdecLogSoftware) {
        if (m_hwdecRetry)
            m_hwdecRetry->stop();
        emit streamDecodeInfoReady(videoCodec(), QStringLiteral("no"));
        return;
    }
    // hwdec-current quedó vacío en el primer video-params: reintentar hasta
    // que mpv lo resuelva (o un tope de seguridad de ~6 s).
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

void PlayerWidget::checkStall()
{
    if (!m_mpv || m_state != QLatin1String("playing"))
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
    // (a.2) Audio externo esperado pero ninguna pista de audio activa: YouTube
    // rechazó el audio-file (403 de una sesión gateada) y el vídeo va mudo
    // hasta que los segmentos del HLS también mueran. Re-resolver temprano
    // (con URLs frescas) en vez de esperar al EOF prematuro de los 60 s.
    if (m_expectsExternalAudio && !hasActiveAudio()
        && now - m_videoLoadMs > 4000 && m_position > 2.0) {
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

// Lee mpv "track-list" y devuelve la altura máxima entre las pistas de
// vídeo. Devuelve 0 si el archivo aún no cargó (FILE_LOADED no llegó) o si
// la consulta falló. Llamado justo después de FILE_LOADED para que
// WatchPage oculte del menú de resoluciones las opciones por encima del
// techo real (evita ofrecer 1080p en un clip de 720p). mpv devuelve
// track-list como nodo ARRAY de MAPs; cada mapa trae "type"=="video",
// "codec" (p.ej. h264/vp9/av1) y dimensiones. La altura "real" de cada
// pista está en "demux-h" (no en "video-params/h", que es de la pista
// actualmente seleccionada). Se ignora "mjpeg" (miniaturas/portadas).
int PlayerWidget::probeMaxVideoHeight() const
{
    if (!m_mpv)
        return 0;
    mpv_node node{};
    if (mpv_get_property(m_mpv, "track-list", MPV_FORMAT_NODE, &node) < 0)
        return 0;
    int maxH = 0;
    if (node.format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < node.u.list->num; ++i) {
            mpv_node *entry = &node.u.list->values[i];
            if (!entry || entry->format != MPV_FORMAT_NODE_MAP)
                continue;
            QString type, codec;
            int64_t h = 0;
            for (int j = 0; j < entry->u.list->num; ++j) {
                const QString k = QString::fromUtf8(
                    entry->u.list->keys[j]);
                mpv_node *v = &entry->u.list->values[j];
                if (!v)
                    continue;
                if (k == QLatin1String("type")
                        && v->format == MPV_FORMAT_STRING)
                    type = QString::fromUtf8(v->u.string);
                else if (k == QLatin1String("codec")
                        && v->format == MPV_FORMAT_STRING)
                    codec = QString::fromUtf8(v->u.string);
                else if (k == QLatin1String("demux-h")
                        && v->format == MPV_FORMAT_INT64)
                    h = v->u.int64;
            }
            if (type == QLatin1String("video")
                    && codec != QLatin1String("mjpeg")
                    && h > 0
                    && maxH < int(h))
                maxH = int(h);
        }
    }
    mpv_free_node_contents(&node);
    return maxH;
}

// true si mpv tiene una pista de audio activa ("audio-params"). Con el audio
// externo (audio-files) rechazado por YouTube (403), mpv reproduce el vídeo
// sin ninguna pista de audio; "audio-params" no existe / queda vacío.
bool PlayerWidget::hasActiveAudio() const
{
    if (!m_mpv)
        return false;
    mpv_node node{};
    if (mpv_get_property(m_mpv, "audio-params", MPV_FORMAT_NODE, &node) < 0)
        return false;
    const bool ok = node.format == MPV_FORMAT_NODE_MAP;
    mpv_free_node_contents(&node);
    return ok;
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
        // También es el primer momento en que track-list tiene las pistas
        // reales. Lo consultamos una vez para que WatchPage filtre el menú
        // de resoluciones: si el vídeo solo llega a 720p, no se ofrecen
        // 1080p+ (que mpv no podría seleccionar y quedarían como "clicks
        // inertes" para el usuario).
        m_videoMaxH = probeMaxVideoHeight();
        NT_DEBUG("E2E qualityLimits max=%d\n", m_videoMaxH);
        emit qualityLimitsChanged(m_videoMaxH);
        break;
    case MPV_EVENT_PLAYBACK_RESTART:
    case MPV_EVENT_START_FILE:
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
        break;
    }
    case MPV_EVENT_PROPERTY_CHANGE: {
        mpv_event_property *prop = static_cast<mpv_event_property *>(event->data);
        if (std::strcmp(prop->name, "time-pos") == 0 && prop->data) {
            double v = *static_cast<double *>(prop->data);
            if (std::abs(v - m_position) > 0.05) {
                m_position = v;
                m_lastProgressMs = QDateTime::currentMSecsSinceEpoch();
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
                // El log de mpv manda: si ya dijo "Using software decoding", el
                // perfil es software aunque hwdec-current reporte un backend
                // (dispositivos de software en GPUs antiguas). Si no habló,
                // hwdec-current llega "vacío" hasta que mpv activa el
                // decodificador, y "no" cuando es software; un backend real
                // confirma HW. Si viene vacío, reintentar con un QTimer.
                if (m_hwdecLogSoftware) {
                    emit streamDecodeInfoReady(videoCodec(),
                                               QStringLiteral("no"));
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
        } else if (std::strcmp(prop->name, "pause") == 0 && prop->data) {
            // Se sigue para saber si el usuario pausó: en pausa la posición no
            // avanza y no debe interpretarse como atasco.
            const bool paused = (*static_cast<int *>(prop->data) != 0);
            if (!paused && m_paused)
                m_lastProgressMs = QDateTime::currentMSecsSinceEpoch();
            m_paused = paused;
        } else if (std::strcmp(prop->name, "volume") == 0) {
            // Consume el cambio para no dejar eventos pendientes.
        } else if (std::strcmp(prop->name, "paused-for-cache") == 0 && prop->data) {
            const int v = *static_cast<int *>(prop->data);
            NT_DEBUG("E2E mpv paused-for-cache=%d\n", v);
        } else if (std::strcmp(prop->name, "demuxer-cache-time") == 0 && prop->data) {
            const double v = *static_cast<double *>(prop->data);
            if (v > 0.2)
                m_cacheOk = true; // hay datos: la pista de vídeo sí carga
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
        if (log && log->text) {
            const QString text = QString::fromUtf8(log->text);
            if (text.contains(QLatin1String("Using software decoding"))) {
                m_hwdecLogSoftware = true;
                // Corrección tardía: el veredicto HW pudo emitirse antes de que
                // llegara esta línea (hwdec-current reportaba un backend falso).
                // Reemitir "no" para que la red de seguridad degrade el perfil.
                if (m_hwdecProbed) {
                    if (m_hwdecRetry)
                        m_hwdecRetry->stop();
                    emit streamDecodeInfoReady(videoCodec(),
                                               QStringLiteral("no"));
                }
            } else if (text.contains(
                           QLatin1String("Using hardware decoding"))) {
                m_hwdecLogSoftware = false;
            }
        }
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