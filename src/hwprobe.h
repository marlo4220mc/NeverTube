#ifndef HWPROBE_H
#define HWPROBE_H

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

struct mpv_handle;

class QTimer;

namespace hwprobe {

// Veredicto de un clip tras el primer frame decodificado y el fin de la
// ventana de gracia. Decidir ANTES de que el decodificador se pronuncie es el
// origen de los falsos positivos de HW (p.ej. av1-hw en i5/HD4600 QSV: el
// probe veía hwdec-current=qsv-copy y los errores "av1_qsv: unsupported" +
// la caída a libdav1d llegaban 5 ms después del veredicto).
//
//   - "HW": mpv confirmó hardware ("Using hardware decoding") y NADA lo
//     desmiente (ni caída a software ni intento HW fallido ni hwdec=no).
//     El backend activo en hwdec-current sin señales en contra es un respaldo
//     para GPUs cuyo log no llegara (mismo comportamiento que antes, pero ya
//     con la gracia consumida).
//   - "SW": mpv confirmó software ("Using software decoding", intento HW del
//     códec que falló en ffmpeg, o hwdec-current == "no").
//   - "":   indeterminado; el llamante continúa la escalera sin concluir.
QString clipVerdict(bool sawHwLog, bool sawSwLog, bool sawHwFailed,
                    const QString &hwdecCurrent);

} // namespace hwprobe

// Sondeo de capacidades de decodificación por hardware al arranque. Reproduce
// en headless (vo=null, hwdec=auto-copy) un micro-clip por códec en orden
// AV1 -> VP9 -> H.264 desde resources/clips.
//
// El veredicto de HW sale del LOG de mpv (módulos vd y ffmpeg), no de
// hwdec-current: mpv emite "Using hardware decoding (<api>)" /
// "Using software decoding" y, cuando un intento HW falla (p.ej. av1_qsv en
// HD 4600), los errores se ven en ffmpeg. hwdec-current queda solo como
// respaldo. Regla (la misma para cualquier GPU):
//   - primer códec con veredicto "HW" -> "av1-hw" / "vp9-hw" / "h264-hw"
//   - todos "SW"            -> "soft-all" (persistido: es un veredicto real)
//   - sin veredicto fiable   -> perfil vacío (soft-all transitorio sin
//     persistir; el sondeo se reintenta en el próximo arranque)
// Un resultado "indeterminado" NO aborta: se continúa probando los códecs
// siguientes (el i5-4590 llega así a H.264, que sí decodifica por HW).
class HwProbe : public QObject
{
    Q_OBJECT

public:
    explicit HwProbe(QObject *parent = nullptr);
    ~HwProbe() override;

    void start();

signals:
    // Perfil detectado: "av1-hw" | "vp9-hw" | "h264-hw" | "soft-all".
    // Vacío si no se pudo determinar de forma fiable: el llamante mantiene el
    // default conservador sin persistir y el sondeo se reintenta al arrancar.
    void finished(const QString &profileName);

private slots:
    void poll();

private:
    void loadCurrent();
    void advance();
    void endOfLadder();
    void finish(const QString &profile);

    mpv_handle *m_mpv = nullptr;
    QTimer *m_timer = nullptr;
    QTemporaryDir m_tmpDir;
    QElapsedTimer m_clipTimer;
    QString m_clipPaths[3];
    int m_index = 0;
    bool m_done = false;
    // Veredicto del clip actual (se resetea en cada loadCurrent). El log del
    // decodificador puede encolarse 1-10 ms DESPUÉS del primer video-params;
    // por eso se espera la gracia (m_decoded + m_decodedTimer) antes de
    // concluir.
    bool m_sawHwLog = false;
    bool m_sawSwLog = false;
    bool m_sawHwFailed = false; // el intento HW del códec falló (ffmpeg)
    bool m_decoded = false;     // primer frame del clip actual
    QElapsedTimer m_decodedTimer;
    // Nº de clips con veredicto SW decisivo: al llegar a h264 sin HW, si hubo
    // algún SW concluyente se persiste soft-all (veredicto real); si todo fue
    // indeterminado, vacío (sin persistir).
    int m_swVerdicts = 0;
    // Un reintento por clip si expira el timeout (init de hw lento en VMs).
    bool m_clipRetried = false;
};

#endif