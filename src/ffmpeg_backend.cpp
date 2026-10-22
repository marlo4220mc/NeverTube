#include "ffmpeg_backend.h"

// FfmpegBackend — implementation (Phases 2-4: decode with optional HW
// VAAPI/DXVA2, render GL frame-driven, audio pulse-simple/QAudioOutput).
//
// Built on the recipe proven in spike/cheapplayer (branch
// spike/cheap-player): HLS/local demux + decode + sws -> RGB + blocking
// audio write as master clock. Differences vs the spike:
//   - it lives in the app under PlaybackBackend (same signals as MpvBackend);
//   - signals are ALWAYS emitted from the GUI thread (deferred invocations),
//     so the widget's direct connections keep their order;
//   - stop() cuts network reads instantly (avformat interrupt callback);
//   - pause by corking the sink (pa_simple_cork / QAudioOutput suspend);
//   - seek with decoder flush and master clock rebase;
//   - Phase 4: on open, hardware acceleration is requested from FFmpeg (VAAPI on
//     Linux / DXVA2 on Windows) with copy-back of the surface to memory
//     before swscale; if there is no device or the codec does not support HW, it
//     decodes in software (the flow is the same, only the source changes).
//
// Master clock: with an audio track, the clock is the AUDIO itself (samples
// written to the sink from each seek's base), NOT wall-time: the video is
// adjusted to the track by dropping early frames (>0.35 s) and skipping
// very late frames (>0.5 s, catch-up with slow external audio). With no audio,
// the video itself paces by sleeping until its pts (wall-time corrected by
// pause/seek). Position = audio clock (what has been heard) or the last
// shown frame's pts if there is no audio; pause/seek do not revert the position.

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

// FFmpeg 5.1 (libavutil 57.28) replaced the old channel_layout/channels pair
// with AVChannelLayout and swr_alloc_set_opts with swr_alloc_set_opts2. Keep
// both paths so the backend also compiles against the older FFmpeg shipped by
// distro LTS releases (Ubuntu 22.04 has 4.4 / libavutil 56). The vendored
// Windows i686 package and every modern distro take the new path unchanged.
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
#define NT_FFMPEG_NEW_CHLAYOUT 1
#endif

#if !defined(Q_OS_WIN) && defined(NEVERTUBE_HAS_PULSE)
#include <pulse/error.h>
#include <pulse/simple.h>
#endif
#if defined(Q_OS_WIN)
#include <QAudio>
#include <QAudioOutput>
#endif

#include "debug.h"
#include "i18n.h"

// ---------------------------------------------------------------------------
// Constantes

// Tolerance of a "future frame" against the master clock before dropping it
// (the video never runs ahead of the audio by more than ~0.35 s).
static constexpr double kFrameAheadToleranceSec = 0.35;

// Tolerance of a "very late frame" against the master clock: with audio as
// the clock, if the video track runs more than ~0.5 s BEHIND the audio (e.g.
// slow SW decode, YouTube's external audio ahead), frames are skipped to
// catch up with the audio track (mpv does the same). Without skipping, the
// video falls behind indefinitely while the audio keeps advancing.
static constexpr double kFrameBehindToleranceSec = 0.5;

// ---------------------------------------------------------------------------
// State published by the decode thread and read by the GUI.

struct FfmpegBackend::Impl
{
    FfmpegBackend *owner = nullptr;

    // ---- commands (GUI -> decode thread) ----
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> pause{false};
    std::atomic<int> seekTick{0};
    std::atomic<double> seekSec{0.0};
    std::atomic<double> volume{1.0}; // 1.0 = 100%
    std::atomic<bool> bench{false};
    // Covered by another window (detected by the widget, 250 ms timer, only
    // on the x86 backend): while true, the decode thread does NOT transfer
    // (copy-back) nor convert (swscale) frames and renderGL uploads no
    // textures nor paints. Real CPU/GPU saving under a floating window
    // (Win7/DWM does not report occlusion: without this the app would keep
    // working at full tilt under the window, competing with the GMA 950).
    std::atomic<bool> covered{false};
    // Render surface size (physical px, dpr already applied), updated by
    // renderGL. frameToImage scales ONLY down to here: fewer pixels
    // in swscale and in the texture upload. 0 = no surface (full
    // resolution, as before). If the surface is larger than the frame, it stays
    // at full resolution (GL does the upscale).
    std::atomic<int> renderW{0};
    std::atomic<int> renderH{0};

