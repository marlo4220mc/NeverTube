#pragma once

#include <QObject>
#include <QString>

// Shared by MpvBackend and FfmpegBackend: is the URL the local HLS server
// of the app itself (http://127.0.0.1:<port>/... or http://localhost)?
// Both engines treat it differently from a remote network URL (the local HLS
// does not support Range requests: no http_multiple, localhost timings).
// Compare by PREFIX ONLY: a "contains" would match remote URLs that carry
// "localhost"/"127.0.0.1" somewhere in the path.
inline bool isLocalStreamUrl(const QString &url)
{
    return url.startsWith(QLatin1String("http://127.0.0.1"))
        || url.startsWith(QLatin1String("http://localhost"));
}

// Engine-neutral playback interface. PlayerWidget (policy: state,
// watchdog, codec ladder, buffer wait) and its clients
// (PlayerController, MainWindow, WatchPage) depend on this interface, NOT on the
// concrete engine. Each implementation translates this engine-agnostic API to its
// native engine:
//   - MpvBackend  (x64, protected piece): all the mpv code that used to live
//     in PlayerWidget, moved with no behavior change.
//   - FfmpegBackend (x86, Win7, later phases): FFmpeg/libav
//     (schannel + DXVA2 h264) without libmpv.
//
// The signals are emitted from the widget thread (GUI). The widget connects
// to them with a direct connection (same thread) to preserve the engine's
// event processing order.

class PlaybackBackend : public QObject
{
    Q_OBJECT

public:
    // End-of-file reason, translated from whatever the engine reports.
    enum class EndReason {
        Quit,  // the engine shut down
        Stop,  // intentional stop (stop() / file replacement)
        Error, // playback error (the message travels in endFile)
        Eof,   // the file ended (natural end or premature EOF)
    };

    explicit PlaybackBackend(QObject *parent = nullptr) : QObject(parent) {}
    ~PlaybackBackend() override = default;

    // --- lifecycle ---

    // Creates the engine's render context. Must be called with the current GL
    // context (from PlayerWidget::initializeGL). Returns true if it ended up ready.
    virtual bool initGL() = 0;
    // Releases the render context (current GL context). Called BEFORE
    // shutdown() in the widget's destructor (the widget does makeCurrent()).
    virtual void destroyRenderContext() = 0;
    // Destroys the engine (releases its handle). Called AFTER
    // destroyRenderContext(). No-op if already destroyed.
    virtual void shutdown() = 0;
    // true if the engine was created and accepts commands (do NOT confuse with render
    // ready: before initializeGL there is no render context).
    virtual bool isValid() const = 0;
    // true if the render context exists (ready for renderGL()).
    virtual bool hasRenderContext() const = 0;
    // Engine render context pointer, FOR debug logging ONLY.
    virtual void *renderContextHandle() const = 0;

    // --- render ---

    // Renders the current frame into the given FBO. Called from paintGL with the
    // current GL context; width/height in physical pixels (already dpr-scaled).
    virtual void renderGL(int fbo, int width, int height) = 0;

    // --- commands ---

    // Loads and plays `url` (with optional resume at startAt). If
    // audioUrl is not empty, tries to open external video+audio together in the
    // same load (equivalent to the per-file "audio-files" option); returns
    // true if the audio was attached during the load (the caller may discard
    // audioUrl) and false if it was loaded without it (the caller keeps audioUrl to
    // attach it after the file is loaded, as a fallback).
    virtual bool playUrl(const QString &url, double startAt,
                         const QString &audioUrl) = 0;
    // ytdl mode (nothing to do in engines that resolve the URLs
    // themselves): the caller keeps using playUrl() afterwards.
    virtual void setYtdlFormat(const QString &format) = 0;
    virtual void stop() = 0;
    virtual void setPaused(bool paused) = 0;
    virtual void seek(double seconds) = 0;
    virtual void setVolume(double volume) = 0;
    // Engine volume ceiling (mpv "volume-max"). Default 100; the
    // "Maximum volume" setting can raise it to 150/175/200 for headroom.
    virtual void setVolumeMax(double maxPercent) = 0;
    virtual void setSubVisibility(bool visible) = 0;
    // Audio language preference ("alang"): re-applied before every
    // playUrl so the engine picks the right dub in each load.
    virtual void setAudioLanguage(const QString &lang) = 0;
    // Attaches an external audio track to the ALREADY loaded file (fallback
    // when playUrl did not consume the audio). Returns the engine code
    // (>= 0 on success, like mpv's 0).
    virtual int attachAudio(const QString &url) = 0;
    // Drops the manually attached external audio track.
    virtual void dropExternalAudio() = 0;

