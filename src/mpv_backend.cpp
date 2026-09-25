#include "mpv_backend.h"

#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QString>
#include <QThread>
#include <QtGlobal>

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
// asigna el llamador y lo libera con mpvNodeFreeStr().
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
static int loadFileWithAudio(mpv_handle *mpv, const QByteArray &videoUrl,
                             const QString &audioUrl)
{
    if (videoUrl.isEmpty() || audioUrl.isEmpty())
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
    QByteArray audioEsc = audioUrl.toUtf8();
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
        r = mpv_command_node(mpv, &args, &result);
        if (r >= 0)
            mpv_free_node_contents(&result);
    }

    mpvNodeFreeStr(&argv[0]);
    mpvNodeFreeStr(&argv[1]);
    mpvNodeFreeStr(&argv[2]);
    mpvNodeFreeStr(&optv[0]);
    free(optList.keys);
    NT_DEBUG("E2E loadfile+audio-files res=%d len=%d\n", r, int(audioUrl.size()));
    return r;
}

MpvBackend::MpvBackend(QObject *parent)
    : PlaybackBackend(parent)
{
    std::setlocale(LC_NUMERIC, "C");

    m_mpv = mpv_create();
    if (!m_mpv)
        return;

    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "hwdec", "auto");
    // vd=v: el veredicto real del decodificador ("Using hardware/software
    // decoding") es verbose y es lo que valida la red de seguridad del perfil.
    mpv_set_option_string(m_mpv, "message-level",
                          "all=warn,vo=info,vd=v,cache=info,stream=info,network=info");
    mpv_set_option_string(m_mpv, "cache", "yes");
    // Colchón de demux (estilo Chrome, versión contenida): 96 MiB para absorber
    // los picos del decodificador software y el throttle de descarga de YouTube
    // sin disparar la RAM (150 MiB era el default de mpv, demasiado para una
    // máquina débil/x64; 64 MiB original quedaba corto en las
    // micro-pausas de arranque). Con el cache activo este tope es además el
    // límite REAL del readahead: cache-secs default es "muy alto" y
    // demuxer-readahead-secs queda "mostly ignored" (manual de mpv), así que
    // no hace falta fijar un readahead en segundos.
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "96MiB");
    // Amortiguación al arranque: sin esto, con el throttle de YouTube la primera
    // decena de segundos reproduce "en seco" y mpv hace micro-pausas a los ~6 y
    // ~10 s hasta que la descarga paralela (http_multiple) se estabiliza.
    mpv_set_option_string(m_mpv, "cache-initial", "3MiB");
    // Hilos del decodificador SOFTWARE (vd=lavc -> h264/vp9/av1 paralelizan por
    // frame-threading). Ideal = nº de núcleos con tope 4: subir más en CPUs de
    // 2 núcleos es sobresuscripción de hilos. Con hwdec activo no interviene
    // (decodifica el HW, no lavc).
    const int cores = qMax(1, QThread::idealThreadCount());
    const QByteArray lavcThreads = QByteArray::number(qMin(cores, 4));
    mpv_set_option_string(m_mpv, "vd-lavc-threads", lavcThreads.constData());
    mpv_set_option_string(m_mpv, "audio-buffer", "2.0");

    if (mpv_initialize(m_mpv) < 0) {
        qWarning("player: mpv_initialize fallo");
        // Handle inservible: al anularlo isValid() devuelve false y ningún
        // comando posterior toca un mpv muerto.
        mpv_destroy(m_mpv);
        m_mpv = nullptr;
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
    mpv_set_wakeup_callback(m_mpv, MpvBackend::mpvWakeup, this);
}

MpvBackend::~MpvBackend()
{
    shutdown();
}

double MpvBackend::speed() const
{
    double value = 1.0;
    if (m_mpv)
        mpv_get_property(m_mpv, "speed", MPV_FORMAT_DOUBLE, &value);
    return value;
}

QString MpvBackend::hwdecCurrent() const
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

QString MpvBackend::videoCodec() const
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

bool MpvBackend::initGL()
{
    if (!m_mpv || m_rctx)
        return m_rctx != nullptr;
    // Diagnóstico del contexto GL real (experimento ANGLE / desktop GL):
    // vendor "Google ... (ANGLE ...)" + es=1 confirman que el contexto del
    // widget es ANGLE y que mpv podrá intentar el hwdec zero-copy d3d11-egl
    // con hwdec=auto (en video/out/gpu/hwdec.c d3d11-egl va ANTES que
    // d3d11va; con GL de escritorio eglGetCurrentDisplay() es NULL y solo
    // queda d3d11va-copy, el copy-back del CPU al 100% en x64).
    if (QOpenGLContext *glc = QOpenGLContext::currentContext()) {
        QOpenGLFunctions *glf = glc->functions();
        const GLubyte *vendor = glf->glGetString(GL_VENDOR);
        const GLubyte *renderer = glf->glGetString(GL_RENDERER);
        if (vendor && renderer)
            NT_DEBUG("E2E GL ctx es=%d vendor=%s renderer=%s\n",
                     glc->isOpenGLES() ? 1 : 0,
                     reinterpret_cast<const char *>(vendor),
                     reinterpret_cast<const char *>(renderer));
    }
    mpv_opengl_init_params init{ getProcAddress, nullptr };
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL) },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &init },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    if (mpv_render_context_create(&m_rctx, m_mpv, params) < 0)
        return false;
    mpv_render_context_set_update_callback(m_rctx, MpvBackend::mpvUpdate, this);
    return true;
}

