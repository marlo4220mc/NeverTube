#ifndef PLAYERWIDGET_H
#define PLAYERWIDGET_H

#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QTimer>

#include <mpv/client.h>
#include <mpv/render.h>

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

    // Acceso de solo lectura para selftest --selftest-player.
    QString hwdec() const { return m_hwdec; }
    int redrawIntervalMs() const { return m_redrawTimerActive ? 33 : 0; }
    // Backend de decodificación por hardware que mpv está usando de verdad
    // para el stream actual, o "no"/vacío si decodifica en software.
    QString hwdecCurrent() const;
    // Códec que mpv está decodificando de verdad para el stream actual
    // ("h264", "vp9", "av1", …).
    QString videoCodec() const;

public slots:
    void playUrl(const QString &url, double startAt = 0.0);
    void playYtdl(const QString &watchUrl, int maxHeight);
    void stop();
    void setPaused(bool paused);
    void seek(double seconds);
    void setVolume(double volume);
    void setSubVisibility(bool visible);
    void setAudioLanguage(const QString &lang);
    // Para VP9/webm: agregar pista de audio externa (opus webm direct URL).
    // En guarda m_pendingAudioFile y se aplica con el comando "audio-file-add"
    // justo después de MPV_EVENT_FILE_LOADED (en mpv 0.41 la opción --audio-file
    // es CLI-only y set_option_string falla con OPTION_NOT_FOUND).
    void setAudioFile(const QString &url);
    // Espera inicial de buffer al arrancar streams de alta resolución (>=1080p):
    // arranca en pausa y solo reanuda cuando el demuxer ha acumulado `secs`
    // segundos de contenido (demuxer-cache-duration) según la descarga real
    // (QTimer de muestreo lanzado en video-params/h). El objetivo se escala por
    // resolución (1440p/2160p piden más margen). Se tolera con un timeout y se
    // aborta si la red va por debajo del consumo. secs en segundos; 0 = off.
    void setInitialBufferSecs(double secs);

signals:
    void positionChanged();
    void durationChanged();
    void stateChanged();
    void mediaError(const QString &message);
    void videoAspectChanged(double aspect);
    void videoSizeChanged(int width, int height);
    // Se emite al empezar a decodificar cada stream real: el códec y el backend
    // de decodificación por hardware que mpv usa de verdad (hwdec-current,
    // p.ej. "vaapi"/"vulkan-copy"; "no"/vacío = software). MainWindow lo usa
    // para detectar (solo la primera vez) si hay decodificación por hardware y
    // persistir el perfil de códec del master edge, y para auto-corregirse:
    // si el perfil es "modern" y al reproducir AV1 mpv cae a software, degrada
    // a "legacy" (esa GPU no tiene decodificador AV1).
    void streamDecodeInfoReady(const QString &codec, const QString &hcdecBackend);
    // La posición se estancó > 5 s reproduciendo (sin pausa de usuario ni
    // buffer inicial). El controlador re-resuelve con URLs frescas y reanuda.
    void streamStalled();

protected:
    void initializeGL() override;
    void paintGL() override;

private slots:
    void poll();
    void onFrameReady();
    void checkInitialBuffer();
    void checkStall();

private:
    static void mpvWakeup(void *ctx);
    static void mpvUpdate(void *ctx);
    void handleEvent(mpv_event *event);
    void emitAspectIfReady();
    void addAudioFileNow();

    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_rctx = nullptr;
    int m_logFrames = 0;
    double m_position = 0.0;
    double m_duration = 0.0;
    QString m_state;
    QString m_hwdec;
    int64_t m_videoW = 0;
    int64_t m_videoH = 0;
    double m_aspect = 0.0;
    QString m_audioLang;
    QString m_pendingAudioFile;
    bool m_audioAdded = false;
    bool m_redrawTimerActive = false;
    bool m_hwdecProbed = false;
    // Espera inicial de buffer (segundos de contenido), activa para >=1080p.
    double m_initialBufferSecs = 0.0;
    QTimer *m_bufferTimer = nullptr;
    QElapsedTimer m_bufferStart;
    // Detección de atasco: posición congelada reproduciendo (ver streamStalled).
    QTimer *m_stallTimer = nullptr;
    qint64 m_lastProgressMs = 0;
    bool m_paused = false;
    // Detección de "vídeo sin datos": con audio externo la posición avanza
    // aunque la pista de vídeo no cargue (queda el último frame del vídeo
    // anterior). m_cacheOk pasa a true cuando el demuxer cachea contenido.
    qint64 m_videoLoadMs = 0;
    bool m_cacheOk = false;
};

#endif