    // current load (read by the thread on startup)
    QString url;
    QString audioUrl;
    double startAt = 0.0;
    QString reqHwdec; // HW device requested ("vaapi"/"dxva2") -> configuredHwdec()

    // ---- public snapshot (thread-safe queries) ----
    mutable QMutex snapMtx;
    struct Snap {
        double position = 0.0;
        double duration = 0.0;
        qint64 vw = 0;
        qint64 vh = 0;
        int maxVideoH = 0;
        bool cacheIdle = false;
        bool audioActive = false;
        QString codec;           // video codec ("h264"/"vp9"/"av1", …)
        QString hwdec;           // "no" (SW) / HW device name (Phase 4)
        int framesDecoded = 0;
    } snap;

    Snap readSnap() const
    {
        QMutexLocker l(&snapMtx);
        return snap;
    }

    // ---- latest frame (decode thread -> GL render) ----
    QMutex frameMtx;
    QImage latest;

    // ---- GL resources (GUI thread only, created lazily) ----
    QOpenGLShaderProgram *prog = nullptr;
    QOpenGLBuffer *vbo = nullptr;
    QOpenGLVertexArrayObject *vao = nullptr;
    GLuint tex = 0;
    int texW = 0;
    int texH = 0;
    bool glOk = false;
};

// ---------------------------------------------------------------------------
// Decode thread -> GUI events (kind of onEngineEvent).

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
// Audio sink: blocking write that paces the master clock (equivalent to mpv's
// sink). The variant is chosen per platform.

struct Sink
{
    virtual ~Sink() = default;
    // Opens an interleaved stereo S16 PCM output at `rate` Hz. false → no audio.
    virtual bool open(int rate) = 0;
    // Writes `frames` (stereo). Blocks according to the sink's pace.
    virtual void writeSamples(const int16_t *data, int frames) = 0;
    // Drop what is pending (seek): do not block.
    virtual void flush() = 0;
    // Pause/resume without losing buffers.
    virtual void cork(bool paused) = 0;
    // Wait for what was written to have played (end of file).
    virtual void drain() = 0;
    virtual void close() = 0;
    virtual bool opened() const = 0;
};

