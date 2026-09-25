#ifndef PLAYERWIDGET_H
#define PLAYERWIDGET_H

#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QTimer>

#include "playback_backend.h"

// Superficie de vídeo + política de reproducción. Todo acceso al motor va por
// la interfaz PlaybackBackend (en esta fase, MpvBackend; la x86 con FFmpeg
// llega en fases posteriores): el widget NO conoce mpv. Aquí viven el estado
// (posición/duración/state), el watchdog de atasco y de sesión gateada
// (checkStall), la red de seguridad del perfil de códec (hwdec probe/retry y
// corrección por log), y la espera inicial de buffer en alta resolución — que
// es la pieza protegida (QOpenGLWidget, redibujado por eventos: el motor
// notifica frameReady cuando hay un frame nuevo y onFrameReady fuerza
// QWidget::update(); render directo al FBO en paintGL).
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

    // Configuración de decodificación por hardware del motor (mpv "hwdec");
    // verificado por --selftest-player como "auto".
    QString hwdec() const;

    // Acceso de solo lectura para selftest --selftest-player: true cuando el
    // contexto de render del motor quedó creado (initializeGL corrió). En mpv
    // ese mismo initGL instala el update callback, así que implica que la
    // ruta de redibujado por eventos está activa (sin timer de poll).
    bool renderContextReady() const { return m_backend && m_backend->hasRenderContext(); }
    // Backend de decodificación por hardware que el motor está usando de
    // verdad para el stream actual, o "no"/vacío si decodifica en software.
    QString hwdecCurrent() const;
    // Códec que el motor está decodificando de verdad para el stream actual
    // ("h264", "vp9", "av1", …).
    QString videoCodec() const;

public slots:
    void playUrl(const QString &url, double startAt = 0.0);
    void playYtdl(const QString &watchUrl, int maxHeight);
    void stop();
    void setPaused(bool paused);
    void seek(double seconds);
    void setVolume(double volume);
    // Tope máximo del volumen del motor (mpv "volume-max"). Default 100; el
    // ajuste "Volumen máximo" de Ajustes puede subirlo a 150/175/200 para dar
    // headroom.
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
    // de decodificación por hardware que el motor usa de verdad (hwdec-current,
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
    // Interrupción de la reproducción distinta de un atasco de posición:
    // EOF prematuro (posición << duración del vídeo) o pista de audio externa
    // que nunca abrió (p.ej. 403 de YouTube sobre las URLs de la sesión: el
    // vídeo avanza en silencio hasta que los segmentos también mueren). El
    // controlador re-resuelve con URLs frescas y reanuda en la posición.
    void streamInterrupted();

protected:
    void initializeGL() override;
    void paintGL() override;

private slots:
    // Backend ::frameReady: hay un frame nuevo (o cambio de estado) que pintar.
    void onFrameReady();
    // Backend ::fileLoaded: el archivo principal terminó de cargar.
    void onFileLoaded();
    // Backend ::streamStart: empieza un stream nuevo (re-armar el probe).
    void onStreamStart();
    // Backend ::endFile: fin de archivo con su razón y mensaje de error.
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
    // x86 (FfmpegBackend): detecta si otra ventana cubre por completo el vídeo
    // (timer lento; Win7/DWM no avisa de oclusión). Mientras esté cubierto, el
    // motor deja de convertir y de pintar (ahorro real de CPU/GPU).
    void checkVideoCovered();

private:
    void emitAspectIfReady();
    // Recorrido del z-order de Windows: true si una ventana visible y no
    // transparente (por encima de nuestra top-level) cubre el rect del vídeo.
    // Solo Windows; en Linux/Linux no se usa (el FfmpegBackend no salta).
    bool occludedByTopWindow();

    PlaybackBackend *m_backend = nullptr;
    double m_position = 0.0;
    double m_duration = 0.0;
    QString m_state;
    int64_t m_videoW = 0;
    int64_t m_videoH = 0;
    // Altura máxima real entre las pistas de vídeo del archivo cargado
    // (track-list). 0 si aún no se leyó. Consumida por WatchPage para
    // filtrar el menú de resoluciones.
    int m_videoMaxH = 0;
    double m_aspect = 0.0;
    // URL de audio externo pendiente de adjuntar (edge/AV1-HLS): se consume en
    // el próximo playUrl (audio-files) o, como respaldo, tras el archivo
    // cargado (attachAudio en fileLoaded).
    QString m_pendingAudioFile;
    bool m_hwdecProbed = false;
    // Veredicto del log del motor para el stream actual: el motor dice
    // explícitamente "Using software decoding" aunque hwdec-current reporte un
    // backend (dispositivos de software en GPUs antiguas). Se resetea por
    // stream.
    bool m_hwdecLogSoftware = false;
    // hwdec-current llega vacío hasta que el motor activa el decodificador; se
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
    // x86: cobertura del vídeo por otra ventana. Timer lento de 250 ms +
    // recorrido del z-order; solo FfmpegBackend (occlusionSkipSupported).
    // En mpv/x64 esto NO aplica (parar el render acumula frames).
    QTimer *m_occlusionTimer = nullptr;
    bool m_videoOccluded = false;
};

#endif