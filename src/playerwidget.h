#ifndef PLAYERWIDGET_H
#define PLAYERWIDGET_H

#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QTimer>

#include "playback_backend.h"

// Video surface + playback policy. All access to the engine goes through the
// PlaybackBackend interface (at this stage, MpvBackend; the x86 with FFmpeg
// comes in later phases): the widget does NOT know mpv. Here live the state
// (position/duration/state), the stall and gated-session watchdogs
// (checkStall), the codec-profile safety net (hwdec probe/retry and
// log-based correction), and the initial high-resolution buffer wait — which
// is the protected piece (QOpenGLWidget, event-driven redraw: the engine
// notifies frameReady when there is a new frame and onFrameReady forces
// QWidget::update(); direct render to the FBO in paintGL).
class PlayerWidget : public QOpenGLWidget
{
    Q_OBJECT
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)

public:
    explicit PlayerWidget(QWidget *parent = nullptr);
    ~PlayerWidget() override;

    double position() const { return m_position; }
    double duration() const { return m_duration; }
    QString state() const { return m_state; }
    double speed() const;

    // Engine hardware decoding setting (mpv "hwdec");
    // verified as "auto" by --selftest-player.
    QString hwdec() const;

    // Read-only access for the --selftest-player selftest: true when the
    // engine's render context was created (initializeGL ran). In mpv that
    // same initGL installs the update callback, so it implies the
    // event-driven redraw path is active (no poll timer).
    bool renderContextReady() const { return m_backend && m_backend->hasRenderContext(); }
    // Hardware decoding backend the engine is really using for the current
    // stream, or "no"/empty if it decodes in software.
    QString hwdecCurrent() const;
    // Codec the engine is really decoding for the current stream
    // ("h264", "vp9", "av1", …).
    QString videoCodec() const;

public slots:
    void playUrl(const QString &url, double startAt = 0.0);
    void playYtdl(const QString &watchUrl, int maxHeight);
    void stop();
    void setPaused(bool paused);
    void seek(double seconds);
    void setVolume(double volume);
    // Engine volume ceiling (mpv "volume-max"). Default 100; the
    // "Volumen máximo" setting in Ajustes can raise it to 150/175/200 for
    // headroom.
    void setVolumeMax(double maxPercent);
    void setSubVisibility(bool visible);
    void setAudioLanguage(const QString &lang);
    // For VP9/webm: attach an external audio track (opus webm direct URL).
    // Stores in m_pendingAudioFile and applies it with the "audio-file-add"
    // command right after MPV_EVENT_FILE_LOADED (in mpv 0.41 the --audio-file
    // option is CLI-only and set_option_string fails with OPTION_NOT_FOUND).
    void setAudioFile(const QString &url);
    // Initial buffer wait when starting high-resolution streams (>=1080p):
    // starts paused and only resumes once the demuxer has accumulated `secs`
    // seconds of content (demuxer-cache-duration) according to the real
    // download (QTimer sampling launched on video-params/h). The target scales
    // with resolution (1440p/2160p ask for more margin). It is tolerated with
    // a timeout and aborted if the network falls behind the consumption. secs in
    // seconds; 0 = off.
    void setInitialBufferSecs(double secs);

signals:
    void positionChanged();
    void durationChanged();
    void stateChanged();
    void mediaError(const QString &message);
    void videoAspectChanged(double aspect);
    void videoSizeChanged(int width, int height);
    // Emitted when each real stream starts decoding: the codec and the hardware
    // decoding backend the engine really uses (hwdec-current,
    // e.g. "vaapi"/"vulkan-copy"; "no"/empty = software). MainWindow uses it
    // as the safety net for the codec profile detected by HwProbe: if the
    // codec the profile treats as HW falls back to software, the profile is downgraded.
    void streamDecodeInfoReady(const QString &codec, const QString &hcdecBackend);
    // When a stream finishes loading, the tallest height (pixels)
    // available among its video tracks is emitted. WatchPage uses it to filter the
    // resolution menu: hide the items above what exists for
    // that video (avoids offering "1080p" on a 720p clip). 0 if it could not
    // be determined (in that case the menu stays complete).
    void qualityLimitsChanged(int maxVideoHeight);
    // The position stalled for > 5 s while playing (no user pause nor
    // initial buffer). The controller re-resolves with fresh URLs and resumes.
    void streamStalled();
    // Playback interruption other than a position stall:
    // premature EOF (position << video duration) or external audio track
    // that never opened (e.g. YouTube 403 on the session's URLs: the
    // video advances silently until the segments die too). The
    // controller re-resolves with fresh URLs and resumes at the position.
    void streamInterrupted();

