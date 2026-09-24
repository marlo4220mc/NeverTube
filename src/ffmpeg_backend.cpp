#include "ffmpeg_backend.h"

// FfmpegBackend — implementación (Fases 2-4: decode con HW opcional
// VAAPI/DXVA2, render GL frame-driven, audio pulse-simple/QAudioOutput).
//
// Construido sobre la receta probada en spike/cheapplayer (rama
// spike/cheap-player): demux HLS/local + decode + sws -> RGB + write de audio
// bloqueante como reloj maestro. Diferencias frente al spike:
//   - vive en la app bajo PlaybackBackend (mismas señales que MpvBackend);
//   - las señales se emiten SIEMPRE desde el hilo GUI (invocaciones
//     diferidas), así las conexiones directas del widget conservan el orden;
//   - stop() corta la red al instante (interrupt callback de avformat);
//   - pausa cortando el sink (pa_simple_cork / QAudioOutput suspend);
//   - seek con flush de decodificadores y reinicio del reloj maestro;
//   - Fase 4: al abrir se pide aceleración por hardware a FFmpeg (VAAPI en
//     Linux / DXVA2 en Windows) con copy-back de la superficie a memoria
//     antes de swscale; si no hay device o el códec no soporta HW, se
//     decodifica en software (el flujo es el mismo, solo cambia el origen).
//
// Reloj maestro: con pista de audio, el reloj es el propio AUDIO (muestras
// escritas al sink desde la base de cada seek), NO wall-time: el vídeo se
// ajusta a la pista descartando frames adelantados (>0.35 s) y saltando
// frames muy atrasados (>0.5 s, catch-up con audio externo lento). Sin audio,
// el propio vídeo pacea durmiendo hasta su pts (wall-time con corrección por
// pausa/seek). Position = reloj de audio (lo que se ha oído) o pts del último
// frame mostrado si no hay audio; pausa/seek no revierten la posición.

#include <QDebug>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QMutexLocker>
#include <QThread>

#include <cmath>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/rational.h>
}

#if !defined(Q_OS_WIN) && defined(NEVERTUBE_HAS_PULSE)
#include <pulse/error.h>
#include <pulse/simple.h>
#endif
#if defined(Q_OS_WIN)
#include <QAudio>
#include <QAudioOutput>
#endif

#include "debug.h"

// ---------------------------------------------------------------------------
// Constantes

// Tolerancia de "frame en el futuro" frente al reloj maestro antes de
// descartarlo (el vídeo nunca se adelanta más de ~0.35 s al audio).
static constexpr double kFrameAheadToleranceSec = 0.35;

// Tolerancia de "frame muy atrasado" frente al reloj maestro: con audio como
// reloj, si la pista de vídeo va más de ~0.5 s POR DETRÁS del audio (p. ej.
// decode SW lento, audio externo de YouTube por delante), se saltan frames
// para alcanzar a la pista de audio (mpv hace lo mismo). Sin salto, el vídeo
// se atrasa indefinidamente y el audio sigue avanzando.
static constexpr double kFrameBehindToleranceSec = 0.5;

// ---------------------------------------------------------------------------
// Estado publicado por el hilo de decode y leído por el GUI.

struct FfmpegBackend::Impl
{
    FfmpegBackend *owner = nullptr;

    // ---- comandos (GUI -> hilo de decode) ----
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> pause{false};
    std::atomic<int> seekTick{0};
    std::atomic<double> seekSec{0.0};
    std::atomic<double> volume{1.0}; // 1.0 = 100%
    std::atomic<bool> bench{false};

    // carga en curso (leída por el hilo al arrancar)
    QString url;
    QString audioUrl;
    double startAt = 0.0;
    QString reqHwdec; // device HW pedido ("vaapi"/"dxva2") -> configuredHwdec()

    // ---- snapshot público (consultas thread-safe) ----
    mutable QMutex snapMtx;
    struct Snap {
        double position = 0.0;
        double duration = 0.0;
        qint64 vw = 0;
        qint64 vh = 0;
        int maxVideoH = 0;
        double cacheSecs = 0.0;
        bool cacheIdle = false;
        bool audioActive = false;
        QString codec;           // códec del vídeo ("h264"/"vp9"/"av1", …)
        QString hwdec;           // "no" (SW) / nombre del device HW (Fase 4)
        int framesDecoded = 0;
    } snap;

    Snap readSnap() const
    {
        QMutexLocker l(&snapMtx);
        return snap;
    }

    // ---- frame más reciente (hilo de decode -> render GL) ----
    QMutex frameMtx;
    QImage latest;

    // ---- recursos GL (solo hilo GUI, creados de forma perezosa) ----
    QOpenGLShaderProgram *prog = nullptr;
    QOpenGLBuffer *vbo = nullptr;
    QOpenGLVertexArrayObject *vao = nullptr;
    GLuint tex = 0;
    int texW = 0;
    int texH = 0;
    bool glOk = false;
};

// ---------------------------------------------------------------------------
// Events del hilo de decode hacia el GUI (kind de onEngineEvent).

enum Ev {
    EvFileLoaded = 1,
    EvStreamStart,
    EvEndFile,
    EvPosition,
    EvDuration,
    EvVideoSize,
    EvPause,
    EvCache,
    EvLog,
};

// ---------------------------------------------------------------------------
// Sink de audio: write bloqueante que pacea el reloj maestro (equivalente al
// sink de mpv). La variante se elige por plataforma.

struct Sink
{
    virtual ~Sink() = default;
    // Abre salida PCM S16 interleaved estéreo a `rate` Hz. false → sin audio.
    virtual bool open(int rate) = 0;
    // Escribe `frames` (estéreo). Bloquea según el ritmo del sink.
    virtual void writeSamples(const int16_t *data, int frames) = 0;
    // Descartar lo pendiente (seek): no bloquear.
    virtual void flush() = 0;
    // Pausa/reanuda sin perder buffers.
    virtual void cork(bool paused) = 0;
    // Espera a que lo escrito haya sonado (final de archivo).
    virtual void drain() = 0;
    virtual void close() = 0;
    virtual bool opened() const = 0;
};

#if !defined(Q_OS_WIN) && defined(NEVERTUBE_HAS_PULSE)
// pulse-simple: el write bloquea hasta que el servidor acepta el dato, así el
// bucle queda paceado por el audio real (receta del spike).
class PulseSink final : public Sink
{
public:
    ~PulseSink() override { close(); }

