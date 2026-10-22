#pragma once

#include "playback_backend.h"

#include <QString>

#include <atomic>

#include <mpv/client.h>
#include <mpv/render.h>

// mpv backend: the only PlaybackBackend implementation at this stage (on
// x64; the x86 with FFmpeg arrives in later stages). Contains ALL the mpv
// integration code that used to live in PlayerWidget, moved WITHOUT behavior
// changes: startup options (vo=libmpv, hwdec=auto, …), property observation,
// wakeup/update callbacks, handleEvent (translated to neutral signals),
// loadfile with the per-file "audio-files" option, direct render to the FBO
// and queries (track-list, audio-params, cache, …).
class MpvBackend : public PlaybackBackend
{
    Q_OBJECT

public:
    explicit MpvBackend(QObject *parent = nullptr);
    ~MpvBackend() override;

    // --- lifecycle / render ---
    bool initGL() override;
    void destroyRenderContext() override;
    void shutdown() override;
    bool isValid() const override { return m_mpv != nullptr; }
    bool hasRenderContext() const override { return m_rctx != nullptr; }
    void *renderContextHandle() const override { return m_rctx; }
    void renderGL(int fbo, int width, int height) override;

    // --- commands ---
    bool playUrl(const QString &url, double startAt,
                 const QString &audioUrl) override;
    void stop() override;
    void setPaused(bool paused) override;
    void seek(double seconds) override;
    void setVolume(double volume) override;
    void setVolumeMax(double maxPercent) override;
    void setSubVisibility(bool visible) override;
    void setAudioLanguage(const QString &lang) override;
    int attachAudio(const QString &url) override;
    void dropExternalAudio() override;

    // --- queries ---
    double speed() const override;
    QString configuredHwdec() const override { return m_hwdec; }
    QString hwdecCurrent() const override;
    QString videoCodec() const override;
    int probeMaxVideoHeight() const override;
    bool hasActiveAudio() const override;
    void probeAudioState(qint64 *tracks, qint64 *audioTracks,
                         qint64 *selectedAudio, QString *format) const override;
    bool ensureAudioSelected() override;
    double cacheDuration() const override;
    bool cacheIdle() const override;

    // Drains the mpv event queue and emits the corresponding signals.
    void pollEvents() override;

private slots:
    // Deferred invocation from mpv's wakeup callback.
    void drainEvents();
    // Deferred invocation from mpv's update callback (new frame).
    void deliverFrameReady();

private:
    static void mpvWakeup(void *ctx);
    static void mpvUpdate(void *ctx);
    // Whether mpv says there is a frame to draw (it is asked with
    // mpv_render_context_update in the render thread). It is written in the
    // render thread and read in the callback's.
    std::atomic<bool> m_framePending{false};
    void handleEvent(mpv_event *event);

    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_rctx = nullptr;
    // Counter of logged render frames (every 90, as before).
    int m_logFrames = 0;
    // Hardware decoding configured ("auto"), read at startup.
    QString m_hwdec;
    // Configured audio language ("alang"): re-applied before each
    // playUrl so mpv picks the right dub on every load.
    QString m_audioLang;
    // true if an external track was attached with attachAudio (for the
    // audio-file-remove of dropExternalAudio).
    bool m_audioAdded = false;
};