    // --- queries ---

    virtual double speed() const = 0;
    // Hardware decoding configured in the engine (mpv "hwdec",
    // e.g. "auto"). The player selftest verifies it.
    virtual QString configuredHwdec() const = 0;
    // Hardware decoding backend the engine ACTUALLY uses for the current
    // stream, or "no"/empty if it decodes in software.
    virtual QString hwdecCurrent() const = 0;
    // Codec the engine is really decoding for the current stream
    // ("h264", "vp9", "av1", ...).
    virtual QString videoCodec() const = 0;
    // Tallest height among the video tracks of the loaded file, or 0 if not
    // loaded yet or unknown (WatchPage filters the resolution menu with it).
    virtual int probeMaxVideoHeight() const = 0;
    // true if there is an active audio track in the current stream (gated-session
    // watchdog: without it the video advances muted).
    virtual bool hasActiveAudio() const = 0;
    // Seconds of content cached by the demuxer (high-resolution initial
    // buffer wait).
    virtual double cacheDuration() const = 0;
    // true if the download finished (everything downloadable is cached).
    virtual bool cacheIdle() const = 0;

    // Drains the engine's event queue and emits the matching signals.
    // Called on the widget thread (from frameReady and from the engine's deferred
    // wakeup).
    virtual void pollEvents() = 0;

    // --- window occlusion (x86 backend only) ---
    // true if the engine can pause its work while another window fully covers the
    // video area. FfmpegBackend YES: stopping copy-back+swscale and
    // render under a floating window is safe (frame-driven, no burst on
    // uncover) and saves real CPU/GPU on Win7, where DWM gives no occlusion
    // notice and the app would keep painting frames nobody sees (symptom: FPS drops
    // with low app CPU/RAM on the GMA 950). MpvBackend NO (stopping the
    // render accumulates frames and produces the "jump on return" — see AGENTS.md
    // "Mini player con vídeo en vivo"): the default is false.
    virtual bool occlusionSkipSupported() const { return false; }
    // true while another window covers the video (detected by the widget with a
    // slow 250 ms timer). No-op if occlusionSkipSupported() is false.
    virtual void setVideoOccluded(bool occluded) { Q_UNUSED(occluded); }

signals:
    // The main file finished loading: time to query the maximum height
    // (probeMaxVideoHeight) and to attach the fallback audio.
    void fileLoaded();
    // A new stream starts (START_FILE / playback restart): the
    // codec gets a chance to report HW/SW again (codec-profile safety net).
    void streamStart();
    // End of file with its reason. For Error, errorText carries the message.
    void endFile(PlaybackBackend::EndReason reason, const QString &errorText);
    // Playback position (seconds).
    void positionUpdated(double seconds);
    // Total stream duration (seconds).
    void durationUpdated(double seconds);
    void videoWidthUpdated(qint64 width);
    void videoHeightUpdated(qint64 height);
    // Pause state (changed by command, by the user or by the engine).
    void pauseUpdated(bool paused);
    // Seconds cached by the demuxer (video download health).
    void cacheTimeUpdated(double seconds);
    // Engine log line ("software/hardware decoding"
    // verdict, network errors, ...). Prefix and text are the engine's.
    void logMessage(const QString &prefix, const QString &text);
    // Emitted by the engine when there is a new frame to paint (video or state
    // change): the widget does pollEvents() + update().
    void frameReady();
};

// Creates the default engine for the current platform: mpv on x64 (protected
// piece) and, when the build enables it (NEVERTUBE_HAS_FFMPEG_BACKEND),
// FfmpegBackend on Windows x86 (Win7). For development and
// verification it can be forced with NEVERTUBE_BACKEND=mpv|ffmpeg.
PlaybackBackend *createPlaybackBackend(QObject *parent);