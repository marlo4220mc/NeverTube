#include "system_media_selftest.h"

#include "system_media.h"

#include <QStringList>
#include <QVariantMap>

#include <cmath>
#include <cstdio>

#if defined(Q_OS_LINUX) && defined(NEVERTUBE_HAVE_DBUS)
#  include <QCoreApplication>
#  include <QDBusArgument>
#  include <QDBusConnection>
#  include <QDBusMessage>
#  include <QDBusObjectPath>
#  include <QDBusPendingCall>
#  include <QDBusPendingCallWatcher>
#  include <QElapsedTimer>
#  include <QThread>

#  include <functional>
#endif

namespace {

#if defined(Q_OS_LINUX) && defined(NEVERTUBE_HAVE_DBUS)

// Waits for a D-Bus reply WITHOUT blocking the event loop: a blocking call to
// a service of our own process would wait for a reply that only this same
// event loop can produce.
QDBusMessage waitFor(QDBusPendingCall pending, int timeoutMs)
{
    QDBusPendingCallWatcher watcher(pending);
    QDBusMessage reply;
    bool done = false;
    QObject::connect(&watcher, &QDBusPendingCallWatcher::finished,
                     [&reply, &done](QDBusPendingCallWatcher *call) {
                         reply = call->reply();
                         done = true;
                     });
    QElapsedTimer timer;
    timer.start();
    while (!done && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return reply;
}

void pumpUntil(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
}

// The round trip against the REAL service, on the session bus of this
// machine. It is skipped (with an [info], not a failure) when there is no bus
// or when the name is taken — a running NeverTube owns it, and talking to it
// from a test would send PlayPause to the video the person is watching.
void liveChecks(bool &ok)
{
    auto check = [&ok](bool cond, const char *what) {
        std::printf("  MPRIS %s: %s\n", cond ? "OK" : "FAIL", what);
        if (!cond)
            ok = false;
    };
    auto info = [](const char *what) {
        std::printf("  MPRIS [info]: %s\n", what);
    };

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        info("no hay bus de sesion en esta maquina: se omite el round-trip");
        return;
    }

    nt::SystemMediaControls controls;
    if (!controls.active()) {
        info("el nombre del bus ya esta tomado (¿la app abierta?): "
             "se omite el round-trip");
        return;
    }
    check(true, "el servicio MPRIS se registra en el bus de sesion");

    const QString service = nt::mprisServiceName();
    const QString path = QStringLiteral("/org/mpris/MediaPlayer2");

    int paused = 0;
    QObject::connect(&controls, &nt::SystemMediaControls::playPauseRequested,
                     [&paused]() { ++paused; });

    controls.setTrack(QStringLiteral("Un video de prueba"),
                      QStringLiteral("Un canal de prueba"),
                      QString(), 90000);
    controls.setStatus(nt::MediaPlayback::Playing);
    controls.setCapabilities(true, false, true);

    QDBusMessage getAll = QDBusMessage::createMethodCall(
        service, path, QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("GetAll"));
    getAll << QStringLiteral("org.mpris.MediaPlayer2.Player");
    const QDBusMessage reply = waitFor(bus.asyncCall(getAll), 3000);
    check(!reply.arguments().isEmpty(), "Properties.GetAll contesta");
    const QVariantMap props = qdbus_cast<QVariantMap>(reply.arguments().value(0));

    check(props.value(QStringLiteral("PlaybackStatus")).toString()
              == QLatin1String("Playing"),
          "PlaybackStatus es 'Playing' en el bus");
    check(props.value(QStringLiteral("CanGoNext")).toBool(),
          "CanGoNext refleja el estado del reproductor");
    check(props.value(QStringLiteral("CanGoPrevious")).toBool() == false,
          "CanGoPrevious refleja el estado del reproductor");
    check(props.value(QStringLiteral("CanSeek")).toBool(),
          "CanSeek refleja el estado del reproductor");

    const QVariantMap metadata =
        qdbus_cast<QVariantMap>(props.value(QStringLiteral("Metadata")));
    check(metadata.value(QStringLiteral("xesam:title")).toString()
              == QLatin1String("Un video de prueba"),
          "Metadata lleva el titulo");
    check(metadata.value(QStringLiteral("xesam:artist")).toStringList()
              == QStringList{QStringLiteral("Un canal de prueba")},
          "Metadata lleva el canal como artista");
    check(metadata.value(QStringLiteral("mpris:length")).toLongLong()
              == 90000000,
          "Metadata lleva la duracion en microsegundos");
    check(metadata.value(QStringLiteral("mpris:trackid")).canConvert<
                  QDBusObjectPath>(),
          "mpris:trackid viaja como object path ('o')");

    // The transport: a real call over the bus has to reach this same process.
    QDBusMessage playPause = QDBusMessage::createMethodCall(
        service, path, QStringLiteral("org.mpris.MediaPlayer2.Player"),
        QStringLiteral("PlayPause"));
    waitFor(bus.asyncCall(playPause), 3000);
    pumpUntil([&paused]() { return paused > 0; }, 3000);
    check(paused == 1, "PlayPause llega a la app como una peticion");

    // Stopped: the state the widgets draw when nothing is playing.
    controls.setStatus(nt::MediaPlayback::Stopped);
    controls.setTrack(QString(), QString(), QString(), 0);
    const QDBusMessage reply2 = waitFor(bus.asyncCall(getAll), 3000);
    const QVariantMap props2 = qdbus_cast<QVariantMap>(reply2.arguments().value(0));
    check(props2.value(QStringLiteral("PlaybackStatus")).toString()
              == QLatin1String("Stopped"),
          "PlaybackStatus pasa a 'Stopped'");
    const QVariantMap metadata2 =
        qdbus_cast<QVariantMap>(props2.value(QStringLiteral("Metadata")));
    check(metadata2.value(QStringLiteral("xesam:title")).toString()
              == QLatin1String("NeverTube"),
          "sin video el titulo cae al nombre de la app");
}

#endif // Q_OS_LINUX && NEVERTUBE_HAVE_DBUS

} // namespace

