#pragma once

#include <QObject>
#include <QString>

// Interfaz neutra del motor de reproducción. PlayerWidget (política: estado,
// watchdog, escalera de códec, espera de buffer) y sus clientes
// (PlayerController, MainWindow, WatchPage) dependen de esta interfaz, NO del
// motor concreto. Cada implementación traduce este API agnóstico a su motor
// nativo:
//   - MpvBackend  (x64, pieza protegida): todo el código mpv que antes vivía
//     en PlayerWidget, movido sin cambios de comportamiento.
//   - FfmpegBackend (x86, Win7, fases posteriores): FFmpeg/libav
//     (schannel + DXVA2 h264) sin libmpv.
//
// Las señales se emiten revisando el hilo del widget (GUI). El widget las
// conecta con conexión directa (mismo hilo) para conservar el orden de
// procesamiento de eventos del motor.

class PlaybackBackend : public QObject
{
    Q_OBJECT

public:
    // Razón del fin de archivo, traducida de lo que el motor reporte.
    enum class EndReason {
        Quit,  // el motor se cerró
        Stop,  // parada intencional (stop() / reemplazo de archivo)
        Error, // error de reproducción (el mensaje viaja en endFile)
        Eof,   // el archivo terminó (final natural o EOF prematuro)
    };

    explicit PlaybackBackend(QObject *parent = nullptr) : QObject(parent) {}
    ~PlaybackBackend() override = default;

    // --- ciclo de vida ---

    // Crea el contexto de render del motor. Debe llamarse con el contexto GL
    // actual (desde PlayerWidget::initializeGL). Devuelve true si quedó listo.
    virtual bool initGL() = 0;
    // Libera el contexto de render (contexto GL actual). Se llama ANTES de
    // shutdown() en el destructor del widget (el widget hace makeCurrent()).
    virtual void destroyRenderContext() = 0;
    // Destruye el motor (libera su handle). Se llama DESPUÉS de
    // destroyRenderContext(). No-op si ya se destruyó.
    virtual void shutdown() = 0;
    // true si el motor se creó y acepta comandos (NO confundir con render
    // listo: antes de initializeGL no hay contexto de render).
    virtual bool isValid() const = 0;
    // true si el contexto de render está creado (listo para renderGL()).
    virtual bool hasRenderContext() const = 0;
    // Puntero del contexto de render del motor, SOLO para logs de depuración.
    virtual void *renderContextHandle() const = 0;

    // --- render ---

    // Renderiza el frame actual al FBO indicado. Se llama desde paintGL con el
    // contexto GL actual; width/height en píxeles físicos (ya con dpr).
    virtual void renderGL(int fbo, int width, int height) = 0;

    // --- comandos ---

    // Carga y reproduce `url` (con reanudación opcional en startAt). Si
    // audioUrl no es vacío, intenta abrir vídeo+audio externo juntos en la
    // misma carga (equivalente a la opción per-file "audio-files"); devuelve
    // true si el audio se adjuntó en la carga (el llamador puede descartar
    // audioUrl) y false si se cargó sin él (el llamador conserva audioUrl para
    // adjuntarlo tras el archivo cargado, como respaldo).
    virtual bool playUrl(const QString &url, double startAt,
                         const QString &audioUrl) = 0;
    // Modo ytdl (nada que hacer en motores que resuelven las URLs ellas
    // mismas): el llamador sigue usando playUrl() después.
    virtual void setYtdlFormat(const QString &format) = 0;
    virtual void stop() = 0;
    virtual void setPaused(bool paused) = 0;
    virtual void seek(double seconds) = 0;
    virtual void setVolume(double volume) = 0;
    // Tope máximo del volumen del motor (mpv "volume-max"). Default 100; el
    // ajuste "Volumen máximo" puede subirlo a 150/175/200 para dar headroom.
    virtual void setVolumeMax(double maxPercent) = 0;
    virtual void setSubVisibility(bool visible) = 0;
    // Preferencia de idioma de audio ("alang"): se re-aplica antes de cada
    // playUrl para que el motor elija el doblaje correcto en cada carga.
    virtual void setAudioLanguage(const QString &lang) = 0;
    // Adjunta una pista de audio externa al archivo YA cargado (fallback
    // cuando playUrl no consumió el audio). Devuelve el código del motor
    // (>= 0 correcto, como el 0 de mpv).
    virtual int attachAudio(const QString &url) = 0;
    // Quita la pista de audio externa adjuntada manualmente.
    virtual void dropExternalAudio() = 0;

    // --- consultas ---

    virtual double speed() const = 0;
    // Decodificación por hardware configurada en el motor (mpv "hwdec",
    // p.ej. "auto"). El selftest del player la verifica.
    virtual QString configuredHwdec() const = 0;
    // Backend de decodificación por hardware que el motor usa DE VERDAD para
    // el stream actual, o "no"/vacío si decodifica en software.
    virtual QString hwdecCurrent() const = 0;
    // Códec que el motor decodifica de verdad para el stream actual
    // ("h264", "vp9", "av1", …).
    virtual QString videoCodec() const = 0;
    // Altura máxima entre las pistas de vídeo del archivo cargado, o 0 si aún
    // no cargó o no se puede saber (WatchPage filtra el menú de resoluciones).
    virtual int probeMaxVideoHeight() const = 0;
    // true si hay una pista de audio activa en el stream actual (watchdog de
    // sesión gateada: sin ella el vídeo avanza mudo).
    virtual bool hasActiveAudio() const = 0;
    // Segundos de contenido cacheados por el demuxer (espera inicial de
    // buffer en alta resolución).
    virtual double cacheDuration() const = 0;
    // true si la descarga terminó (todo lo descargable ya está en caché).
    virtual bool cacheIdle() const = 0;

    // Drena la cola de eventos del motor y emite las señales correspondientes.
    // Se llama en el hilo del widget (desde frameReady y desde el wakeup
    // diferido del motor).
    virtual void pollEvents() = 0;

signals:
    // El archivo principal terminó de cargar: momento de consultar la altura
    // máxima (probeMaxVideoHeight) y de adjuntar el audio de respaldo.
    void fileLoaded();
    // Empieza un stream nuevo (START_FILE / re-arranque de reproducción): el
    // códec vuelve a dar oportunidad de reportar HW/SW (red de seguridad del
    // perfil de códec).
    void streamStart();
    // Fin de archivo con su razón. Para Error, errorText trae el mensaje.
    void endFile(PlaybackBackend::EndReason reason, const QString &errorText);
    // Posición de reproducción (segundos).
    void positionUpdated(double seconds);
    // Duración total del stream (segundos).
    void durationUpdated(double seconds);
    void videoWidthUpdated(qint64 width);
    void videoHeightUpdated(qint64 height);
    // Estado de pausa (cambió por comando, por el usuario o por el motor).
    void pauseUpdated(bool paused);
    // Segundos cacheados por el demuxer (salud de la descarga de vídeo).
    void cacheTimeUpdated(double seconds);
    // Línea de log del motor (veredicto "software/hardware decoding",
    // errores de red, …). El prefijo y el texto son los del motor.
    void logMessage(const QString &prefix, const QString &text);
    // Lo pide el motor cuando hay un frame nuevo que pintar (vídeo o cambio
    // de estado): el widget hace pollEvents() + update().
    void frameReady();
};