    bool open(int rate) override
    {
        pa_sample_spec ss{};
        ss.format = PA_SAMPLE_S16LE;
        ss.rate = static_cast<uint32_t>(rate);
        ss.channels = 2;
        int err = 0;
        m_pa = pa_simple_new(nullptr, "nevertube", PA_STREAM_PLAYBACK, nullptr,
                             "nevertube", &ss, nullptr, nullptr, &err);
        return m_pa != nullptr;
    }

    void writeSamples(const int16_t *data, int frames) override
    {
        // Si el server murió (m_failed), no insistir con syscalls: el bucle
        // sigue contando muestras y el vídeo continúa mudo, SIN disparar el
        // watchdog de audio de la app en falso (la pista sigue activa en el
        // demux; es el sink el que no suena).
        if (!m_pa || m_failed || frames <= 0)
            return;
        const size_t bytes = static_cast<size_t>(frames) * 4u;
        if (pa_simple_write(m_pa, data, bytes, nullptr) < 0)
            m_failed = true;
    }

    void flush() override
    {
        if (m_pa)
            pa_simple_flush(m_pa, nullptr);
    }

    void cork(bool paused) override
    {
        // pulse-simple no tiene cork (eso es pa_stream del API completo);
        // pausar = descartar lo encolado en el servidor: el audio se congela
        // al instante y al reanudar simplemente se vuelve a escribir.
        if (m_pa && paused)
            pa_simple_flush(m_pa, nullptr);
    }

    void drain() override
    {
        // No hay pa_simple_drain: spin hasta que la latencia del servidor se
        // vacíe (lo escrito ya ha sonado) para no cortar la cola al final.
        if (!m_pa)
            return;
        int perr = 0;
        for (int i = 0; i < 200; ++i) {
            const pa_usec_t lat = pa_simple_get_latency(m_pa, &perr);
            if (perr < 0)
                break;
            if (lat <= 30000) // <= 30 ms pendiente -> prácticamente sonado
                break;
            QThread::msleep(5);
        }
    }

    void close() override
    {
        if (m_pa) {
            pa_simple_free(m_pa);
            m_pa = nullptr;
        }
    }

    bool opened() const override { return m_pa != nullptr; }
    bool failed() const { return m_failed; }

private:
    pa_simple *m_pa = nullptr;
    bool m_failed = false;
};
#elif defined(Q_OS_WIN)
// QAudioOutput (Qt Multimedia): el write no bloquea; el bucle pacea esperando
// hueco en el buffer del sink (equivalente al write bloqueante de pulse).
// Solo entra en la compilación del build x86 (Fase 5); en esta fase el .cpp
// Windows aún no se compila.
class QAudioSink final : public Sink
{
public:
    ~QAudioSink() override { close(); }

    bool open(int rate) override
    {
        QAudioFormat fmt;
        fmt.setSampleRate(rate);
        fmt.setChannelCount(2);
        fmt.setSampleSize(16);
        fmt.setCodec(QStringLiteral("audio/pcm"));
        fmt.setByteOrder(QAudioFormat::LittleEndian);
        fmt.setSampleType(QAudioFormat::SignedInt);
        m_out = std::make_unique<QAudioOutput>(fmt, nullptr);
        // Cola de ~0.8 s: margen para que el bucle no se atore con el ritmo
        // del sink.
        m_out->setBufferSize(rate * 4 / 5);
        m_dev = m_out->start();
        return m_dev != nullptr;
    }

    void writeSamples(const int16_t *data, int frames) override
    {
        if (!m_out || frames <= 0)
            return;
        // Device caído (cable/sink): no escribir, seguir contando muestras
        // (vídeo mudo pero la pista sigue activa para el watchdog de la app).
        if (m_out->error() != QAudio::NoError ||
            m_out->state() == QAudio::StoppedState)
            return;
        const qint64 bytes = static_cast<qint64>(frames) * 4;
        for (int i = 0; i < 4000; ++i) {
            if (m_out->bytesFree() >= bytes)
                break;
            QThread::msleep(2);
        }
        if (m_dev)
            m_dev->write(reinterpret_cast<const char *>(data), bytes);
    }

    void flush() override
    {
        if (!m_out)
            return;
        m_out->stop();
        m_out->start();
        m_dev = m_out->start();
    }

    void cork(bool paused) override
    {
        if (!m_out)
            return;
        if (paused)
            m_out->suspend();
        else
            m_out->resume();
    }

    void drain() override
    {
        if (!m_out)
            return;
        for (int i = 0; i < 2000; ++i) {
            if (m_out->bytesFree() >= m_out->bufferSize())
                break;
            QThread::msleep(5);
        }
        m_out->stop();
    }

    void close() override
    {
        m_dev = nullptr;
        m_out.reset();
    }

    bool opened() const override { return m_out != nullptr; }

private:
    std::unique_ptr<QAudioOutput> m_out;
    QIODevice *m_dev = nullptr;
};
#else
// Sin sink real (Linux sin pulse-simple): no hay quien pacee el write, así
// que el propio sink duerme el tiempo real equivalente a los frames escritos.
// Solo para builds de desarrollo sin pulse; la reproducción real de la x86
// (Fase 5) lleva su propio sink (WASAPI/QAudioOutput).
class NullSink final : public Sink
{
public:
    bool open(int) override { return true; }

    void writeSamples(const int16_t *, int frames) override
    {
        if (frames > 0) {
            const double us = static_cast<double>(frames) * 1e6 / 48000.0;
            QThread::usleep(static_cast<unsigned long>(us));
        }
    }

    void flush() override {}
    void cork(bool) override {}
    void drain() override {}
    void close() override {}
    bool opened() const override { return true; }
};
#endif

static std::unique_ptr<Sink> makeSink()
{
#if defined(Q_OS_WIN)
    return std::make_unique<QAudioSink>();
#elif defined(NEVERTUBE_HAS_PULSE)
    return std::make_unique<PulseSink>();
#else
    return std::make_unique<NullSink>();
#endif
}

// ---------------------------------------------------------------------------
// Helpers

// URL local (servidor HLS de la propia app): sin opciones de throttle.
static bool isLocalUrl(const char *url)
{
    const QByteArray u(url);
    return u.contains("127.0.0.1") || u.contains("localhost");
}