bool systemMediaSelfTest()
{
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        std::printf("  MEDIA %s: %s\n", cond ? "OK" : "FAIL", what);
        if (!cond)
            ok = false;
    };

    // --- playback status ---
    check(nt::mprisPlaybackStatus(nt::MediaPlayback::Playing)
              == QLatin1String("Playing"),
          "Playing se llama 'Playing' en MPRIS");
    check(nt::mprisPlaybackStatus(nt::MediaPlayback::Paused)
              == QLatin1String("Paused"),
          "Paused se llama 'Paused' en MPRIS");
    check(nt::mprisPlaybackStatus(nt::MediaPlayback::Stopped)
              == QLatin1String("Stopped"),
          "Stopped se llama 'Stopped' en MPRIS");

    // --- loop / shuffle ---
    check(nt::mprisLoopStatus(true) == QLatin1String("Playlist"),
          "el repeat de la app es 'Playlist' (vuelve al primer video)");
    check(nt::mprisLoopStatus(false) == QLatin1String("None"),
          "sin repeat el LoopStatus es 'None'");

    // --- track id ---
    const QVariant trackId = nt::mprisTrackId(
        QStringLiteral("/org/mpris/MediaPlayer2/Track/7"));
#if defined(Q_OS_LINUX) && defined(NEVERTUBE_HAVE_DBUS)
    check(trackId.value<QDBusObjectPath>().path()
              == QLatin1String("/org/mpris/MediaPlayer2/Track/7"),
          "el track id es el path que se le pasa");
    check(QString::fromLatin1(trackId.typeName()) == QLatin1String("QDBusObjectPath"),
          "en D-Bus el track id es un object path, no una cadena");
