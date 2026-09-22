#include "hwprobe.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#include <mpv/client.h>

#include <clocale>

#include "debug.h"

namespace {

// Orden deliberado: si AV1 tiene HW, es el mejor códec y no hace falta probar
// más; VP9 solo se acepta si AV1 cayó a software (misma semántica que la
// escalera anterior); H.264 HW confirma el perfil legacy; H.264 software es la
// prueba de que no hay HW en absoluto (el H.264 lo decodifican en HW todas las
// GPUs con decodificador de vídeo).
struct ClipSpec
{
    const char *resource;
    const char *file;
    const char *profile;
};

const ClipSpec kClips[] = {
    { ":/clips/av1.mkv", "av1.mkv", "av1-hw" },
    { ":/clips/vp9.webm", "vp9.webm", "vp9-hw" },
    { ":/clips/h264.mp4", "h264.mp4", "h264-hw" },
};
constexpr int kClipCount = 3;

constexpr int kPollMs = 10;
// 8 s por clip (con un reintento) es de sobra para un clip local de 512x288.
// En máquinas virtuales/runners el PRIMER init del decodificador por hardware
// (DXVA/VAAPI) puede tardar varios segundos y el clip expiraba a los 3 s; si
// tras el reintento tampoco hay veredicto, no hay conclusión fiable y se
// prefiere abortar (default conservador) antes que asumir SW.
constexpr qint64 kClipTimeoutMs = 8000;

} // namespace

HwProbe::HwProbe(QObject *parent)
    : QObject(parent)
{
}

HwProbe::~HwProbe()
{
    if (m_timer)
        m_timer->stop();
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
}

void HwProbe::start()
{
    if (m_done || m_mpv)
        return;
    // mpv exige LC_NUMERIC=C: sin esto mpv_create devuelve NULL.
    std::setlocale(LC_NUMERIC, "C");

    if (!m_tmpDir.isValid()) {
        qWarning("hwprobe: no se pudo crear el directorio temporal");
        finish(QString());
        return;
    }
    for (int i = 0; i < kClipCount; ++i) {
        QFile in(QString::fromLatin1(kClips[i].resource));
        if (!in.open(QIODevice::ReadOnly)) {
            qWarning("hwprobe: falta el recurso %s", kClips[i].resource);
            finish(QString());
            return;
        }
        const QString out =
            m_tmpDir.filePath(QString::fromLatin1(kClips[i].file));
        QFile of(out);
        if (!of.open(QIODevice::WriteOnly)) {
            qWarning("hwprobe: no se pudo preparar %s",
                     out.toUtf8().constData());
            finish(QString());
            return;
        }
        of.write(in.readAll());
        of.close();
        m_clipPaths[i] = QDir::toNativeSeparators(out);
    }

    m_mpv = mpv_create();
    if (!m_mpv) {
        qWarning("hwprobe: mpv_create fallo");
        finish(QString());
        return;
    }
    // Headless: sin ventana ni audio. auto-copy es lo que permite que vo=null
    // acepte el frame en cualquier plataforma (vulkan-copy / d3d11va-copy /
    // vaapi-copy): los modos sin -copy exigirían un VO GPU real.
    mpv_set_option_string(m_mpv, "vo", "null");
    mpv_set_option_string(m_mpv, "hwdec", "auto-copy");
    mpv_set_option_string(m_mpv, "audio", "no");
    mpv_set_option_string(m_mpv, "aid", "no");
    mpv_set_option_string(m_mpv, "config", "no");
    mpv_set_option_string(m_mpv, "load-scripts", "no");
    mpv_set_option_string(m_mpv, "terminal", "no");
    // vd=v para capturar el veredicto real del decodificador:
    //   "Using hardware decoding (<api>)" (info) / "Using software decoding" (verbose).
    mpv_set_option_string(m_mpv, "msg-level", "all=no,vd=v");
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "force-window", "no");
    if (mpv_initialize(m_mpv) < 0) {
        qWarning("hwprobe: mpv_initialize fallo");
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        finish(QString());
        return;
    }
    // Los eventos de log traen el veredicto de HW (ver comentario del header).
    mpv_request_log_messages(m_mpv, "v");

    m_index = 0;
    m_clipRetried = false;
    m_clipTimer.start();
    loadCurrent();

    m_timer = new QTimer(this);
    m_timer->setInterval(kPollMs);
    connect(m_timer, &QTimer::timeout, this, &HwProbe::poll);
    m_timer->start();
}

void HwProbe::loadCurrent()
{
    if (!m_mpv)
        return;
    const QByteArray p = m_clipPaths[m_index].toUtf8();
    const char *cmd[] = { "loadfile", p.constData(), "replace", nullptr };
    // Nuevo clip, nuevo veredicto: el log del decodificador anterior no cuenta.
    m_sawHwLog = false;
    m_sawSwLog = false;
    mpv_command(m_mpv, cmd);
    NT_DEBUG("E2E hwprobe load %d=%s\n", m_index, p.constData());
}

