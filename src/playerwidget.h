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
    // Tope máximo del volumen de mpv (mpv "volume-max"). Default 100; el ajuste
    // "Volumen máximo" de Ajustes puede subirlo a 150/175/200 para dar headroom.
    void setVolumeMax(double maxPercent);
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
    // como red de seguridad del perfil de códec detectado por HwProbe: si el
    // códec que el perfil da por HW cae a software, degrada el perfil.
    void streamDecodeInfoReady(const QString &codec, const QString &hcdecBackend);
    // Cuando un stream termina de cargarse, se emite la mayor altura (píxeles)
    // disponible entre sus pistas de vídeo. WatchPage la usa para filtrar el
    // menú de resoluciones: ocultar los ítems por encima de lo que existe para
    // ese vídeo (evita ofrecer "1080p" en un clip de 720p). 0 si no se pudo
    // determinar (en ese caso el menú se queda completo).
    void qualityLimitsChanged(int maxVideoHeight);
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
    void retryHwdec();

private:
    static void mpvWakeup(void *ctx);
    static void mpvUpdate(void *ctx);
    void handleEvent(mpv_event *event);
    void emitAspectIfReady();
    void addAudioFileNow();
    // single loadfile de vídeo+audio externo juntos: mpv abre y sincroniza
    // ambas pistas desde el arranque (opción per-file "audio-files" en el
    // comando loadfile). Devuelve el código del comando; <0 -> fallback
    // audio-add posterior (addAudioFileNow en FILE_LOADED).
    int loadFileWithAudio(const QByteArray &videoUrl);
    // Lee mpv "track-list" y devuelve la altura máxima entre las pistas de
    // vídeo. Devuelve 0 si el archivo aún no cargó (FILE_LOADED no llegó) o
    // si la consulta falló. La lista de calidades del menú (WatchPage) se
    // oculta por encima del techo real para no ofrecer resoluciones que el
    // mpv no puede seleccionar.
    int probeMaxVideoHeight() const;

    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_rctx = nullptr;
    int m_logFrames = 0;
    double m_position = 0.0;
    double m_duration = 0.0;
    QString m_state;
    QString m_hwdec;
    int64_t m_videoW = 0;
    int64_t m_videoH = 0;
    // Altura máxima real entre las pistas de vídeo del archivo cargado
    // (track-list). 0 si aún no se leyó. Consumida por WatchPage para
    // filtrar el menú de resoluciones.
    int m_videoMaxH = 0;
    double m_aspect = 0.0;
    QString m_audioLang;
    QString m_pendingAudioFile;
    bool m_audioAdded = false;
    bool m_redrawTimerActive = false;
    bool m_hwdecProbed = false;
    // Veredicto del log de mpv para el stream actual: mpv dice explícitamente
    // "Using software decoding" aunque hwdec-current reporte un backend
    // (dispositivos de software en GPUs antiguas). Se resetea por stream.
    bool m_hwdecLogSoftware = false;
    // hwdec-current llega vacío hasta que mpv activa el decodificador; se
    // reintenta con este timer para que la red de seguridad del perfil reciba
    // un backend resuelto ya en el primer vídeo.
    QTimer *m_hwdecRetry = nullptr;
    int m_hwdecRetryAttempts = 0;
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