int FfmpegBackend::interruptCb(void *ctx)
{
    const Impl *m = static_cast<const Impl *>(ctx);
    return m->stop.load() ? 1 : 0;
}

// Abre el contenedor con el callback de interrupción y, para URLs http no
// locales, con http_multiple (varios rangos en paralelo: el throttle de
// YouTube es POR PETICIÓN, y una descarga continua de /videoplayback se
// estrangula a ~150 KB/s mientras que varias en paralelo no).
static bool openFormat(AVFormatContext **out, const QString &url, void *ictx)
{
    if (url.isEmpty())
        return false;
    *out = avformat_alloc_context();
    (*out)->interrupt_callback = { FfmpegBackend::interruptCb, ictx };
    AVDictionary *opts = nullptr;
    if (!isLocalUrl(url.toUtf8().constData()))
        av_dict_set(&opts, "http_multiple", "1", 0);
    const int rc =
        avformat_open_input(out, url.toUtf8().constData(), nullptr, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        // El contexto lo preasignamos nosotros -> la propiedad sigue siendo
        // nuestra si avformat_open_input falla (no lo libera él).
        if (*out)
            avformat_free_context(*out);
        *out = nullptr;
        return false;
    }
    return true;
}

// ¿El frame viene de una superficie de hardware (VAAPI/DXVA2)? Esos no se
// pueden pasar a swscale directamente: primero hay que traerlos a memoria.
static bool isHwPixelFormat(AVPixelFormat fmt)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(fmt);
    return d && (d->flags & AV_PIX_FMT_FLAG_HWACCEL);
}

// Abre un decodificador a partir de codecpar. Si hay un device HW creado
// (Fase 4) se lo pedimos a FFmpeg: él decide por códec si el hwaccel aplica
// (h264 en DXVA2/VAAPI; av1/vp9 sin HW en la máquina de pruebas -> software igual).
static AVCodecContext *openDecoder(const AVCodecParameters *par,
                                   AVBufferRef *hwDevice)
{
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec)
        return nullptr;
    AVCodecContext *cc = avcodec_alloc_context3(codec);
    if (!cc)
        return nullptr;
    if (avcodec_parameters_to_context(cc, par) < 0) {
        avcodec_free_context(&cc);
        return nullptr;
    }
    if (hwDevice) {
        cc->hw_device_ctx = av_buffer_ref(hwDevice);
    } else {
        // Decodificación SOFTWARE (target x86, h264 a 1 solo hilo por defecto):
        // h264/vp9/av1 paralelizan por frame-threading. min(núcleos, 4) como
        // tope para no sobresuscribir CPUs de 2 núcleos. NO se toca con
        // hwDevice activo (DXVA2/VAAPI ya decodifica; esos hwaccels no se
        // mezclan con frame-threading).
        const int cores = qMax(1, QThread::idealThreadCount());
        cc->thread_count = qMin(cores, 4);
    }
    if (avcodec_open2(cc, codec, nullptr) < 0) {
        avcodec_free_context(&cc);
        return nullptr;
    }
    return cc;
}

// Altura máxima entre las pistas de VÍDEO del contenedor (ignora poster
// adjuntos tipo "attached_pic", p. ej. el mjpeg de los mp3 con carátula).
static int maxVideoHeight(const AVFormatContext *fmt)
{
    int best = 0;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream *st = fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;
        if ((st->disposition & AV_DISPOSITION_ATTACHED_PIC) &&
            st->codecpar->codec_id == AV_CODEC_ID_MJPEG)
            continue;
        best = std::max(best, st->codecpar->height);
    }
    return best;
}

// Receta del spike: sws -> RGB24 dentro de un QImage reutilizable.
struct SwsState
{
    SwsContext *ctx = nullptr;
    int w = 0;
    int h = 0;
    AVPixelFormat fmt = AV_PIX_FMT_NONE;

    ~SwsState()
    {
        if (ctx)
            sws_freeContext(ctx);
    }
};