void HwProbe::advance(bool hardware)
{
    if (m_done)
        return;
    if (hardware) {
        finish(QString::fromLatin1(kClips[m_index].profile));
        return;
    }
    // Software en este códec: pasar al siguiente (o soft-all si era h264).
    if (m_index >= kClipCount - 1) {
        finish(QStringLiteral("soft-all"));
        return;
    }
    if (m_mpv) {
        const char *stop[] = { "stop", nullptr };
        mpv_command(m_mpv, stop);
    }
    ++m_index;
    m_clipRetried = false;
    m_clipTimer.restart();
    loadCurrent();
}

void HwProbe::poll()
{
    if (!m_mpv || m_done)
        return;

    bool ended = false;
    while (true) {
        mpv_event *event = mpv_wait_event(m_mpv, 0);
        if (!event || event->event_id == MPV_EVENT_NONE)
            break;
        if (event->event_id == MPV_EVENT_END_FILE) {
            auto *end = static_cast<mpv_event_end_file *>(event->data);
            if (end && end->reason != MPV_END_FILE_REASON_QUIT
                && end->reason != MPV_END_FILE_REASON_STOP)
                ended = true;
        } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            // Veredicto real del decodificador (ver header de hwprobe.h).
            auto *lm = static_cast<mpv_event_log_message *>(event->data);
            if (lm && lm->text) {
                const QString text = QString::fromUtf8(lm->text);
                if (text.contains(QLatin1String("Using hardware decoding")))
                    m_sawHwLog = true;
                else if (text.contains(QLatin1String("Using software decoding")))
                    m_sawSwLog = true;
            }
        }
    }

    char *hwRaw = nullptr;
    const int r = mpv_get_property(m_mpv, "hwdec-current", MPV_FORMAT_STRING,
                                   &hwRaw);
    const QString hwdec = (r >= 0 && hwRaw) ? QString::fromUtf8(hwRaw)
                                            : QString();
    if (hwRaw)
        mpv_free(hwRaw);

    int64_t h = 0;
    mpv_get_property(m_mpv, "video-params/h", MPV_FORMAT_INT64, &h);
    const bool decoded = h > 0;

    // Solo concluimos con dato real: el clip decodificó (h>0) y mpv resolvió
    // hwdec-current (lo deja vacío hasta activar el decodificador). Un
    // END_FILE por error (sin llegar a decodificar) es INDETERMINADO, no
    // "software": un fallo de decode no implica falta de soporte HW. En ese
    // caso se aborta el sondeo y el llamante aplica un perfil conservador
    // (soft-all) sin persistir, reintentando el sondeo al arrancar.
    //
    // El veredicto de HW manda desde el LOG de mpv. Si mpv dijo "Using software
    // decoding" es software aunque hwdec-current reporte un backend (GPUs
    // antiguas con dispositivo de software, p.ej. GMA 950). Solo si mpv no
    // emitió ninguna de las dos líneas se usa hwdec-current como respaldo.
    if (decoded && !hwdec.isEmpty()) {
        const bool logVerdict = m_sawHwLog || m_sawSwLog;
        const bool hardware = logVerdict ? (m_sawHwLog && !m_sawSwLog)
                                         : (hwdec != QLatin1String("no"));
        NT_DEBUG("E2E hwprobe %s hwdec=%s log(hw=%d sw=%d) -> %s\n",
                 kClips[m_index].file, hwdec.toUtf8().constData(),
                 m_sawHwLog ? 1 : 0, m_sawSwLog ? 1 : 0,
                 hardware ? "HW" : "SW");
        advance(hardware);
        return;
    }
    if (ended) {
        NT_DEBUG("E2E hwprobe %s decode-error -> indeterminado\n",
                 kClips[m_index].file);
        finish(QString());
        return;
    }

    if (m_clipTimer.elapsed() > kClipTimeoutMs) {
        if (!m_clipRetried) {
            // Un reintento por clip: en VMs/runners el primer intento de hwdec
            // puede tardar (init del decodificador DXVA/VAAPI) más que el
            // timeout. El reintento reinicia el veredicto del clip.
            m_clipRetried = true;
            NT_DEBUG("E2E hwprobe retry clip=%s\n", kClips[m_index].file);
            if (m_mpv) {
                const char *stop[] = { "stop", nullptr };
                mpv_command(m_mpv, stop);
            }
            m_clipTimer.restart();
            loadCurrent();
            return;
        }
        NT_DEBUG("E2E hwprobe timeout clip=%s hwdec=%s\n", kClips[m_index].file,
                 hwdec.toUtf8().constData());
        finish(QString());
    }
}

void HwProbe::finish(const QString &profile)
{
    if (m_done)
        return;
    m_done = true;
    if (m_timer) {
        m_timer->stop();
        m_timer->deleteLater();
        m_timer = nullptr;
    }
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
    if (!profile.isEmpty())
        NT_DEBUG("E2E hwprobe profile=%s\n", profile.toUtf8().constData());
    emit finished(profile);
}
