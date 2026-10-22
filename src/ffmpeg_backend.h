#pragma once

#include "playback_backend.h"

#include <QImage>
#include <QSize>
#include <QString>

// FfmpegBackend: own player built with FFmpeg/libav (NO libmpv) for Win7 x86.
// Demux (local/HLS) + decode (Phase 2: software; Phase 4: DXVA2 h264 with
// copy-back) + swscale → RGB + frame-driven GL render + pulse-simple (Linux) /
// QAudioOutput (Windows) audio.
//
// Thread model: the heavy work (demux/decode/convert/audio) runs in its own
// thread; this QObject lives in the GUI thread. The thread does NOT emit
// signals directly (it would break the widget's direct connections): it
// publishes state in a snapshot (thread-safe queries) and delivers
// frames/events to the GUI thread with deferred invocations (deliverFrame /
// onEngineEvent), which are the ones that emit the neutral signals.
// pollEvents() is a no-op: there is no event queue to drain, the backend
// pushes.
//
// Phase 4: HW is requested on open (VAAPI/DXVA2, copy-back before swscale);
// hwdecCurrent() reports the real HW device after the first frame ("no" on
// SW, empty until then). The render is per delivered frame (no fixed timer);
// the widget's 33 ms timer remains as a safety net and is harmless here.
//
// x86: the widget uses a 250 ms timer to detect whether another window covers
// the video (Win7/DWM does not report occlusion). While covered, the decode
// thread does NOT transfer (copy-back) nor convert (swscale) frames and
// renderGL does not paint: real CPU/GPU saving under a floating window (the
// GMA 950 FPS drop with low app CPU/RAM is the DWM compositor competing).
// Also, frameToImage scales ONLY down to the render surface size (fewer
// pixels in swscale and in the texture upload; GL does the upscale).

class FfmpegBackend : public PlaybackBackend
{
    Q_OBJECT

public:
    explicit FfmpegBackend(QObject *parent = nullptr);
    ~FfmpegBackend() override;

    // Backend internal state (full definition in the .cpp). It is declared
    // here in the public section so the decode thread's free functions
    // (runLoop, renderGLCore, openFormat) can name it; it is not part of the
    // API and its fields must not be used outside the .cpp.
    struct Impl;

    // --- PlaybackBackend ---
    bool initGL() override;
    void destroyRenderContext() override;
    // x86: the widget detects window coverage (250 ms timer + Windows z-order)
    // and pauses the pipeline while covered (see playback_backend.h
    // occlusionSkipSupported). mpv does NOT support it.
    bool occlusionSkipSupported() const override { return true; }
    void setVideoOccluded(bool occluded) override;
    void shutdown() override;
    bool isValid() const override;
    bool hasRenderContext() const override;
    void *renderContextHandle() const override;
    void renderGL(int fbo, int width, int height) override;
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
    double speed() const override;
    QString configuredHwdec() const override;
    QString hwdecCurrent() const override;
    QString videoCodec() const override;
    int probeMaxVideoHeight() const override;
    bool hasActiveAudio() const override;
    void probeAudioState(qint64 *tracks, qint64 *audioTracks,
                         qint64 *selectedAudio, QString *format) const override;
    bool ensureAudioSelected() override;
    double cacheDuration() const override;
    bool cacheIdle() const override;
    void pollEvents() override;

    // --- diagnostics / selftest ---
    // Bench mode: decodes and converts WITHOUT an audio sink and WITHOUT
    // delivering frames to the render (it only counts framesDecoded()). For
    // offline selftests; does not affect normal playback.
    void setBenchMode(bool on);
    // Video frames decoded since the last playUrl (bench and normal mode;
    // the selftest verifies them).
    int framesDecoded() const;
    // Current video size (0x0 if there is no frame yet).
    QSize videoSize() const;

private slots:
    // They run on the GUI thread (deferred invocations from the decode thread).
    void deliverFrame(const QImage &img);
    void onEngineEvent(int kind, double a, double b, int c,
                       const QString &s);

private:
    Impl *m = nullptr;
};