protected:
    void initializeGL() override;
    void paintGL() override;

private slots:
    // Backend ::frameReady: there is a new frame (or state change) to paint.
    void onFrameReady();
    // Backend ::fileLoaded: the main file finished loading.
    void onFileLoaded();
    // Backend ::streamStart: a new stream starts (re-arm the probe).
    void onStreamStart();
    // Backend ::endFile: end of file with its reason and error message.
    void onEndFile(PlaybackBackend::EndReason reason, const QString &errorText);
    void onPositionUpdated(double seconds);
    void onDurationUpdated(double seconds);
    void onVideoWidth(qint64 width);
    void onVideoHeight(qint64 height);
    void onPauseUpdated(bool paused);
    void onCacheTime(double seconds);
    void onLogMessage(const QString &prefix, const QString &text);
    void checkInitialBuffer();
    void checkStall();
    void retryHwdec();
    // x86 (FfmpegBackend): detects whether another window fully covers the video
    // (slow timer; Win7/DWM gives no occlusion notice). While covered, the
    // engine stops converting and painting (real CPU/GPU saving).
    void checkVideoCovered();

private:
    void emitAspectIfReady();
    // Walk of the Windows z-order: true if a visible, non-transparent
    // window (above our top-level) covers the video rect.
    // Windows only; not used on Linux (the FfmpegBackend never skips).
    bool occludedByTopWindow();

    PlaybackBackend *m_backend = nullptr;
    double m_position = 0.0;
    double m_duration = 0.0;
    QString m_state;
    int64_t m_videoW = 0;
    int64_t m_videoH = 0;
    // Tallest height among the video tracks of the loaded file
    // (track-list). 0 if not read yet. Consumed by WatchPage to
    // filter the resolution menu.
    int m_videoMaxH = 0;
    double m_aspect = 0.0;
    // Pending external audio URL to attach (edge/AV1-HLS): consumed on the
    // next playUrl (audio-files) or, as a fallback, after the file is
    // loaded (attachAudio in fileLoaded).
    QString m_pendingAudioFile;
    bool m_hwdecProbed = false;
    // Verdict from the engine's log for the current stream: the engine explicitly
    // says "Using software decoding" even if hwdec-current reports a
    // backend (software devices on old GPUs). It is reset per
    // stream.
    bool m_hwdecLogSoftware = false;
    // hwdec-current arrives empty until the engine activates the decoder; this
    // timer retries so the profile safety net receives a
    // resolved backend already on the first video.
    QTimer *m_hwdecRetry = nullptr;
    int m_hwdecRetryAttempts = 0;
    // Initial buffer wait (seconds of content), active for >=1080p.
    double m_initialBufferSecs = 0.0;
    QTimer *m_bufferTimer = nullptr;
    QElapsedTimer m_bufferStart;
    // Stall detection: position frozen while playing (see streamStalled).
    QTimer *m_stallTimer = nullptr;
    qint64 m_lastProgressMs = 0;
    bool m_paused = false;
    // "Video without data" detection: with external audio the position advances
    // even if the video track never loads (the previous video's last
    // frame stays). m_cacheOk becomes true when the demuxer caches content.
    qint64 m_videoLoadMs = 0;
    bool m_cacheOk = false;
    // The clock of the audio (its own: the one of the video is reset by the
    // check of the video that does not load, and with a shared clock the two
    // watchdogs stole each other's time).
    qint64 m_audioLoadMs = 0;
    // Whether the state of the audio was already reported (once per
    // stream): it is the diagnosis of the "the first opening has no audio".
    bool m_audioProbed = false;
    // x86: video covered by another window. Slow 250 ms timer +
    // z-order walk; only FfmpegBackend (occlusionSkipSupported).
    // On mpv/x64 this does NOT apply (stopping the render accumulates frames).
    QTimer *m_occlusionTimer = nullptr;
    bool m_videoOccluded = false;
};

#endif