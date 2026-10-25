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

// Builds an mpv_node of string type with the given value. The buffer is
// allocated by the caller and freed with mpvNodeFreeStr().
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

// loadfile with the per-file "audio-files" option: the video and the external
// audio are opened and synced in the same load. Without this mpv starts with
// video only and the audio joins afterwards (initial muted video + re-sync
// dropfps; and on re-resolutions the audio may never arrive). Returns the
// command's result code.
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
    // mpv interprets audio-files as a list of strings separated by ':';
    // an https://... URL gets split into chunks (https, //host, etc.). We escape
    // each ':' with '\:' so the value arrives intact as a single entry.
    QByteArray audioEsc = audioUrl.toUtf8();
    audioEsc.replace(':', "\\:");
    mpvNodeSetStr(&optv[0], audioEsc);

    // Modern signature (mpv >= 0.38): loadfile url replace index=<int> options.
    // Old signature (mpv 0.33-0.37, e.g. the Windows bundle): without the
    // "index" argument, the options go in the fourth position. The modern one
    // is tried first and, if that mpv does not understand it
    // (INVALID_PARAMETER), it is retried with the old one.
    int r = MPV_ERROR_INVALID_PARAMETER;
    for (int pass = 0; pass < 2 && r == MPV_ERROR_INVALID_PARAMETER; ++pass) {
        if (pass == 0) {
            argList.num = 5;
            argList.values = argv;
            argv[3].format = MPV_FORMAT_INT64; // index (ignored with replace)
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
    // vd=v: the decoder's real verdict ("Using hardware/software decoding") is
    // verbose and it is what validates the profile's safety net.
    mpv_set_option_string(m_mpv, "message-level",
                          "all=warn,vo=info,vd=v,cache=info,stream=info,network=info");
    mpv_set_option_string(m_mpv, "cache", "yes");
    // Demux buffer (Chrome-style, contained size): 96 MiB to absorb the
    // software decoder's peaks and YouTube's download throttle without
    // spiking the RAM (150 MiB was mpv's default, too much for a weak/x64
    // machine; the original 64 MiB fell short during startup micro-pauses).
    // With the cache enabled this cap is also the REAL readahead limit:
    // cache-secs defaults to "very high" and demuxer-readahead-secs stays
    // "mostly ignored" (mpv manual), so there is no need to fix a readahead
    // in seconds.
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "96MiB");
    // Startup cushioning: without this, with YouTube's throttle the first ten
    // seconds play "dry" and mpv micro-pauses at ~6 and ~10 s until the
    // parallel download (http_multiple) stabilizes.
    mpv_set_option_string(m_mpv, "cache-initial", "3MiB");
    // SOFTWARE decoder threads (vd=lavc -> h264/vp9/av1 parallelize via
    // frame-threading). Ideal = core count capped at 4: going higher on
    // 2-core CPUs is thread oversubscription. With hwdec active it does not
    // take part (the HW decodes, not lavc).
    const int cores = qMax(1, QThread::idealThreadCount());
    const QByteArray lavcThreads = QByteArray::number(qMin(cores, 4));
    mpv_set_option_string(m_mpv, "vd-lavc-threads", lavcThreads.constData());
    mpv_set_option_string(m_mpv, "audio-buffer", "2.0");

    if (mpv_initialize(m_mpv) < 0) {
        qWarning("player: mpv_initialize fallo");
        // Useless handle: nulling it makes isValid() return false and no later
        // command touches a dead mpv.
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
    // Diagnostics of the real GL context (ANGLE / desktop GL experiment):
    // vendor "Google ... (ANGLE ...)" + es=1 confirm that the widget's context
    // is ANGLE and that mpv will be able to attempt the zero-copy d3d11-egl
    // hwdec with hwdec=auto (in video/out/gpu/hwdec.c d3d11-egl comes BEFORE
    // d3d11va; with desktop GL eglGetCurrentDisplay() is NULL and only
    // d3d11va-copy remains, the CPU-to-100% copy-back on x64).
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
    // The CANONICAL flow of the libmpv render API: before rendering, the
    // app asks mpv what there is to draw (mpv_render_context_update) and
    // the update callback has to be followed by that call.
    //
    // The app did NOT call it ever, and mpv complained about it in every
    // session ("mpv_render_context_render() not being called or stuck",
    // 235 times in two minutes). The symptom of the broken handshake is
    // that a surface that was hidden and comes back (the fullscreen that
    // ends) stays BLACK until the next frame arrives.
    //
    // It is called HERE and not in the update callback because the doc says
    // so: "this has to happen on the render thread, and NOT from the update
    // callback itself".
    const uint64_t flags = mpv_render_context_update(m_rctx);
    m_framePending = (flags & MPV_RENDER_UPDATE_FRAME) != 0;
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
    // Audio language preference ("alang"): only with an explicit preference
    // does mpv pick a dub (even if it has DEFAULT=NO) instead of the track
    // marked DEFAULT=YES by YouTube (normally the original audio).
    const QByteArray lang = m_audioLang.toUtf8();
    mpv_set_option_string(m_mpv, "alang", lang.constData());
    // YouTube strangles the continuous download of the direct URL to
    // ~initcwndbps (~1.4 Mbps), not enough for 1080p: buffering pauses at
    // 6-10 s. ffmpeg downloads ranges in parallel ("http_multiple") and the
    // short Range requests are not strangled (35 MB/s) -> it defeats the
    // throttle. Only for external URLs: the local HLS master does not support
    // Range requests.
    if (isLocalStreamUrl(url))
        mpv_set_option_string(m_mpv, "stream-lavf-o", "");
    else
        mpv_set_option_string(m_mpv, "stream-lavf-o", "http_multiple=1");
    const QByteArray u = url.toUtf8();
    // "start" ALWAYS with a concrete value: mpv ignores/reserves the previous
    // value if an empty string is passed, and a resumed video (start=X) would
    // leave that X as the output for ALL the following ones (every video in
    // the queue would start at the first one's position). Absolute value,
    // without the "+" prefix.
    const QByteArray opt = QByteArray::number(startAt > 0.0 ? startAt : 0.0, 'f', 2);
    mpv_set_option_string(m_mpv, "start", opt.constData());
    if (startAt > 0.0)
        NT_DEBUG("E2E playUrl start=%s\n", opt.constData());
    const char *cmd[] = { "loadfile", u.constData(), nullptr };
    // Every new video starts playing: mpv keeps the pause state across
    // loadfile, and a previously paused video left the next one paused
    // ("video I click starts paused").
    int paused = 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &paused);
    NT_DEBUG("E2E playUrl len=%d http=%d audio=%d\n", int(u.size()),
             url.startsWith(QLatin1String("http")) ? 1 : 0,
             int(!audioUrl.isEmpty()));
    // With external audio (edge/AV1-HLS) the video and the audio are opened
    // in the SAME loadfile (per-file "audio-files" option): mpv syncs them
    // from the start, without the initial muted video and the later re-sync
    // dropfps. If the command fails (old mpv without the option), the plain
    // loadfile is kept and FILE_LOADED will do the previous audio-add.
    if (!audioUrl.isEmpty()) {
        const int r = loadFileWithAudio(m_mpv, u, audioUrl);
        if (r >= 0) {
            return true; // audio attached: the caller can discard the URL
        }
        NT_DEBUG("E2E loadfile+audio-files fallo (%s); fallback audio-add\n",
                 mpv_error_string(r));
        mpv_command_async(m_mpv, 0, cmd);
    } else {
        mpv_command_async(m_mpv, 0, cmd);
    }
    return false; // loaded without audio: the caller keeps the pending URL
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
    // audio-add select: adds the track and selects it as the current audio.
    const char *cmd[] = { "audio-add", u.constData(), "select", nullptr };
    const int r = mpv_command(m_mpv, cmd);
    m_audioAdded = r >= 0;
    // Any old track (leftover audio of a previous video) that mpv kept is
    // discarded if there was a replacement: each loadfile must only have its
    // own audio.
    NT_DEBUG("E2E audio-add res=%d len=%d\n", r, int(u.size()));
    return r;
}

