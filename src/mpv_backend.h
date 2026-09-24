#pragma once

#include "playback_backend.h"

#include <QString>

#include <mpv/client.h>
#include <mpv/render.h>

// Backend mpv: la única implementación de PlaybackBackend en esta fase (en
// x64; la x86 con FFmpeg llega en fases posteriores). Contiene TODO el código
// de integración mpv que antes vivía en PlayerWidget, movido SIN cambios de
// comportamiento: opciones de arranque (vo=libmpv, hwdec=auto, …), observe de
// propiedades, callbacks wakeup/update, handleEvent (traducido a señales
// neutras), loadfile con la opción per-file "audio-files", render directo al
// FBO y consultas (track-list, audio-params, cache, …).
class MpvBackend : public PlaybackBackend
{
    Q_OBJECT

public:
    explicit MpvBackend(QObject *parent = nullptr);
    ~MpvBackend() override;

    // --- ciclo de vida / render ---
    bool initGL() override;
    void destroyRenderContext() override;
    void shutdown() override;
    bool isValid() const override { return m_mpv != nullptr; }
    bool hasRenderContext() const override { return m_rctx != nullptr; }
    void *renderContextHandle() const override { return m_rctx; }
    void renderGL(int fbo, int width, int height) override;

    // --- comandos ---
    bool playUrl(const QString &url, double startAt,
                 const QString &audioUrl) override;
    void setYtdlFormat(const QString &format) override;
    void stop() override;
    void setPaused(bool paused) override;
    void seek(double seconds) override;
    void setVolume(double volume) override;
    void setVolumeMax(double maxPercent) override;
    void setSubVisibility(bool visible) override;
    void setAudioLanguage(const QString &lang) override;
    int attachAudio(const QString &url) override;
    void dropExternalAudio() override;

    // --- consultas ---
    double speed() const override;
    QString configuredHwdec() const override { return m_hwdec; }
    QString hwdecCurrent() const override;
    QString videoCodec() const override;
    int probeMaxVideoHeight() const override;
    bool hasActiveAudio() const override;
    double cacheDuration() const override;
    bool cacheIdle() const override;

    // Drena la cola de eventos mpv y emite las señales correspondientes.
    void pollEvents() override;

private slots:
    // Invocado de forma diferida por el callback de wakeup de mpv.
    void drainEvents();
    // Invocado de forma diferida por el callback de update de mpv (frame nuevo).
    void deliverFrameReady();

private:
    static void mpvWakeup(void *ctx);
    static void mpvUpdate(void *ctx);
    void handleEvent(mpv_event *event);

    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_rctx = nullptr;
    // Contador de frames logueados del render (cada 90, como antes).
    int m_logFrames = 0;
    // Decodificación por hardware configurada ("auto"), leída al arrancar.
    QString m_hwdec;
    // Idioma de audio configurado ("alang"): se re-aplica antes de cada
    // playUrl para que mpv elija el doblaje correcto en cada carga.
    QString m_audioLang;
    // true si se adjuntó una pista externa con attachAudio (para el
    // audio-file-remove de dropExternalAudio).
    bool m_audioAdded = false;
};