#else
    check(trackId.toString()
              == QLatin1String("/org/mpris/MediaPlayer2/Track/7"),
          "el track id es el path que se le pasa");
    check(!trackId.isNull(), "el track id existe aunque no haya QtDBus");
#endif

    // --- metadata ---
    const QString path = QStringLiteral("/org/mpris/MediaPlayer2/Track/3");
    const QVariantMap meta = nt::mprisMetadata(
        QStringLiteral("  Titulo  "), QStringLiteral("Canal"),
        QStringLiteral("https://i.ytimg.com/x.jpg"), 60000, path);
    check(meta.value(QStringLiteral("xesam:title")).toString()
              == QLatin1String("Titulo"),
          "el titulo va recortado en xesam:title");
    check(meta.value(QStringLiteral("xesam:artist")).toStringList()
              == QStringList{QStringLiteral("Canal")},
          "el canal va como lista de un artista");
    check(meta.value(QStringLiteral("mpris:artUrl")).toString()
              == QLatin1String("https://i.ytimg.com/x.jpg"),
          "la miniatura va en mpris:artUrl");
    check(meta.value(QStringLiteral("mpris:length")).toLongLong() == 60000000,
          "la duracion va en MICROsegundos (60 s = 60000000)");
#if defined(Q_OS_LINUX) && defined(NEVERTUBE_HAVE_DBUS)
    check(meta.value(QStringLiteral("mpris:trackid")).value<QDBusObjectPath>().path() == path,
          "el track id de la metadata es el del video");
#else
    check(meta.value(QStringLiteral("mpris:trackid")).toString() == path,
          "el track id de la metadata es el del video");
#endif

    const QVariantMap noDuration = nt::mprisMetadata(
        QStringLiteral("Otro"), QString(), QString(), 0, path);
    check(!noDuration.contains(QStringLiteral("mpris:length")),
          "sin duracion conocida NO se manda la longitud");
    check(!noDuration.contains(QStringLiteral("xesam:artist")),
          "sin canal no se inventa un artista");
    check(!noDuration.contains(QStringLiteral("mpris:artUrl")),
          "sin miniatura no se manda artUrl");
    check(nt::mprisMetadata(QString(), QString(), QString(), 0, path)
                  .value(QStringLiteral("xesam:title"))
                  .toString()
              == QLatin1String("NeverTube"),
          "sin titulo el widget muestra el nombre de la app");

    // --- volume (0..1 in MPRIS, 0..100 in the app) ---
    check(qFuzzyCompare(nt::mprisVolume(50.0), 0.5),
          "50 % de volumen es 0.5 en MPRIS");
    check(qFuzzyCompare(nt::mprisVolume(0.0), 0.0), "0 % es 0.0");
    check(nt::mprisVolume(-10.0) >= 0.0, "un volumen negativo no viaja negativo");
    check(qFuzzyCompare(nt::percentFromMprisVolume(0.25), 25.0),
          "0.25 en MPRIS es 25 % en la app");
    check(qFuzzyCompare(nt::percentFromMprisVolume(6.0), 600.0),
          "la conversion de vuelta no recorta el volumen boost");
    check(qFuzzyCompare(nt::percentFromMprisVolume(-1.0), 0.0),
          "un volumen negativo de MPRIS cae a 0");
    check(qFuzzyCompare(nt::percentFromMprisVolume(
                            std::nan("")), 0.0),
          "NaN no envenena el volumen");

    // --- the backend this binary carries ---
    const QString backend = nt::systemMediaBackendName();
    std::printf("  MEDIA [info]: backend compilado = %s\n",
                backend.toUtf8().constData());
    check(backend == QLatin1String("MPRIS")
              || backend == QLatin1String("SMTC")
              || backend == QLatin1String("none"),
          "el backend se identifica con un nombre conocido");

#if defined(Q_OS_LINUX) && defined(NEVERTUBE_HAVE_DBUS)
    liveChecks(ok);
#endif

    return ok;
}
