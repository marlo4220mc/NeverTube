#ifndef HWPROBE_H
#define HWPROBE_H

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

struct mpv_handle;

class QTimer;

// Sondeo de capacidades de decodificación por hardware al arranque. Reproduce
// en headless (vo=null, hwdec=auto-copy) un micro-clip por códec en orden
// AV1 -> VP9 -> H.264 desde resources/clips.
//
// El veredicto de HW sale del LOG de mpv, no de hwdec-current: mpv emite
// "Using hardware decoding (<api>)" (level info) o "Using software decoding"
// (level verbose) y esa línea refleja la ruta REAL del decodificador. En GPUs
// antiguas (p.ej. Intel GMA 950, solo MPEG-2) hwdec-current puede reportar un
// backend de software (dispositivo WARP) y dar un falso positivo; el log lo
// desmiente ("Using software decoding"). hwdec-current queda solo como
// respaldo si mpv no emitiera ninguna de las dos líneas:
//   - primer códec con log de HW -> "av1-hw" / "vp9-hw" / "h264-hw"
//   - todos caen a software      -> "soft-all"
// Fija el perfil antes del primer vídeo. MainWindow mantiene además una red de
// seguridad reactiva (streamDecodeInfoReady) que degrada el perfil si el códec
// que HwProbe dio por HW acaba decodificándose en software.
class HwProbe : public QObject
{
    Q_OBJECT

public:
    explicit HwProbe(QObject *parent = nullptr);
    ~HwProbe() override;

    void start();

signals:
    // Perfil detectado: "av1-hw" | "vp9-hw" | "h264-hw" | "soft-all".
    // Vacío si no se pudo determinar de forma fiable (timeout, sin decodificador
    // o error de arranque): el llamante mantiene el default h264 sin persistir
    // y el sondeo se reintentará en el próximo arranque.
    void finished(const QString &profileName);

private slots:
    void poll();

private:
    void loadCurrent();
    void advance(bool hardware);
    void finish(const QString &profile);

    mpv_handle *m_mpv = nullptr;
    QTimer *m_timer = nullptr;
    QTemporaryDir m_tmpDir;
    QElapsedTimer m_clipTimer;
    QString m_clipPaths[3];
    int m_index = 0;
    bool m_done = false;
    // Veredicto de HW leído del log de mpv para el clip actual (se resetea en
    // cada loadCurrent). m_sawHwLog gana salvo que aparezca m_sawSwLog, que
    // indica que mpv cayó a software tras intentar el HW.
    bool m_sawHwLog = false;
    bool m_sawSwLog = false;
};

#endif
