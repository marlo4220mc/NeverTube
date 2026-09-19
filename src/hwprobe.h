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
// AV1 -> VP9 -> H.264 desde resources/clips y lee hwdec-current tras decodificar
// el primer frame:
//   - primer códec con backend real -> "av1-hw" / "vp9-hw" / "h264-hw"
//   - los tres caen a software      -> "soft-all"
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
};

#endif