#if !defined(Q_OS_WIN) && defined(NEVERTUBE_HAS_PULSE)
// pulse-simple: the write blocks until the server accepts the data, so the
// loop stays paced by the real audio (spike recipe).
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
        // If the server died (m_failed), do not insist with syscalls: the loop
        // keeps counting samples and the video stays mute, WITHOUT falsely
        // triggering the app's audio watchdog (the track is still active in the
        // demux; it is the sink that does not sound).
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
        // pulse-simple has no cork (that is pa_stream of the full API);
        // pause = drop what is queued on the server: the audio freezes
        // instantly and on resume it is simply written again.
        if (m_pa && paused)
            pa_simple_flush(m_pa, nullptr);
    }

    void drain() override
    {
        // No pa_simple_drain: spin until the server latency is
        // emptied (what was written has already played) so as not to cut the tail at the end.
        if (!m_pa)
            return;
        int perr = 0;
        for (int i = 0; i < 200; ++i) {
            const pa_usec_t lat = pa_simple_get_latency(m_pa, &perr);
            if (perr < 0)
                break;
            if (lat <= 30000) // <= 30 ms pending -> practically played
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
// QAudioOutput (Qt Multimedia): the write does not block; the loop paces waiting
// for a gap in the sink buffer (equivalent to pulse's blocking write).
// It only enters the x86 build compilation (Phase 5); in this phase the Windows
// .cpp is not compiled yet.
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
        // ~0.8 s queue: margin so the loop does not stall with the sink's
        // pace.
        m_out->setBufferSize(rate * 4 / 5);
        m_dev = m_out->start();
        return m_dev != nullptr;
    }

    void writeSamples(const int16_t *data, int frames) override
    {
        if (!m_out || frames <= 0)
            return;
        // Device down (cable/sink): do not write, keep counting samples
        // (video mute but the track stays active for the app's watchdog).
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
        // start() starts the sink and returns the QIODevice: it only needs
        // to be called once (the previous duplicate call was dropped).
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
// No real sink (Linux without pulse-simple): there is no one to pace the write, so
// the sink itself sleeps the real time equivalent to the frames written.
// Only for development builds without pulse; the x86 real playback
// (Phase 5) carries its own sink (WASAPI/QAudioOutput).
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

// avformat interrupt callback: makes stop() cut network reads
// instantly (without it, av_read_frame can block for seconds).
static int interruptCb(void *ctx)
{
    const FfmpegBackend::Impl *m =
        static_cast<const FfmpegBackend::Impl *>(ctx);
    return m->stop.load() ? 1 : 0;
}

// Opens the container with the interrupt callback and, for non-local http
// URLs, with http_multiple (several ranges in parallel: YouTube's throttle
// is PER REQUEST, and a continuous download of /videoplayback is
// throttled to ~150 KB/s while several in parallel are not).
static bool openFormat(AVFormatContext **out, const QString &url, void *ictx)
{
    if (url.isEmpty())
        return false;
    *out = avformat_alloc_context();
    if (!*out) // OOM: with no context there is nowhere to attach the callback
        return false;
    (*out)->interrupt_callback = { interruptCb, ictx };
    AVDictionary *opts = nullptr;
    if (!isLocalStreamUrl(url))
        av_dict_set(&opts, "http_multiple", "1", 0);
    const int rc =
        avformat_open_input(out, url.toUtf8().constData(), nullptr, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        // We preallocate the context ourselves -> ownership stays
        // ours if avformat_open_input fails (it does not free it).
        if (*out)
            avformat_free_context(*out);
        *out = nullptr;
        return false;
    }
    return true;
}

// Does the frame come from a hardware surface (VAAPI/DXVA2)? Those cannot
// be passed to swscale directly: first they must be brought into memory.
static bool isHwPixelFormat(AVPixelFormat fmt)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(fmt);
    return d && (d->flags & AV_PIX_FMT_FLAG_HWACCEL);
}

// Does the codec expose a hwaccel usable with the device we created?
// (avcodec_get_hw_config reflects ONLY what is compiled in the package). If there is NO
// hwaccel, the hw_device_ctx is not attached — the decoder goes to PURE software:
//   - h264 (dxva2 in the i686 build) -> with device -> DXVA2 on the x86 target; if
//     the hardware does not support the hwaccel (a platform without h264 DXVA2), the
//     open with device FAILS and runLoop retries the decoder in PURE software;
//   - av1/vp9/aac/opus... (no compiled hwaccel) -> software. It is MANDATORY
//     for av1: FFmpeg 9's native decoder aborts the whole clip with ENOSYS
//     ("Your platform doesn't support hardware accelerated AV1 decoding",
//     av1dec.c) if it sees the hw_device_ctx but does not get a hwaccel, instead of
//     falling back to software like h264.
static bool codecSupportsHw(const AVCodec *codec, AVHWDeviceType type)
{
    for (int i = 0;; ++i) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
        if (!cfg)
            break;
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            cfg->device_type == type)
            return true;
    }
    return false;
}

// Opens a decoder from codecpar. The HW device is attached only if
// the codec has a hwaccel compiled for it (codecSupportsHw); if not, the
// chain is the same but in software (av1/vp9 on the x86 target, and the audio).
static AVCodecContext *openDecoder(const AVCodecParameters *par,
                                   AVBufferRef *hwDevice,
                                   AVHWDeviceType hwType)
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
    if (hwDevice && codecSupportsHw(codec, hwType)) {
        cc->hw_device_ctx = av_buffer_ref(hwDevice);
    } else {
        // SOFTWARE decoding (x86 target, h264 with a single thread by default):
        // h264/vp9/av1 parallelize by frame-threading. min(cores, 4) as
        // a cap so as not to oversubscribe 2-core CPUs. NOT touched with
        // hwDevice active (DXVA2/VAAPI already decodes; those hwaccels do not
        // mix with frame-threading).
        const int cores = qMax(1, QThread::idealThreadCount());
        cc->thread_count = qMin(cores, 4);
    }
    if (avcodec_open2(cc, codec, nullptr) < 0) {
        avcodec_free_context(&cc);
        return nullptr;
    }
    return cc;
}