void MpvBackend::destroyRenderContext()
{
    if (m_rctx) {
        mpv_render_context_set_update_callback(m_rctx, nullptr, nullptr);
        mpv_render_context_free(m_rctx);
        m_rctx = nullptr;
    }
}

void MpvBackend::shutdown()
{
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
}

void MpvBackend::renderGL(int fbo, int width, int height)
{
    if (!m_rctx)
        return;
    mpv_opengl_fbo mpfbo{ fbo, width, height, 0 };
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
                 m_logFrames, width, height, res);
}

bool MpvBackend::playUrl(const QString &url, double startAt,
                         const QString &audioUrl)
{
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
    if (isLocalStreamUrl(url))
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
    NT_DEBUG("E2E playUrl len=%d http=%d audio=%d\n", int(u.size()),
             url.startsWith(QLatin1String("http")) ? 1 : 0,
             int(!audioUrl.isEmpty()));
    // Con audio externo (edge/AV1-HLS) el vídeo y el audio se abren en el MISMO
    // loadfile (opción per-file "audio-files"): mpv los sincroniza desde el
    // arranque, sin el vídeo mudo inicial y el dropfps del re-sync posterior.
    // Si el comando falla (mpv viejo sin la opción), se conserva el loadfile
    // simple y FILE_LOADED hará el audio-add de antes.
    if (!audioUrl.isEmpty()) {
        const int r = loadFileWithAudio(m_mpv, u, audioUrl);
        if (r >= 0) {
            return true; // audio adjuntado: el llamador puede descartar la URL
        }
        NT_DEBUG("E2E loadfile+audio-files fallo (%s); fallback audio-add\n",
                 mpv_error_string(r));
        mpv_command_async(m_mpv, 0, cmd);
    } else {
        mpv_command_async(m_mpv, 0, cmd);
    }
    return false; // se cargó sin audio: el llamador conserva la URL pendiente
}

void MpvBackend::setYtdlFormat(const QString &format)
{
    if (!m_mpv)
        return;
    mpv_set_option_string(m_mpv, "ytdl", "yes");
    mpv_set_option_string(m_mpv, "ytdl-format", format.toUtf8().constData());
}

void MpvBackend::stop()
{
    if (!m_mpv)
        return;
    const char *cmd[] = { "stop", nullptr };
    mpv_command_async(m_mpv, 0, cmd);
}

void MpvBackend::setPaused(bool paused)
{
    if (!m_mpv)
        return;
    int flag = paused ? 1 : 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvBackend::seek(double seconds)
{
    if (!m_mpv)
        return;
    mpv_set_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &seconds);
}

void MpvBackend::setVolume(double volume)
{
    if (!m_mpv)
        return;
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &volume);
}

void MpvBackend::setVolumeMax(double maxPercent)
{
    if (!m_mpv)
        return;
    const QByteArray v = QByteArray::number(maxPercent, 'f', 1);
    mpv_set_option_string(m_mpv, "volume-max", v.constData());
}

void MpvBackend::setSubVisibility(bool visible)
{
    if (!m_mpv)
        return;
    int flag = visible ? 1 : 0;
    mpv_set_property(m_mpv, "sub-visibility", MPV_FORMAT_FLAG, &flag);
}

int MpvBackend::attachAudio(const QString &url)
{
    if (!m_mpv)
        return MPV_ERROR_UNINITIALIZED;
    const QByteArray u = url.toUtf8();
    // audio-add select: añade la pista y la selecciona como audio actual.
    const char *cmd[] = { "audio-add", u.constData(), "select", nullptr };
    const int r = mpv_command(m_mpv, cmd);
    m_audioAdded = r >= 0;
    // La pista vieja (oído de un vídeo anterior) que mpv conserve se descarta
    // si hubo reemplazo: cada loadfile solo debe tener su audio propio.
    NT_DEBUG("E2E audio-add res=%d len=%d\n", r, int(u.size()));
    return r;
}

void MpvBackend::dropExternalAudio()
{
    if (!m_mpv)
        return;
    // Flujo clásico: nada que añadir; además limpia la pista externa
    // de un vídeo anterior con audio-file merodeando en mpv. El rc se ignora:
    // MPV_ERROR_COMMAND (sin pista que quitar) no es un fallo para nosotros.
    const char *drop[] = { "audio-file-remove", "auto", nullptr };
    mpv_command(m_mpv, drop);
    m_audioAdded = false;
}

