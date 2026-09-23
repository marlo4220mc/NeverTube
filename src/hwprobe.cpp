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
// (DXVA/VAAPI) puede tardar varios segundos; si tras el reintento tampoco hay
// veredicto, el clip queda indeterminado y se continúa con el siguiente códec.
constexpr qint64 kClipTimeoutMs = 8000;
// Gracia tras el primer frame: el veredicto del decodificador (log y errores
// de ffmpeg) se encola 1-10 ms DESPUÉS del primer video-params. Decidir sin
// esperarlo es lo que producía falsos positivos (av1-hw en i5 con qsv-copy:
// los errores "av1_qsv: unsupported" llegaban 5 ms después del veredicto).
constexpr qint64 kGraceMs = 400;

// El códec del clip actual, p.ej. "av1-hw" -> "av1".
QByteArray codecOf(int index)
{
    const QString profile = QLatin1String(kClips[index].profile);
    const int dash = profile.indexOf(QLatin1Char('-'));
    return profile.left(dash).toUtf8();
}

// Error del intento de HW del códec en el log de ffmpeg/vd: el intento falló
// pero el clip puede seguir decodificándose por software. Es la señal que
// desmiente un hwdec-current = "<api>-copy" activo (i5/HD4600, av1_qsv).
bool hwAttemptFailed(const QString &prefix, const QString &text,
                     const QByteArray &codec)
{
    if (!(prefix.contains(QLatin1String("ffmpeg"))
          || prefix.contains(QLatin1String("vd"))
          || prefix.contains(QLatin1String("hwdec"))))
        return false;
    if (!text.contains(QLatin1String(codec.constData()), Qt::CaseInsensitive))
        return false;
    static const char *kErrors[] = { "error", "unsupported", "invalid",
                                     "failed", "could not",  "cannot" };
    for (const char *e : kErrors) {
        if (text.contains(QLatin1String(e), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

} // namespace

namespace hwprobe {

QString clipVerdict(bool sawHwLog, bool sawSwLog, bool sawHwFailed,
                    const QString &hwdecCurrent)
{
    if (hwdecCurrent == QLatin1String("no"))
        return QStringLiteral("SW");
    if (sawSwLog || sawHwFailed)
        return QStringLiteral("SW");
    if (sawHwLog)
        return QStringLiteral("HW");
    if (!hwdecCurrent.isEmpty())
        return QStringLiteral("HW"); // respaldo: backend activo, nada en contra
    return QString();
}

} // namespace hwprobe

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
    // vd=v (veredicto del decodificador: "Using hardware decoding (...)" /
    // "Using software decoding") + ffmpeg=v (errores del intento HW, p.ej.
    // "av1_qsv: Error ... unsupported (-3)"). El módulo ffmpeg estaba en "no"
    // y por eso el probe no veía que el intento HW había fallado.
    mpv_set_option_string(m_mpv, "msg-level", "all=no,vd=v,ffmpeg=v");
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
    m_swVerdicts = 0;
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
    // Nuevo clip, nuevo veredicto: ni el log ni el estado de decode anteriores
    // cuentan para este clip.
    m_sawHwLog = false;
    m_sawSwLog = false;
    m_sawHwFailed = false;
    m_decoded = false;
    mpv_command(m_mpv, cmd);
    NT_DEBUG("E2E hwprobe load %d=%s\n", m_index, p.constData());
}

// Siguiente códec de la escalera (o fin). Un clip "indeterminado" también
// avanza: abortar dejaba al i5-4590 sin llegar a H.264, su perfil real.
void HwProbe::advance()
{
    if (m_done)
        return;
    if (m_index >= kClipCount - 1) {
        endOfLadder();
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

void HwProbe::endOfLadder()
{
    // H.264 sin HW: si algún códec dio un SW decisivo real, "soft-all" es un
    // veredicto (se persiste); si todo fue indeterminado, vacío (no persistir,
    // reintentar al arrancar).
    if (m_swVerdicts > 0) {
        finish(QStringLiteral("soft-all"));
    } else {
        NT_DEBUG("E2E hwprobe todo indeterminado -> no persistir\n");
        finish(QString());
    }
}

void HwProbe::poll()
{
    if (!m_mpv || m_done)
        return;

    // Veredicto de un clip. Requiere frames decodificados (m_decoded) y que la
    // ventana de gracia haya pasado para que lleguen los mensajes del
    // decodificador (veredicto del log y errores del intento HW). HW solo si
    // el log lo confirma; SW y UNKNOWN avanzan en la escalera.
    const auto decide = [this](const QString &hwdec) {
        const QString verdict = hwprobe::clipVerdict(
            m_sawHwLog, m_sawSwLog, m_sawHwFailed, hwdec);
        NT_DEBUG("E2E hwprobe %s hwdec=%s log(hw=%d sw=%d fail=%d) -> %s\n",
                 kClips[m_index].file, hwdec.toUtf8().constData(),
                 m_sawHwLog ? 1 : 0, m_sawSwLog ? 1 : 0,
                 m_sawHwFailed ? 1 : 0, verdict.toUtf8().constData());
        if (verdict == QLatin1String("HW")) {
            finish(QString::fromLatin1(kClips[m_index].profile));
            return;
        }
        if (verdict == QLatin1String("SW"))
            ++m_swVerdicts;
        advance();
    };

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
            auto *lm = static_cast<mpv_event_log_message *>(event->data);
            if (lm && lm->text) {
                const QString text = QString::fromUtf8(lm->text);
                if (text.contains(QLatin1String("Using hardware decoding")))
                    m_sawHwLog = true;
                else if (text.contains(QLatin1String("Using software decoding")))
                    m_sawSwLog = true;
                else if (hwAttemptFailed(QString::fromUtf8(lm->prefix), text,
                                         codecOf(m_index)))
                    m_sawHwFailed = true;
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
    if (h > 0 && !m_decoded) {
        m_decoded = true;
        m_decodedTimer.restart();
    }

    // 1) Primer frame decodificado + fin de la gracia -> decidir.
    if (m_decoded && m_decodedTimer.elapsed() >= kGraceMs) {
        decide(hwdec);
        return;
    }
    // 2) El clip terminó por error SIN llegar a decodificar: indeterminado.
    if (ended && !m_decoded) {
        NT_DEBUG("E2E hwprobe %s decode-error -> siguiente códec\n",
                 kClips[m_index].file);
        advance();
        return;
    }
    // 3) Timeout del clip (init del decodificador lento en VMs): un reintento,
    // y si tampoco, indeterminado (avanza a siguiente códec).
    if (m_clipTimer.elapsed() > kClipTimeoutMs) {
        if (!m_clipRetried) {
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
        NT_DEBUG("E2E hwprobe timeout clip=%s hwdec=%s\n",
                 kClips[m_index].file, hwdec.toUtf8().constData());
        advance();
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