// Maximum height among the container's VIDEO tracks (ignores attached
// posters like "attached_pic", e.g. the mjpeg of mp3s with cover art).
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

// Spike recipe: sws -> RGB24 into a reusable QImage.
struct SwsState
{
    SwsContext *ctx = nullptr;
    int w = 0;
    int h = 0;
    int tw = 0; // conversion target size (render surface); 0 = w/h
    int th = 0;
    AVPixelFormat fmt = AV_PIX_FMT_NONE;

    ~SwsState()
    {
        if (ctx)
            sws_freeContext(ctx);
    }
};

// Converts `f` to RGB24 into `img`. If maxW/maxH > 0 and smaller than the frame,
// scales ONLY down to that size (render surface, physical px)
// respecting the stream aspect: fewer pixels in swscale AND in the texture
// upload (the test machine's iGPU appreciates the GPU margin when DWM
// composites windows on top). If the surface is larger than the frame, it stays
// at full resolution (GL does the upscale, as before).
static void frameToImage(const AVFrame *f, QImage &img, SwsState &st,
                         int maxW, int maxH)
{
    const int w = f->width;
    const int h = f->height;
    const AVPixelFormat fmt = static_cast<AVPixelFormat>(f->format);
    int tw = w;
    int th = h;
    if (maxW > 0 && maxH > 0 && (maxW < w || maxH < h)) {
        const double scale =
            qMin(static_cast<double>(maxW) / w, static_cast<double>(maxH) / h);
        tw = qMax(2, static_cast<int>(w * scale));
        th = qMax(2, static_cast<int>(h * scale));
    }
    if (!st.ctx || st.w != w || st.h != h || st.fmt != fmt ||
        st.tw != tw || st.th != th) {
        if (st.ctx)
            sws_freeContext(st.ctx);
        st.ctx = sws_getContext(w, h, fmt, tw, th, AV_PIX_FMT_RGB24,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
        st.w = w;
        st.h = h;
        st.tw = tw;
        st.th = th;
        st.fmt = fmt;
    }
    if (!st.ctx) {
        img = QImage();
        return;
    }
    if (img.isNull() || img.width() != tw || img.height() != th)
        img = QImage(tw, th, QImage::Format_RGB888);
    uint8_t *dst[4] = { img.bits(), nullptr, nullptr, nullptr };
    int linesize[4] = { img.bytesPerLine(), 0, 0, 0 };
    sws_scale(st.ctx, f->data, f->linesize, 0, h, dst, linesize);
}

// Software volume gain (S16): 1.0 does not touch anything.
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

// Delivers an event to the GUI thread (deferred invocation -> onEngineEvent).
static void emitPost(FfmpegBackend *be, int kind, double a, double b, int c,
                     const QString &s)
{
    QMetaObject::invokeMethod(be, "onEngineEvent", Qt::QueuedConnection,
                              Q_ARG(int, kind), Q_ARG(double, a),
                              Q_ARG(double, b), Q_ARG(int, c),
                              Q_ARG(QString, s));
}

// Delivers a frame to the GUI thread (deferred invocation -> deliverFrame).
static void emitFrame(FfmpegBackend *be, const QImage &img)
{
    QMetaObject::invokeMethod(be, "deliverFrame", Qt::QueuedConnection,
                              Q_ARG(QImage, img));
}

// ---------------------------------------------------------------------------
// Decode thread

static void runLoop(FfmpegBackend::Impl *m)
{
    FfmpegBackend *be = m->owner;
    const bool bench = m->bench.load();

    // Containers and decoders (freed in cleanup).
    AVFormatContext *vFmt = nullptr;
    AVFormatContext *aFmt = nullptr; // audio externo (audioUrl)
    AVCodecContext *vc = nullptr;    // video
    AVCodecContext *ac = nullptr;    // audio (muxed from vFmt or external)
    bool extAudio = false;           // ac comes from aFmt
    int muxAudioIdx = -1;            // audio track muxed in vFmt (if ac lives there)
    int extAudioIdx = -1;
    // HW device (Phase 4): the reference is ALWAYS freed here (also on the
    // error paths after its creation). openDecoder does its own
    // av_buffer_ref(), so this unref does not touch the codec contexts.
    AVBufferRef *hwDevice = nullptr;

    auto cleanup = [&]() {
        if (ac)
            avcodec_free_context(&ac);
        if (vc)
            avcodec_free_context(&vc);
        if (aFmt)
            avformat_close_input(&aFmt);
        if (vFmt)
            avformat_close_input(&vFmt);
        if (hwDevice)
            av_buffer_unref(&hwDevice);
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
        fail(nt::tr("error.openUrl"));
        return;
    }
    if (avformat_find_stream_info(vFmt, nullptr) < 0) {
        fail(nt::tr("error.streamInfo"));
        return;
    }

    NT_DEBUG("E2E ffmpeg open <- %s (streams=%u dur=%.1fs)\n",
             m->url.toUtf8().constData(), vFmt->nb_streams,
             vFmt->duration > 0 ? vFmt->duration / 1e6 : 0.0);

    const int vs =
        av_find_best_stream(vFmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) {
        fail(nt::tr("error.noVideoTrack"));
        return;
    }

    // --- Phase 4: hardware acceleration device (if any) ---
    // Windows x86: DXVA2 (the H.264 of the x86 target iGPU). Linux: VAAPI.
    // If it cannot be created, it decodes in software (the flow decides).
    const char *hwName = nullptr;
    AVHWDeviceType hwType = AV_HWDEVICE_TYPE_NONE;
#if defined(Q_OS_WIN)
    hwName = "dxva2";
    hwType = AV_HWDEVICE_TYPE_DXVA2;
    if (av_hwdevice_ctx_create(&hwDevice, AV_HWDEVICE_TYPE_DXVA2, nullptr,
                               nullptr, 0) < 0)
        hwDevice = nullptr;
#else
    hwName = "vaapi";
    hwType = AV_HWDEVICE_TYPE_VAAPI;
    if (av_hwdevice_ctx_create(&hwDevice, AV_HWDEVICE_TYPE_VAAPI, nullptr,
                               nullptr, 0) < 0) {
        // No X11 display (headless/CI): try the default render node.
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

    vc = openDecoder(vFmt->streams[vs]->codecpar, hwDevice, hwType);
    if (!vc && hwDevice) {
        // The device was created (DXVA2/VAAPI) but the decoder does NOT start
        // WITH it: codecSupportsHw only reflects what is COMPILED in the package,
        // not what the hardware supports — e.g. DXVA2 of h264 on a
        // platform without hardware h264: avcodec_open2 with the attached
        // device fails. Without this retry in PURE SOFTWARE the whole clip
        // would die with "Sin decodificador de vídeo" (EvEndFile Error) instead
        // of falling back to SW. It never changes the av1 open (codecSupportsHw=no ->
        // direct SW, with no device to revert).
        NT_DEBUG("E2E ffmpeg: decoder HW no inicia (%s) -> reintentando SW\n",
                 hwName);
        vc = openDecoder(vFmt->streams[vs]->codecpar, nullptr,
                         AV_HWDEVICE_TYPE_NONE);
    }
    if (!vc) {
        fail(nt::tr("error.noDecoder")
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
        // hwdec is resolved with the FIRST processed frame (mpv keeps
        // hwdec-current empty until the decoder activates; the widget
        // retries with retryHwdec until it knows).
        m->snap.hwdec.clear();
        m->snap.cacheIdle = true; // no "cache" model: it can be touched now
    }

    // --- audio: external if it comes and opens; if not, the muxed one ---
    if (!m->audioUrl.isEmpty()) {
        if (openFormat(&aFmt, m->audioUrl, m) &&
            avformat_find_stream_info(aFmt, nullptr) >= 0) {
            extAudioIdx =
                av_find_best_stream(aFmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (extAudioIdx >= 0)
                ac = openDecoder(aFmt->streams[extAudioIdx]->codecpar,
                                 hwDevice, hwType);
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
            ac = openDecoder(vFmt->streams[muxAudioIdx]->codecpar, hwDevice,
                             hwType);
    }

    // Sink (real playback only; the bench decodes without sound).
    std::unique_ptr<Sink> sink;
    const bool hasAudio = ac != nullptr;
    if (hasAudio && !bench) {
        sink = makeSink();
        if (!sink->open(48000)) {
            NT_DEBUG("E2E ffmpeg: sin sink de audio (salida video-only)\n");
            sink.reset();
        }
    }
    if (ac) {
        // The channel count is read into a local BEFORE the macro call on
        // purpose: MSVC does NOT accept a preprocessor directive inside the
        // arguments of a macro (error C2121 "'#': invalid character: possibly
        // the result of a macro expansion"), while GCC accepts it happily. A
        // guard placed directly in the NT_DEBUG(...) argument list therefore
        // compiles on Linux and blows up ONLY on the x86 Windows build (the only
        // one with libav, so the only one that reaches these lines). Hoisting it
        // out of the call keeps both compilers and both builds happy.
#ifdef NT_FFMPEG_NEW_CHLAYOUT
        const int acChannels = ac->ch_layout.nb_channels;
#else
        const int acChannels = ac->channels;
#endif
        NT_DEBUG("E2E ffmpeg audio %dHz ch=%d src=%s\n", ac->sample_rate,
                 acChannels, extAudio ? "externa" : "muxed");
    }

    // Resampler to stereo S16 48 kHz.
    SwrContext *swr = nullptr;
    uint8_t *abuf = nullptr;
    int abufCap = 0;
    if (ac) {
        int rc = 0;
#ifdef NT_FFMPEG_NEW_CHLAYOUT
        AVChannelLayout outLayout{};
        av_channel_layout_default(&outLayout, 2);
        rc = swr_alloc_set_opts2(
            &swr, &outLayout, AV_SAMPLE_FMT_S16, 48000, &ac->ch_layout,
            ac->sample_fmt, ac->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&outLayout);
#else
        const int64_t inLayout =
            ac->channel_layout ? ac->channel_layout
                               : av_get_default_channel_layout(ac->channels);
        swr = swr_alloc_set_opts(
            nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 48000, inLayout,
            ac->sample_fmt, ac->sample_rate, 0, nullptr);
        rc = swr ? 0 : -1;
#endif
        if (rc < 0 || !swr) {
            NT_DEBUG("E2E ffmpeg: init del resampler falló; sin audio\n");
            ac = nullptr;
        }
    }

    // --- initial events (basic mpv order: loaded -> dur -> size) ---
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

    // --- master clock and helpers ---
    int64_t t0 = av_gettime_relative(); // rebased on the initial seek and later seeks
    int64_t pausedUs = 0;
    int64_t pauseStartUs = 0;
    bool wasPaused = false;
    int64_t audioSamples = 0; // stereo samples written (audio position)
    // AUDIO clock base: with an audio track, the master clock is the
    // audio itself (what has been written to the sink), NOT wall-time — so pauses,
    // seeks and video stalls do not desync HD: the video adjusts to the
    // track. Base = content position at the moment of (re)starting the
    // count (startAt / seek target), because audioSamples counts from 0
    // after each seek.
    double audioClockBase = m->startAt;
    double lastVideoPtsShown = 0.0;
    bool audioActiveOnce = false;
    int64_t lastPosEmitUs = 0;
    bool hwdecResolved = false; // HW/SW verdict set on the first frame

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

    // --- initial seek (resume position) ---
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

    // --- main loop ---
    int appliedSeekTick = 0;
    bool vReadDone = false; // video container read exhausted (EOF)
    bool aReadDone = false; // external audio read exhausted (EOF)
    bool draining = false;  // final drain: do NOT drop "future" frames

    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    AVFrame *hwScratch = av_frame_alloc(); // SW copy of the HW frame (copy-back)
    SwsState swsState;
    QImage outFrame;

    auto handleVideoFrame = [&](AVFrame *f, double tb) {
        const double wall = masterClock();
        const double pts =
            (f->pts == AV_NOPTS_VALUE) ? wall : static_cast<double>(f->pts) * tb;
        if (!draining && f->pts != AV_NOPTS_VALUE &&
            pts > wall + kFrameAheadToleranceSec)
            return; // too far in the future (the audio paces) -> drop
        if (!draining && ac && f->pts != AV_NOPTS_VALUE &&
            pts < wall - kFrameBehindToleranceSec)
            return; // too far BEHIND the audio: skip to catch up with the
                    // audio track (slow SW decode / external audio)
        if (!ac && !draining) {
            // no audio: the clock is the video itself; sleep until its pts.
            const double d = pts - wall;
            if (d > 0.002)
                av_usleep(static_cast<unsigned>(d * 1e6));
        }
        // Covered by another window (x86): do not transfer (copy-back) nor
        // convert nor emit — the frame is dropped without touching CPU/RAM. The
        // audio continues and the position is updated with the audio clock (without
        // this, the widget's stall watchdog would see the position frozen
        // > 5 s with the video covered and would fire a false streamStalled). The
        // decode stays warm; on uncover the most recent frame
        // is shown (frame-driven, no burst).
        if (m->covered.load()) {
            lastVideoPtsShown = pts;
            if (ac)
                setPosition(audioClockBase + audioPos(), false);
            return;
        }
        // Phase 4: if the frame comes from a HW surface (VAAPI/DXVA2),
        // bring it to memory (copy-back) before swscale; a failure here only
        // drops that frame.
        AVFrame *src = f;
        const bool hwFrame =
            isHwPixelFormat(static_cast<AVPixelFormat>(f->format));
        if (hwFrame) {
            av_frame_unref(hwScratch);
            if (av_hwframe_transfer_data(hwScratch, f, 0) < 0)
                return;
            src = hwScratch;
        }
        frameToImage(src, outFrame, swsState, m->renderW.load(),
                     m->renderH.load());
        if (outFrame.isNull())
            return;
        if (!bench)
            emitFrame(be, outFrame);
        if (!hwdecResolved) {
        // First processed frame: HW/SW verdict for the widget
        // (streamDecodeInfoReady) and the reactive codec ladder.
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
                swr_close(swr); // empties the resampler's internal state
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

        // ---- read from the video container (video + muxed audio) ----
        if (!vReadDone && !m->stop.load()) {
            const int r = av_read_frame(vFmt, pkt);
            if (r < 0) {
                if (r == AVERROR_EOF) {
                    vReadDone = true;
                } else if (r == AVERROR(EAGAIN)) {
                    // HLS live: wait a bit and continue.
                    av_usleep(100 * 1000);
                } else {
                    if (m->stop.load())
                        break;
                    char err[AV_ERROR_MAX_STRING_SIZE] = { 0 };
                    av_strerror(r, err, sizeof err);
                    fail(nt::tr("error.networkStream")
                             .arg(QString::fromUtf8(err)));
                    break;
                }
            } else {
                if (pkt->stream_index == vs) {
                    // decode video
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

        // ---- read from the external audio container ----
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
                    fail(nt::tr("error.externalAudio")
                             .arg(QString::fromUtf8(err)));
                    break;
                }
            } else {
                if (pkt->stream_index == extAudioIdx)
                    feedAudio(pkt);
                av_packet_unref(pkt);
            }
        }

        // ---- end: drain decoders and close with EOF ----
        if (vReadDone && (!extAudio || aReadDone)) {
            draining = true;

            // Drain the video (some frame may remain queued).
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
            // Drain the audio (muxed or external).
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

        // Short pause between cycles when the video runs faster than the
        // audio (avoids a hot loop in video-only with frames without pts).
        if (!ac && !vReadDone)
            QThread::usleep(2000);
    } // while (!m->stop.load())

    av_frame_free(&frame);
    av_frame_free(&hwScratch);
    av_packet_free(&pkt);
    av_free(abuf);
    if (swr)
        swr_free(&swr);
    cleanup();
}

// ---------------------------------------------------------------------------
// GL render (GUI thread only; paintGL calls renderGL with the current context).

// Compresses the render into a single static block to keep it all in view.
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

    // RGB888 texture (full upload only if the size changes).
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

    // Quad fitted to the frame aspect (letterbox: the widget is already sized
    // to the aspect by WatchPage, but 1 px roundings must not stretch).
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
    // v=0 top (first row of the QImage), v=1 bottom -> upright image.
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
    // The decode thread scales the conversion down to the surface size (only
    // downwards): tell it the current size on every paint.
    m->renderW.store(width);
    m->renderH.store(height);
    if (m->covered.load())
        return; // covered: decode no longer converts; do not upload textures nor paint
    QImage img;
    {
        QMutexLocker l(&m->frameMtx);
        img = m->latest;
    }
    renderGLCore(m, img, width, height);
}

void FfmpegBackend::setVideoOccluded(bool occluded)
{
    m->covered.store(occluded);
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
    // Reset of the pending seek: without this, a seek made on the previous video
    // (seekTick != 0) was reapplied when starting the next one, because runLoop
    // starts with appliedSeekTick=0 and the first cycle sees different ticks. The
    // old thread is already joined here, so there is no race with the reset.
    m->seekTick = 0;
    m->seekSec = 0.0;
    // HW device requested for configuredhwdec() (what is really used
    // is resolved on the first frame -> hwdecCurrent()).
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
        m->snap.hwdec.clear(); // unknown until the first frame (mpv-style)
        m->snap.cacheIdle = true;
    }
    {
        QMutexLocker l(&m->frameMtx);
        m->latest = QImage();
    }
    m->thread = std::thread([this] { runLoop(m); });
    // External audio is handled inside the load itself (like mpv with
    // "audio-files"): we return true so the widget drops the fallback.
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
    // Volume arrives in % (0..100, possibly capped up to 200). 1.0 = 100%.
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
    // Device requested for the next load; the one really used after the
    // first frame is hwdecCurrent() (it may end up in SW if there is no device).
    return m->reqHwdec.isEmpty() ? QStringLiteral("no") : m->reqHwdec;
}

QString FfmpegBackend::hwdecCurrent() const { return m->readSnap().hwdec; }

QString FfmpegBackend::videoCodec() const { return m->readSnap().codec; }

int FfmpegBackend::probeMaxVideoHeight() const
{
    return m->readSnap().maxVideoH;
}

// The state of the audio (the same probe as the mpv one: how many tracks,
// how many of them are audio, which one is selected and its format).
void FfmpegBackend::probeAudioState(qint64 *tracks, qint64 *audioTracks,
                                    qint64 *selectedAudio,
                                    QString *format) const
{
    if (tracks)
        *tracks = -1;
    if (audioTracks)
        *audioTracks = -1;
    if (selectedAudio)
        *selectedAudio = -1;
    if (format)
        format->clear();
    if (tracks)
        *tracks = -1;
    if (audioTracks)
        *audioTracks = m ? (m->readSnap().audioActive ? 1 : 0) : -1;
}

// The FfmpegBackend has a single audio track and it is always selected:
// there is nothing to select.
bool FfmpegBackend::ensureAudioSelected()
{
    return false;
}

bool FfmpegBackend::hasActiveAudio() const
{
    return m->readSnap().audioActive;
}

double FfmpegBackend::cacheDuration() const
{
    // This backend has no measurable "cache" model: the demuxer reads directly
    // from the socket and keeps no buffer of its own (cacheIdle is always true).
    // 0.0 is returned as mpv with an empty buffer; the app's watchdog does not
    // depend on this value in this engine.
    return 0.0;
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