void MpvBackend::dropExternalAudio()
{
    if (!m_mpv)
        return;
    // Classic flow: nothing to add; it also clears the external track
    // of a previous video with an audio-file lingering in mpv. The rc is
    // ignored: MPV_ERROR_COMMAND (no track to remove) is not a failure for us.
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

// true if the engine has an active audio track ("audio-params"). With the
// external audio (audio-files) rejected by YouTube (403), the engine plays
// the video without any audio track; "audio-params" does not exist / stays
// empty.
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

// The state of the audio of mpv: how many tracks there are, how many of
// them are audio, which one is selected and its format.
//
// It is the diagnosis of the "the first opening has no audio": with the
// counts, it is clear whether the external track arrived at all (audio=0)
// or whether it arrived and was not selected (audio=1, sel=-1).
void MpvBackend::probeAudioState(qint64 *tracks, qint64 *audioTracks,
                                 qint64 *selectedAudio, QString *format) const
{
    if (tracks)
        *tracks = -1;
    if (audioTracks)
        *audioTracks = -1;
    if (selectedAudio)
        *selectedAudio = -1;
    if (format)
        format->clear();
    if (!m_mpv)
        return;
    int64_t n = 0;
    if (mpv_get_property(m_mpv, "track-list/count", MPV_FORMAT_INT64, &n) >= 0
        && tracks)
        *tracks = n;
    int64_t na = 0;
    if (mpv_get_property(m_mpv, "current-tracks/audio", MPV_FORMAT_INT64, &na) >= 0
        && audioTracks)
        *audioTracks = na;
    int64_t sel = -1;
    if (mpv_get_property(m_mpv, "aid", MPV_FORMAT_INT64, &sel) >= 0
        && selectedAudio)
        *selectedAudio = sel;
    if (format) {
        char *f = nullptr;
        if (mpv_get_property(m_mpv, "audio-params/format", MPV_FORMAT_STRING, &f) >= 0
            && f) {
            *format = QString::fromUtf8(f);
            mpv_free(f);
        }
    }
}

// Selects the audio track when there is one and it is not selected.
//
// The case of the external audio (the "edge" mode): the track arrives (the
// "audio-files" of the loadfile) but mpv does not always select it, and the
// video plays mute. Re-resolving the whole stream (what the app did
// before) throws away the URLs that were being downloaded and it is slow
// and noisy; selecting the track that is already there is immediate.
//
// It returns true when it selected one.
bool MpvBackend::ensureAudioSelected()
{
    if (!m_mpv)
        return false;
    // The id of the selected audio track ("aid"). -1 = none.
    int64_t sel = -1;
    mpv_get_property(m_mpv, "aid", MPV_FORMAT_INT64, &sel);
    // The list of the tracks: the audio ones (the property "track-list" is
    // a list of maps; the "audio-files" of the loadfile adds one).
    int64_t count = 0;
    if (mpv_get_property(m_mpv, "track-list/count", MPV_FORMAT_INT64, &count) < 0)
        return false;
    for (int64_t i = 0; i < count; ++i) {
        const QByteArray path = "track-list/" + QByteArray::number(static_cast<int>(i)) + "/type";
        char *type = nullptr;
        if (mpv_get_property(m_mpv, path.constData(), MPV_FORMAT_STRING, &type) < 0
            || !type)
            continue;
        const bool isAudio = QByteArray(type) == QByteArrayLiteral("audio");
        mpv_free(type);
        if (!isAudio)
            continue;
        const QByteArray idPath =
            "track-list/" + QByteArray::number(static_cast<int>(i)) + "/id";
        int64_t id = -1;
        if (mpv_get_property(m_mpv, idPath.constData(), MPV_FORMAT_INT64, &id) < 0)
            continue;
        if (id == sel)
            return false;   // it is already selected
        // The track is there and it is not the selected one: select it.
        mpv_set_property(m_mpv, "aid", MPV_FORMAT_INT64, &id);
        NT_DEBUG("E2E audio seleccion=%lld (venia %lld)\n",
                 static_cast<long long>(id), static_cast<long long>(sel));
        return true;
    }
    return false;
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
            // Decoder verdict and raw engine errors.
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
    // The update callback of libmpv: mpv says "there is something new to
    // draw" and the app has to repaint. The repaint is ALWAYS asked for
    // (not only when a flag says so): the callback is the only thing that
    // wakes the app up, and a repaint that depends on a flag can leave the
    // surface black (the cycle stops and nothing restarts it).
    QMetaObject::invokeMethod(self, "deliverFrameReady", Qt::QueuedConnection);
}