static void frameToImage(const AVFrame *f, QImage &img, SwsState &st)
{
    const int w = f->width;
    const int h = f->height;
    const AVPixelFormat fmt = static_cast<AVPixelFormat>(f->format);
    if (!st.ctx || st.w != w || st.h != h || st.fmt != fmt) {
        if (st.ctx)
            sws_freeContext(st.ctx);
        st.ctx = sws_getContext(w, h, fmt, w, h, AV_PIX_FMT_RGB24,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
        st.w = w;
        st.h = h;
        st.fmt = fmt;
    }
    if (!st.ctx) {
        img = QImage();
        return;
    }
    if (img.isNull() || img.width() != w || img.height() != h)
        img = QImage(w, h, QImage::Format_RGB888);
    uint8_t *dst[4] = { img.bits(), nullptr, nullptr, nullptr };
    int linesize[4] = { img.bytesPerLine(), 0, 0, 0 };
    sws_scale(st.ctx, f->data, f->linesize, 0, h, dst, linesize);
}

// Ganancia de volumen en software (S16): 1.0 no toca nada.
static void applyGain(int16_t *s, int n, float g)
{
    if (g == 1.0f)
        return;
    for (int i = 0; i < n; ++i) {
        const int32_t v = static_cast<int32_t>(static_cast<float>(s[i]) * g);
        s[i] = static_cast<int16_t>(
            std::max<int32_t>(-32768, std::min<int32_t>(32767, v)));
    }
}

// Entrega un evento al hilo GUI (invocación diferida -> onEngineEvent).
static void emitPost(FfmpegBackend *be, int kind, double a, double b, int c,
                     const QString &s)
{
    QMetaObject::invokeMethod(be, "onEngineEvent", Qt::QueuedConnection,
                              Q_ARG(int, kind), Q_ARG(double, a),
                              Q_ARG(double, b), Q_ARG(int, c),
                              Q_ARG(QString, s));
}

// Entrega un frame al hilo GUI (invocación diferida -> deliverFrame).
static void emitFrame(FfmpegBackend *be, const QImage &img)
{
    QMetaObject::invokeMethod(be, "deliverFrame", Qt::QueuedConnection,
                              Q_ARG(QImage, img));
}

// ---------------------------------------------------------------------------
// Hilo de decode

static void runLoop(FfmpegBackend::Impl *m)
{
    FfmpegBackend *be = m->owner;
    const bool bench = m->bench.load();

    // Contenedores y decodificadores (liberados en cleanup).
    AVFormatContext *vFmt = nullptr;
    AVFormatContext *aFmt = nullptr; // audio externo (audioUrl)
    AVCodecContext *vc = nullptr;    // vídeo
    AVCodecContext *ac = nullptr;    // audio (muxed de vFmt o externo)
    bool extAudio = false;           // ac proviene de aFmt
    int muxAudioIdx = -1;            // pista de audio muxed en vFmt (si ac vive ahí)
    int extAudioIdx = -1;

    auto cleanup = [&]() {
        if (ac)
            avcodec_free_context(&ac);
        if (vc)
            avcodec_free_context(&vc);
        if (aFmt)
            avformat_close_input(&aFmt);
        if (vFmt)
            avformat_close_input(&vFmt);
        NT_DEBUG("E2E ffmpeg hilo finalizado\n");
    };
    auto fail = [&](const QString &msg) {
        NT_DEBUG("E2E ffmpeg error: %s\n", msg.toUtf8().constData());
        emitPost(be, EvLog, 0, 0, 0, msg);
        emitPost(be, EvEndFile, 0, 0,
                 static_cast<int>(PlaybackBackend::EndReason::Error), msg);
        cleanup();
    };

    if (!openFormat(&vFmt, m->url, m)) {
        fail(QStringLiteral("No se pudo abrir la URL de vídeo"));
        return;
    }
    if (avformat_find_stream_info(vFmt, nullptr) < 0) {
        fail(QStringLiteral("No se pudo leer la información del stream"));
        return;
    }

    NT_DEBUG("E2E ffmpeg open <- %s (streams=%u dur=%.1fs)\n",
             m->url.toUtf8().constData(), vFmt->nb_streams,
             vFmt->duration > 0 ? vFmt->duration / 1e6 : 0.0);

    const int vs =
        av_find_best_stream(vFmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) {
        fail(QStringLiteral("No se encontró pista de vídeo en el stream"));
        return;
    }

    // --- Fase 4: device de aceleración por hardware (si existe) ---
    // Windows x86: DXVA2 (H.264 de la iGPU del target x86). Linux: VAAPI.
    // Si no se puede crear, se decodifica en software (el flujo manda).
    AVBufferRef *hwDevice = nullptr;
    const char *hwName = nullptr;
#if defined(Q_OS_WIN)
    hwName = "dxva2";
    if (av_hwdevice_ctx_create(&hwDevice, AV_HWDEVICE_TYPE_DXVA2, nullptr,
                               nullptr, 0) < 0)
        hwDevice = nullptr;
#else
    hwName = "vaapi";
    if (av_hwdevice_ctx_create(&hwDevice, AV_HWDEVICE_TYPE_VAAPI, nullptr,
                               nullptr, 0) < 0) {
        // Sin display X11 (headless/CI): intentar el render node por defecto.
        av_hwdevice_ctx_create(&hwDevice, AV_HWDEVICE_TYPE_VAAPI,
                               "/dev/dri/renderD128", nullptr, 0);
    }
    if (!hwDevice)
        hwName = nullptr;
#endif
    const QString hwNameQ =
        hwDevice ? QString::fromLatin1(hwName) : QString();
    NT_DEBUG("E2E ffmpeg hw device -> %s\n",
             hwDevice ? hwName : "(no disponible, SW)");

    vc = openDecoder(vFmt->streams[vs]->codecpar, hwDevice);
    if (!vc) {
        fail(QStringLiteral("Sin decodificador de vídeo para el códec %1")
                 .arg(QString::fromUtf8(
                     avcodec_get_name(vFmt->streams[vs]->codecpar->codec_id))));
        return;
    }

    const int64_t durUs = vFmt->duration;
    const double duration =
        (durUs > 0 && durUs != AV_NOPTS_VALUE) ? durUs / 1e6 : 0.0;
    {
        QMutexLocker l(&m->snapMtx);
        m->snap.codec = QString::fromUtf8(avcodec_get_name(vc->codec->id));
        m->snap.duration = duration;
        m->snap.maxVideoH = maxVideoHeight(vFmt);
        m->snap.vw = vFmt->streams[vs]->codecpar->width;
        m->snap.vh = vFmt->streams[vs]->codecpar->height;
        // hwdec se resuelve con el PRIMER frame procesado (mpv deja
        // hwdec-current vacío hasta que el decodificador activa; el widget
        // reintenta con retryHwdec hasta saberlo).
        m->snap.hwdec.clear();
        m->snap.cacheIdle = true; // sin modelo de "cache": se puede tocar ya
    }

    // --- audio: externo si viene y abre; si no, el muxed del vídeo ---
    if (!m->audioUrl.isEmpty()) {
        if (openFormat(&aFmt, m->audioUrl, m) &&
            avformat_find_stream_info(aFmt, nullptr) >= 0) {
            extAudioIdx =
                av_find_best_stream(aFmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (extAudioIdx >= 0)
                ac = openDecoder(aFmt->streams[extAudioIdx]->codecpar,
                                 hwDevice);
        }
        if (!ac && aFmt) {
            avformat_close_input(&aFmt);
            aFmt = nullptr;
            NT_DEBUG("E2E ffmpeg audio externo no disponible; probando muxed\n");
        }
    }
    if (!ac) {
        muxAudioIdx =
            av_find_best_stream(vFmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (muxAudioIdx >= 0)
            ac = openDecoder(vFmt->streams[muxAudioIdx]->codecpar, hwDevice);
    }

    // Sink (solo reproducción real; el banco decodifica sin sonido).
    std::unique_ptr<Sink> sink;
    const bool hasAudio = ac != nullptr;
    if (hasAudio && !bench) {
        sink = makeSink();
        if (!sink->open(48000)) {
            NT_DEBUG("E2E ffmpeg: sin sink de audio (salida video-only)\n");
            sink.reset();
        }
    }
    if (ac)
        NT_DEBUG("E2E ffmpeg audio %dHz ch=%d src=%s\n", ac->sample_rate,
                 ac->ch_layout.nb_channels,
                 extAudio ? "externa" : "muxed");

    // Resampler a S16 estéreo 48 kHz.
    SwrContext *swr = nullptr;
    uint8_t *abuf = nullptr;
    int abufCap = 0;
    if (ac) {
        AVChannelLayout outLayout{};
        av_channel_layout_default(&outLayout, 2);
        const int rc = swr_alloc_set_opts2(
            &swr, &outLayout, AV_SAMPLE_FMT_S16, 48000, &ac->ch_layout,
            ac->sample_fmt, ac->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&outLayout);
        if (rc < 0 || !swr) {
            NT_DEBUG("E2E ffmpeg: swr_alloc_set_opts2 falló; sin audio\n");
            ac = nullptr;
        }
    }

    // --- eventos iniciales (orden básico de mpv: loaded -> dur -> tamaño) ---
    emitPost(be, EvFileLoaded, 0, 0, 0, QString());
    emitPost(be, EvDuration, duration, 0, 0, QString());
    if (m->snap.vw > 0 && m->snap.vh > 0)
        emitPost(be, EvVideoSize, static_cast<double>(m->snap.vw),
                 static_cast<double>(m->snap.vh), 0, QString());
    emitPost(be, EvLog, 0, 0, 0,
             QStringLiteral("vídeo %1x%2 códec=%3 hw=%4")
                 .arg(m->snap.vw)
                 .arg(m->snap.vh)
                 .arg(m->snap.codec)
                 .arg(hwDevice ? hwNameQ : QStringLiteral("no")));
    emitPost(be, EvCache, 1.0, 0, 0, QString());
    emitPost(be, EvStreamStart, 0, 0, 0, QString());

    // --- reloj maestro y helpers ---
    int64_t t0 = av_gettime_relative(); // re-basado en seek inicial y seeks
    int64_t pausedUs = 0;
    int64_t pauseStartUs = 0;
    bool wasPaused = false;
    int64_t audioSamples = 0; // muestras estéreo escritas (posición de audio)
    // Base del reloj de AUDIO: con pista de audio, el reloj maestro es el
    // propio audio (lo que se ha escrito al sink), NO wall-time — así pausas,
    // seeks y atascos del vídeo no desincronizan HD: el vídeo se ajusta a la
    // pista. Base = posición del contenido en el momento de (re)arrancar la
    // cuenta (startAt / destino del seek), porque audioSamples cuenta desde 0
    // tras cada seek.
    double audioClockBase = m->startAt;
    double lastVideoPtsShown = 0.0;
    bool audioActiveOnce = false;
    int64_t lastPosEmitUs = 0;
    bool hwdecResolved = false; // veredicto HW/SW definido con el primer frame

    auto audioPos = [&]() {
        return static_cast<double>(audioSamples) / 48000.0;
    };
    auto masterClock = [&]() {
        if (ac)
            return audioClockBase + audioPos();
        return static_cast<double>(av_gettime_relative() - t0 - pausedUs) /
               1e6;
    };
    auto setPosition = [&](double p, bool force) {
        {
            QMutexLocker l(&m->snapMtx);
            m->snap.position = p;
        }
        const int64_t now = av_gettime_relative();
        if (!force && now - lastPosEmitUs < 100000)
            return;
        lastPosEmitUs = now;
        emitPost(be, EvPosition, p, 0, 0, QString());
    };
    auto setAudioActive = [&]() {
        if (audioActiveOnce)
            return;
        audioActiveOnce = true;
        QMutexLocker l(&m->snapMtx);
        m->snap.audioActive = true;
    };
    auto rebaseClock = [&](double sec) {
        t0 = av_gettime_relative() - static_cast<int64_t>(sec * 1e6);
        pausedUs = 0;
        pauseStartUs = wasPaused ? av_gettime_relative() : 0;
    };

    // --- seek inicial (reanudar posición) ---
    if (m->startAt > 0.0) {
        const int64_t us = static_cast<int64_t>(m->startAt * 1e6);
        avformat_seek_file(vFmt, -1, INT64_MIN, us, INT64_MAX, 0);
        if (aFmt)
            avformat_seek_file(aFmt, -1, INT64_MIN, us, INT64_MAX, 0);
        if (vc)
            avcodec_flush_buffers(vc);
        if (ac)
            avcodec_flush_buffers(ac);
        if (sink)
            sink->flush();
        rebaseClock(m->startAt);
        setPosition(m->startAt, true);
        NT_DEBUG("E2E ffmpeg reanuda %.1fs\n", m->startAt);
    }

    // --- bucle principal ---
    int appliedSeekTick = 0;
    bool vReadDone = false; // lectura del contenedor vídeo agotada (EOF)
    bool aReadDone = false; // lectura del audio externo agotada (EOF)
    bool draining = false;  // drenado final: NO descartar frames "futuros"

    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    AVFrame *hwScratch = av_frame_alloc(); // copia SW del frame HW (copy-back)
    SwsState swsState;
    QImage outFrame;

    auto handleVideoFrame = [&](AVFrame *f, double tb) {
        const double wall = masterClock();
        const double pts =
            (f->pts == AV_NOPTS_VALUE) ? wall : static_cast<double>(f->pts) * tb;
        if (!draining && f->pts != AV_NOPTS_VALUE &&
            pts > wall + kFrameAheadToleranceSec)
            return; // demasiado en el futuro (el audio pacea) -> descartar
        if (!draining && ac && f->pts != AV_NOPTS_VALUE &&
            pts < wall - kFrameBehindToleranceSec)
            return; // muy ATRASADO frente al audio: saltar para alcanzar la
                    // pista de audio (decode SW lento / audio externo)
        if (!ac && !draining) {
            // sin audio: el reloj es el propio vídeo; dormir hasta su pts.
            const double d = pts - wall;
            if (d > 0.002)
                av_usleep(static_cast<unsigned>(d * 1e6));
        }
        // Fase 4: si el frame viene de una superficie HW (VAAPI/DXVA2),
        // traerlo a memoria (copy-back) antes de swscale; un fallo aquí solo
        // descarta ese frame.
        AVFrame *src = f;
        const bool hwFrame =
            isHwPixelFormat(static_cast<AVPixelFormat>(f->format));
        if (hwFrame) {
            av_frame_unref(hwScratch);
            if (av_hwframe_transfer_data(hwScratch, f, 0) < 0)
                return;
            src = hwScratch;
        }
        frameToImage(src, outFrame, swsState);
        if (outFrame.isNull())
            return;
        if (!bench)
            emitFrame(be, outFrame);
        if (!hwdecResolved) {
            // Primer frame procesado: veredicto HW/SW para el widget
            // (streamDecodeInfoReady) y la escalera reactiva de códec.
            hwdecResolved = true;
            QMutexLocker l(&m->snapMtx);
            m->snap.hwdec =
                (hwDevice && hwFrame) ? hwNameQ : QStringLiteral("no");
        }
        lastVideoPtsShown = pts;
        {
            QMutexLocker l(&m->snapMtx);
            if (m->snap.vw != src->width || m->snap.vh != src->height) {
                m->snap.vw = src->width;
                m->snap.vh = src->height;
                l.unlock();
                emitPost(be, EvVideoSize, static_cast<double>(src->width),
                         static_cast<double>(src->height), 0, QString());
                l.relock();
            }
            ++m->snap.framesDecoded;
        }
        setPosition(ac ? audioClockBase + audioPos() : pts, false);
    };

    auto handleAudioFrame = [&](AVFrame *f) {
        if (!swr || !ac)
            return;
        const int outN = swr_get_out_samples(swr, f->nb_samples);
        const int need =
            av_samples_get_buffer_size(nullptr, 2, outN, AV_SAMPLE_FMT_S16, 1);
        if (need > abufCap) {
            av_free(abuf);
            abuf = static_cast<uint8_t *>(av_malloc(need));
            abufCap = need;
        }
        const int n =
            swr_convert(swr, &abuf, outN,
                        const_cast<const uint8_t **>(f->extended_data),
                        f->nb_samples);
        if (n > 0) {
            const float gain = m->volume.load();
            applyGain(reinterpret_cast<int16_t *>(abuf), n * 2, gain);
            if (sink)
                sink->writeSamples(reinterpret_cast<int16_t *>(abuf), n);
            audioSamples += n;
            setAudioActive();
        }
    };

    auto feedAudio = [&](AVPacket *p) {
        if (avcodec_send_packet(ac, p) < 0)
            return;
        while (!m->stop.load()) {
            const int gr = avcodec_receive_frame(ac, frame);
            if (gr == 0) {
                handleAudioFrame(frame);
                av_frame_unref(frame);
            } else {
                break;
            }
        }
    };

    while (!m->stop.load()) {
        // ---- seek ----
        if (m->seekTick.load() != appliedSeekTick) {
            appliedSeekTick = m->seekTick.load();
            const double sec = m->seekSec.load();
            const int64_t us = static_cast<int64_t>(sec * 1e6);
            avformat_seek_file(vFmt, -1, INT64_MIN, us, INT64_MAX, 0);
            if (aFmt)
                avformat_seek_file(aFmt, -1, INT64_MIN, us, INT64_MAX, 0);
            if (vc)
                avcodec_flush_buffers(vc);
            if (ac)
                avcodec_flush_buffers(ac);
            if (swr)
                swr_close(swr); // vacía el estado interno del resampler
            if (sink)
                sink->flush();
            vReadDone = false;
            aReadDone = false;
            audioSamples = 0;
            audioClockBase = sec;
            rebaseClock(sec);
            setPosition(sec, true);
            emitPost(be, EvCache, 1.0, 0, 0, QString());
            NT_DEBUG("E2E ffmpeg seek %.1fs\n", sec);
            continue;
        }

        // ---- pausa ----
        if (m->pause.load() != wasPaused) {
            if (m->pause.load()) {
                pauseStartUs = av_gettime_relative();
                if (sink)
                    sink->cork(true);
                emitPost(be, EvPause, 0, 0, 1, QString());
            } else {
                pausedUs += av_gettime_relative() - pauseStartUs;
                if (sink)
                    sink->cork(false);
                emitPost(be, EvPause, 0, 0, 0, QString());
            }
            wasPaused = m->pause.load();
            NT_DEBUG("E2E ffmpeg pausa=%d\n", wasPaused ? 1 : 0);
        }
        if (wasPaused) {
            QThread::msleep(10);
            continue;
        }

        // ---- leer del contenedor de vídeo (vídeo + audio muxed) ----
        if (!vReadDone && !m->stop.load()) {
            const int r = av_read_frame(vFmt, pkt);
            if (r < 0) {
                if (r == AVERROR_EOF) {
                    vReadDone = true;
                } else if (r == AVERROR(EAGAIN)) {
                    // HLS en vivo: esperar un poco y seguir.
                    av_usleep(100 * 1000);
                } else {
                    if (m->stop.load())
                        break;
                    char err[AV_ERROR_MAX_STRING_SIZE] = { 0 };
                    av_strerror(r, err, sizeof err);
                    fail(QStringLiteral("Error de red/stream: %1")
                             .arg(QString::fromUtf8(err)));
                    break;
                }
            } else {
                if (pkt->stream_index == vs) {
                    // decodificar vídeo
                    if (avcodec_send_packet(vc, pkt) >= 0) {
                        while (!m->stop.load()) {
                            const int gr = avcodec_receive_frame(vc, frame);
                            if (gr == 0) {
                                handleVideoFrame(
                                    frame, av_q2d(vFmt->streams[vs]->time_base));
                                av_frame_unref(frame);
                            } else {
                                break;
                            }
                        }
                    }
                } else if (ac && !extAudio &&
                           pkt->stream_index == muxAudioIdx) {
                    feedAudio(pkt);
                }
                av_packet_unref(pkt);
            }
        }

        // ---- leer del contenedor de audio externo ----
        if (extAudio && !aReadDone && !m->stop.load()) {
            const int r = av_read_frame(aFmt, pkt);
            if (r < 0) {
                if (r == AVERROR_EOF) {
                    aReadDone = true;
                } else if (r == AVERROR(EAGAIN)) {
                    av_usleep(100 * 1000);
                } else {
                    if (m->stop.load())
                        break;
                    char err[AV_ERROR_MAX_STRING_SIZE] = { 0 };
                    av_strerror(r, err, sizeof err);
                    fail(QStringLiteral("Error de audio externo: %1")
                             .arg(QString::fromUtf8(err)));
                    break;
                }
            } else {
                if (pkt->stream_index == extAudioIdx)
                    feedAudio(pkt);
                av_packet_unref(pkt);
            }
        }

        // ---- fin: drenar decodificadores y cerrar con EOF ----
        if (vReadDone && (!extAudio || aReadDone)) {
            draining = true;

            // Drenar el vídeo (puede quedar algún frame en cola).
            avcodec_send_packet(vc, nullptr);
            while (!m->stop.load()) {
                const int gr = avcodec_receive_frame(vc, frame);
                if (gr == 0) {
                    handleVideoFrame(frame,
                                     av_q2d(vFmt->streams[vs]->time_base));
                    av_frame_unref(frame);
                } else {
                    break;
                }
            }
            // Drenar el audio (muxed o externo).
            if (ac) {
                avcodec_send_packet(ac, nullptr);
                while (!m->stop.load()) {
                    const int gr = avcodec_receive_frame(ac, frame);
                    if (gr == 0) {
                        handleAudioFrame(frame);
                        av_frame_unref(frame);
                    } else {
                        break;
                    }
                }
            }
            if (sink)
                sink->drain();
            const double finalPos = ac ? audioClockBase + audioPos()
                                 : lastVideoPtsShown;
            {
                QMutexLocker l(&m->snapMtx);
                m->snap.cacheIdle = true;
            }
            setPosition(finalPos, true);
            NT_DEBUG("E2E ffmpeg EOF (pos=%.1f dur=%.1f)\n", finalPos,
                     duration);
            emitPost(be, EvEndFile, 0, 0,
                     static_cast<int>(PlaybackBackend::EndReason::Eof),
                     QString());
            break;
        }

        // Pausa corta entre ciclos cuando el vídeo va más deprisa que el
        // audio (evita un bucle candente en video-only con frames sin pts).
        if (!ac && !vReadDone)
            QThread::usleep(2000);
    } // while (!m->stop.load())

    av_frame_free(&frame);
    av_frame_free(&hwScratch);
    av_packet_free(&pkt);
    av_free(abuf);
    if (swr)
        swr_free(&swr);
    if (hwDevice)
        av_buffer_unref(&hwDevice);
    cleanup();
}

// ---------------------------------------------------------------------------
// Render GL (solo hilo GUI; paintGL llama renderGL con el contexto actual).

// Comprime el render en un bloque estático para tenerlo todo a la vista.
static void renderGLCore(FfmpegBackend::Impl *m, const QImage &img, int w,
                         int h)
{
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    if (!ctx)
        return;
    QOpenGLFunctions *fn = ctx->functions();

    if (!m->glOk) {
        m->prog = new QOpenGLShaderProgram();
        const char *vsrc =
            "attribute vec2 aPos;\n"
            "attribute vec2 aTex;\n"
            "varying vec2 vTex;\n"
            "void main() { vTex = aTex; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
        const char *fsrc =
            "varying vec2 vTex;\n"
            "uniform sampler2D uTex;\n"
            "void main() { gl_FragColor = texture2D(uTex, vTex); }\n";
        if (!m->prog->addShaderFromSourceCode(QOpenGLShader::Vertex, vsrc) ||
            !m->prog->addShaderFromSourceCode(QOpenGLShader::Fragment, fsrc) ||
            !m->prog->link()) {
            qWarning() << "ffmpeg: shader GL no compiló:"
                       << m->prog->log();
            delete m->prog;
            m->prog = nullptr;
            fn->glClearColor(0.f, 0.f, 0.f, 1.f);
            fn->glClear(GL_COLOR_BUFFER_BIT);
            return;
        }
        m->prog->bindAttributeLocation("aPos", 0);
        m->prog->bindAttributeLocation("aTex", 1);
        m->vao = new QOpenGLVertexArrayObject();
        m->vao->create();
        m->vbo = new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer);
        m->vbo->create();
        m->vbo->bind();
        m->vbo->allocate(static_cast<int>(sizeof(GLfloat) * 16));
        m->vbo->release();
        m->glOk = true;
    }

    fn->glViewport(0, 0, w, h);
    fn->glDisable(GL_DEPTH_TEST);
    fn->glClearColor(0.f, 0.f, 0.f, 1.f);
    fn->glClear(GL_COLOR_BUFFER_BIT);
    if (img.isNull() || img.width() <= 0 || img.height() <= 0)
        return;

    // Textura RGB888 (subida completa solo si cambia el tamaño).
    if (!m->tex)
        fn->glGenTextures(1, &m->tex);
    fn->glActiveTexture(GL_TEXTURE0);
    fn->glBindTexture(GL_TEXTURE_2D, m->tex);
    fn->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (m->texW != img.width() || m->texH != img.height()) {
        fn->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, img.width(), img.height(), 0,
                         GL_RGB, GL_UNSIGNED_BYTE, img.constBits());
        m->texW = img.width();
        m->texH = img.height();
    } else {
        fn->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, img.width(), img.height(),
                            GL_RGB, GL_UNSIGNED_BYTE, img.constBits());
    }
    fn->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    fn->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    fn->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    fn->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Quad ajustado al aspect del frame (letterbox: el widget ya se dimensiona
    // al aspect por WatchPage, pero redondeos de 1 px no deben estirar).
    const float imgA = static_cast<float>(img.width()) / img.height();
    const float winA = static_cast<float>(w) / static_cast<float>(h ? h : 1);
    float nx1 = -1.f, nx2 = 1.f, ny1 = -1.f, ny2 = 1.f;
    if (winA > imgA) { // franjas laterales
        const float dw = static_cast<float>(h) * imgA;
        nx1 = -dw / static_cast<float>(w);
        nx2 = dw / static_cast<float>(w);
    } else { // franjas arriba/abajo
        const float dh = static_cast<float>(w) / imgA;
        ny1 = -dh / static_cast<float>(h);
        ny2 = dh / static_cast<float>(h);
    }
    // v=0 arriba (primera fila del QImage), v=1 abajo -> imagen derecha.
    const GLfloat verts[16] = {
        nx1, ny1, 0.f, 1.f, nx2, ny1, 1.f, 1.f,
        nx1, ny2, 0.f, 0.f, nx2, ny2, 1.f, 0.f,
    };

    m->prog->bind();
    m->prog->setUniformValue("uTex", 0);
    if (m->vao->isCreated())
        m->vao->bind();
    m->vbo->bind();
    m->vbo->write(0, verts, static_cast<int>(sizeof verts));
    m->prog->enableAttributeArray(0);
    m->prog->setAttributeBuffer(0, GL_FLOAT, 0, 2, 4 * static_cast<int>(sizeof(GLfloat)));
    m->prog->enableAttributeArray(1);
    m->prog->setAttributeBuffer(1, GL_FLOAT,
                                2 * static_cast<int>(sizeof(GLfloat)), 2,
                                4 * static_cast<int>(sizeof(GLfloat)));
    fn->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m->vbo->release();
    if (m->vao->isCreated())
        m->vao->release();
    m->prog->release();
    fn->glBindTexture(GL_TEXTURE_2D, 0);
}

// ---------------------------------------------------------------------------
// FfmpegBackend

FfmpegBackend::FfmpegBackend(QObject *parent)
    : PlaybackBackend(parent)
    , m(new Impl)
{
    m->owner = this;
}

FfmpegBackend::~FfmpegBackend()
{
    shutdown();
    delete m;
}

bool FfmpegBackend::initGL() { return true; }

void FfmpegBackend::destroyRenderContext()
{
    if (m->tex && QOpenGLContext::currentContext()) {
        QOpenGLContext::currentContext()->functions()->glDeleteTextures(1,
                                                                        &m->tex);
        m->tex = 0;
        m->texW = m->texH = 0;
    }
    delete m->vao;
    m->vao = nullptr;
    delete m->vbo;
    m->vbo = nullptr;
    delete m->prog;
    m->prog = nullptr;
    m->glOk = false;
}

void FfmpegBackend::shutdown()
{
    if (m->thread.joinable()) {
        m->stop = true;
        m->thread.join();
        m->thread = std::thread();
        m->stop = false;
    }
}

bool FfmpegBackend::isValid() const { return true; }

bool FfmpegBackend::hasRenderContext() const { return true; }

void *FfmpegBackend::renderContextHandle() const { return nullptr; }

void FfmpegBackend::renderGL(int, int width, int height)
{
    QImage img;
    {
        QMutexLocker l(&m->frameMtx);
        img = m->latest;
    }
    renderGLCore(m, img, width, height);
}

bool FfmpegBackend::playUrl(const QString &url, double startAt,
                            const QString &audioUrl)
{
    if (m->thread.joinable()) {
        m->stop = true;
        m->thread.join();
        m->thread = std::thread();
        m->stop = false;
    }
    m->url = url;
    m->audioUrl = audioUrl;
    m->startAt = std::max(0.0, startAt);
    m->pause = false;
    // Device HW solicitado para configuredhwdec() (lo que de verdad se usa
    // se resuelve con el primer frame -> hwdecCurrent()).
    m->reqHwdec = QLatin1String(
#if defined(Q_OS_WIN)
        "dxva2"
#else
        "vaapi"
#endif
    );
    {
        QMutexLocker l(&m->snapMtx);
        m->snap = Impl::Snap{};
        m->snap.hwdec.clear(); // desconocido hasta el primer frame (mpv-style)
        m->snap.cacheIdle = true;
    }
    {
        QMutexLocker l(&m->frameMtx);
        m->latest = QImage();
    }
    m->thread = std::thread([this] { runLoop(m); });
    // El audio externo se gestiona dentro de la propia carga (como mpv con
    // "audio-files"): devolvemos true para que el widget descarte el respaldo.
    return true;
}

void FfmpegBackend::setYtdlFormat(const QString &) {}

void FfmpegBackend::stop()
{
    shutdown();
}

void FfmpegBackend::setPaused(bool paused) { m->pause = paused; }

void FfmpegBackend::seek(double seconds) { m->seekSec = seconds; m->seekTick++; }

void FfmpegBackend::setVolume(double volume)
{
    // El volumen llega en % (0..100, con tope posible hasta 200). 1.0 = 100%.
    const double g = volume / 100.0;
    m->volume = std::max(0.0, std::min(4.0, g));
}

void FfmpegBackend::setVolumeMax(double) {}

void FfmpegBackend::setSubVisibility(bool) {}

void FfmpegBackend::setAudioLanguage(const QString &) {}

int FfmpegBackend::attachAudio(const QString &) { return -1; }

void FfmpegBackend::dropExternalAudio() {}

double FfmpegBackend::speed() const { return 1.0; }

QString FfmpegBackend::configuredHwdec() const
{
    // El device pedido para el próximo load; el que de verdad se usa tras el
    // primer frame es hwdecCurrent() (puede acabar en SW si no hay device).
    return m->reqHwdec.isEmpty() ? QStringLiteral("no") : m->reqHwdec;
}

QString FfmpegBackend::hwdecCurrent() const { return m->readSnap().hwdec; }

QString FfmpegBackend::videoCodec() const { return m->readSnap().codec; }

int FfmpegBackend::probeMaxVideoHeight() const
{
    return m->readSnap().maxVideoH;
}

bool FfmpegBackend::hasActiveAudio() const
{
    return m->readSnap().audioActive;
}

double FfmpegBackend::cacheDuration() const
{
    return m->readSnap().cacheSecs;
}

bool FfmpegBackend::cacheIdle() const { return m->readSnap().cacheIdle; }

void FfmpegBackend::pollEvents() {}

void FfmpegBackend::deliverFrame(const QImage &img)
{
    {
        QMutexLocker l(&m->frameMtx);
        m->latest = img;
    }
    emit frameReady();
}

void FfmpegBackend::onEngineEvent(int kind, double a, double b, int c,
                                  const QString &s)
{
    switch (kind) {
    case EvFileLoaded:
        emit fileLoaded();
        break;
    case EvStreamStart:
        emit streamStart();
        break;
    case EvEndFile:
        emit endFile(static_cast<EndReason>(c), s);
        break;
    case EvPosition:
        emit positionUpdated(a);
        break;
    case EvDuration:
        emit durationUpdated(a);
        break;
    case EvVideoSize:
        emit videoWidthUpdated(static_cast<qint64>(a));
        emit videoHeightUpdated(static_cast<qint64>(b));
        break;
    case EvPause:
        emit pauseUpdated(c != 0);
        break;
    case EvCache:
        emit cacheTimeUpdated(a);
        break;
    case EvLog:
        emit logMessage(QStringLiteral("ffmpeg"), s);
        break;
    default:
        break;
    }
}

void FfmpegBackend::setBenchMode(bool on) { m->bench = on; }

int FfmpegBackend::framesDecoded() const { return m->readSnap().framesDecoded; }

QSize FfmpegBackend::videoSize() const
{
    const Impl::Snap s = m->readSnap();
    return { static_cast<int>(s.vw), static_cast<int>(s.vh) };
}