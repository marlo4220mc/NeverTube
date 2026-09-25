#pragma once

#include "playback_backend.h"

#include <QImage>
#include <QSize>
#include <QString>

// FfmpegBackend: reproductor propio con FFmpeg/libav (SIN libmpv) para la
// Win7 x86. Demux (local/HLS) + decode (Fase 2: software; Fase 4:
// DXVA2 h264 con copy-back) + swscale → RGB + render GL frame-driven +
// audio pulse-simple (Linux) / QAudioOutput (Windows).
//
// Modelo de hilos: el trabajo pesado (demux/decode/convert/audio) corre en un
// hilo propio; este objeto QObject vive en el hilo GUI. El hilo NO emite
// señales directamente (rompería las conexiones directas del widget): publica
// estado en un snapshot (consultas thread-safe) y entrega frames/eventos al
// hilo GUI con invocaciones diferidas (deliverFrame / onEngineEvent), que son
// las que emiten las señales neutras. pollEvents() es no-op: no hay cola de
// eventos que drenar, el backend empuja.
//
// Fase 4: al abrir se solicita HW (VAAPI/DXVA2, copy-back antes de swscale);
// hwdecCurrent() reporta el device HW real tras el primer frame ("no" en SW,
// vacío hasta entonces). El render es por frame entregado (sin timer fijo); el
// timer de 33 ms del widget queda como red de seguridad y es inofensivo aquí.
//
// x86: el widget detecta con un timer de 250 ms si otra ventana cubre el vídeo
// (Win7/DWM no avisa de oclusión). Mientras está cubierto, el hilo de decode NO
// transfiere (copy-back) ni convierte (swscale) frames y renderGL no pinta:
// ahorro real de CPU/GPU bajo una ventana flotante (el drop de FPS de la GMA
// 950 con CPU/RAM de la app bajos es el compositor DWM compitiendo). Además,
// frameToImage escala SOLO hacia abajo hasta el tamaño del surface de render
// (menos píxeles en swscale y en la subida de textura; el upscale lo hace GL).

class FfmpegBackend : public PlaybackBackend
{
    Q_OBJECT

public:
    explicit FfmpegBackend(QObject *parent = nullptr);
    ~FfmpegBackend() override;

    // Estado interno del backend (definición completa en el .cpp). Se declara
    // aquí en sección pública para que las funciones libres del hilo de
    // decode (runLoop, renderGLCore, openFormat) puedan nombrarlo; no es parte
    // del API y sus campos no deben usarse fuera del .cpp.
    struct Impl;

    // --- PlaybackBackend ---
    bool initGL() override;
    void destroyRenderContext() override;
    // x86: el widget detecta cobertura de ventanas (timer de 250 ms + z-order
    // de Windows) y pausa el pipeline mientras esté cubierto (ver
    // playback_backend.h occlusionSkipSupported). mpv NO lo soporta.
    bool occlusionSkipSupported() const override { return true; }
    void setVideoOccluded(bool occluded) override;
    void shutdown() override;
    bool isValid() const override;
    bool hasRenderContext() const override;
    void *renderContextHandle() const override;
    void renderGL(int fbo, int width, int height) override;
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
    double speed() const override;
    QString configuredHwdec() const override;
    QString hwdecCurrent() const override;
    QString videoCodec() const override;
    int probeMaxVideoHeight() const override;
    bool hasActiveAudio() const override;
    double cacheDuration() const override;
    bool cacheIdle() const override;
    void pollEvents() override;

    // --- diagnóstico / selftest ---
    // Modo banco: decodifica y convierte SIN sink de audio y SIN entregar
    // frames al render (solo cuenta framesDecoded()). Para selftest offline;
    // no afecta a la reproducción normal.
    void setBenchMode(bool on);
    // Frames de vídeo decodificados desde la última playUrl (modo banco y
    // normal; el selftest los verifica).
    int framesDecoded() const;
    // Tamaño del vídeo actual (0x0 si aún no hay frame).
    QSize videoSize() const;

private slots:
    // Corren en el hilo GUI (invocaciones diferidas desde el hilo de decode).
    void deliverFrame(const QImage &img);
    void onEngineEvent(int kind, double a, double b, int c,
                       const QString &s);

private:
    Impl *m = nullptr;
};