void MpvBackend::setAudioLanguage(const QString &lang)
{
    m_audioLang = lang;
    if (!m_mpv)
        return;
    const QByteArray b = m_audioLang.toUtf8();
    mpv_set_option_string(m_mpv, "alang", b.constData());
}

int MpvBackend::probeMaxVideoHeight() const
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

// true si el motor tiene una pista de audio activa ("audio-params"). Con el
// audio externo (audio-files) rechazado por YouTube (403), el motor reproduce
// el vídeo sin ninguna pista de audio; "audio-params" no existe / queda vacío.
bool MpvBackend::hasActiveAudio() const
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

double MpvBackend::cacheDuration() const
{
    double v = 0.0;
    if (m_mpv)
        mpv_get_property(m_mpv, "demuxer-cache-duration", MPV_FORMAT_DOUBLE, &v);
    return v;
}

bool MpvBackend::cacheIdle() const
{
    int v = 0;
    if (m_mpv)
        mpv_get_property(m_mpv, "demuxer-cache-idle", MPV_FORMAT_FLAG, &v);
    return v != 0;
}

void MpvBackend::pollEvents()
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

void MpvBackend::handleEvent(mpv_event *event)
{
    switch (event->event_id) {
    case MPV_EVENT_FILE_LOADED:
        emit fileLoaded();
        break;
    case MPV_EVENT_PLAYBACK_RESTART:
    case MPV_EVENT_START_FILE:
        emit streamStart();
        break;
    case MPV_EVENT_END_FILE: {
        mpv_event_end_file *end = static_cast<mpv_event_end_file *>(event->data);
        switch (end->reason) {
        case MPV_END_FILE_REASON_QUIT:
            emit endFile(EndReason::Quit, QString());
            break;
        case MPV_END_FILE_REASON_STOP:
            emit endFile(EndReason::Stop, QString());
            break;
        case MPV_END_FILE_REASON_ERROR:
            emit endFile(EndReason::Error,
                         QString::fromUtf8(mpv_error_string(end->error)));
            break;
        default:
            emit endFile(EndReason::Eof, QString());
            break;
        }
        break;
    }
    case MPV_EVENT_PROPERTY_CHANGE: {
        mpv_event_property *prop = static_cast<mpv_event_property *>(event->data);
        if (!prop || !prop->name)
            break;
        const char *n = prop->name;
        if (std::strcmp(n, "time-pos") == 0 && prop->data) {
            emit positionUpdated(*static_cast<double *>(prop->data));
        } else if (std::strcmp(n, "duration") == 0 && prop->data) {
            emit durationUpdated(*static_cast<double *>(prop->data));
        } else if (std::strcmp(n, "video-params/w") == 0 && prop->data) {
            emit videoWidthUpdated(*static_cast<int64_t *>(prop->data));
        } else if (std::strcmp(n, "video-params/h") == 0 && prop->data) {
            emit videoHeightUpdated(*static_cast<int64_t *>(prop->data));
        } else if (std::strcmp(n, "pause") == 0 && prop->data) {
            emit pauseUpdated(*static_cast<int *>(prop->data) != 0);
        } else if (std::strcmp(n, "paused-for-cache") == 0 && prop->data) {
            const int v = *static_cast<int *>(prop->data);
            NT_DEBUG("E2E mpv paused-for-cache=%d\n", v);
        } else if (std::strcmp(n, "demuxer-cache-time") == 0 && prop->data) {
            const double v = *static_cast<double *>(prop->data);
            NT_DEBUG("E2E mpv cache-time=%.2f\n", v);
            emit cacheTimeUpdated(v);
        } else if (std::strcmp(n, "network-bytes-per-second") == 0 && prop->data) {
            const qint64 v = *static_cast<qint64 *>(prop->data);
            NT_DEBUG("E2E mpv bps=%lld\n", static_cast<long long>(v));
        }
        break;
    }
    case MPV_EVENT_LOG_MESSAGE: {
        mpv_event_log_message *log =
            static_cast<mpv_event_log_message *>(event->data);
        if (log) {
            if (log->text)
                emit logMessage(QString::fromUtf8(log->prefix),
                                QString::fromUtf8(log->text));
            // Veredicto del decodificador y errores del motor en bruto.
            NT_DEBUG("[mpv:%s] %s", log->prefix, log->text);
        }
        break;
    }
    default:
        break;
    }
}

void MpvBackend::drainEvents()
{
    pollEvents();
}

void MpvBackend::deliverFrameReady()
{
    emit frameReady();
}

void MpvBackend::mpvWakeup(void *ctx)
{
    MpvBackend *self = static_cast<MpvBackend *>(ctx);
    QMetaObject::invokeMethod(self, "drainEvents", Qt::QueuedConnection);
}

void MpvBackend::mpvUpdate(void *ctx)
{
    MpvBackend *self = static_cast<MpvBackend *>(ctx);
    QMetaObject::invokeMethod(self, "deliverFrameReady", Qt::QueuedConnection);
}