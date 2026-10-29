// The Windows branch includes C++/WinRT, which uses std::min/std::max: windows.h
// defines them as macros, so NOMINMAX has to be set before ANY header of this
// translation unit.
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#    define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#  endif
#endif

#include "system_media.h"

#include "debug.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QStringList>

#include <cmath>

#if defined(Q_OS_LINUX) && defined(NEVERTUBE_HAVE_DBUS)
#  include <QDBusAbstractAdaptor>
#  include <QDBusConnection>
#  include <QDBusError>
#  include <QDBusMessage>
#  include <QDBusObjectPath>
#endif

#if defined(_WIN32) && defined(NEVERTUBE_WIN_SMTC)
#  include <windows.h>
#  include <inspectable.h>
#  include <systemmediatransportcontrolsinterop.h>

#  include <winrt/Windows.Foundation.h>
#  include <winrt/Windows.Media.h>
#  include <winrt/Windows.Storage.Streams.h>
#  include <winrt/base.h>
#endif

// ===========================================================================
// Pure conversions (no platform of any kind: they are what --selftest-mpris
// checks, and they run on every platform).
// ===========================================================================

QString nt::mprisPlaybackStatus(MediaPlayback state)
{
    switch (state) {
    case MediaPlayback::Playing:
        return QStringLiteral("Playing");
    case MediaPlayback::Paused:
        return QStringLiteral("Paused");
    case MediaPlayback::Stopped:
        break;
    }
    return QStringLiteral("Stopped");
}

QString nt::mprisLoopStatus(bool repeat)
{
    // The app's repeat wraps to the FIRST video of the queue, which is
    // "Playlist" in MPRIS (there is no per-video loop in the app: a queue of
    // one video wrapping is "Playlist" for MPRIS too).
    return repeat ? QStringLiteral("Playlist") : QStringLiteral("None");
}

QVariant nt::mprisTrackId(const QString &trackPath)
{
#if defined(Q_OS_LINUX) && defined(NEVERTUBE_HAVE_DBUS)
    // 'o' on the wire: clients compare it with the path they already have.
    return QVariant::fromValue(QDBusObjectPath(trackPath));
#else
    // No QtDBus on this platform, so there is no object path type to express
    // it with. Only the selftest and the Windows flyout read this.
    return QVariant(trackPath);
#endif
}

QVariantMap nt::mprisMetadata(const QString &title, const QString &artist,
                              const QString &artUrl, qint64 durationMs,
                              const QString &trackPath)
{
    QVariantMap metadata;
    const QString cleanTitle = title.trimmed();
    metadata.insert(QStringLiteral("mpris:trackid"), mprisTrackId(trackPath));
    metadata.insert(QStringLiteral("xesam:title"),
                    cleanTitle.isEmpty() ? QStringLiteral("NeverTube")
                                         : cleanTitle);
    const QString cleanArtist = artist.trimmed();
    if (!cleanArtist.isEmpty())
        metadata.insert(QStringLiteral("xesam:artist"),
                        QStringList{cleanArtist});
    if (!artUrl.isEmpty())
        metadata.insert(QStringLiteral("mpris:artUrl"), artUrl);
    // µs, and only when it is known: a length of 0 is drawn as a finished
    // 0 s track by the widgets instead of an unbounded one.
    if (durationMs > 0)
        metadata.insert(QStringLiteral("mpris:length"), durationMs * 1000);
    return metadata;
}

double nt::mprisVolume(double percent)
{
    // 0..100 in the app (it can pass 100 with the volume boost, which MPRIS
    // allows: the spec asks 0..1 for the normal range, not a hard clamp).
    return qMax(0.0, percent) / 100.0;
}

double nt::percentFromMprisVolume(double volume)
{
    if (std::isnan(volume) || volume < 0.0)
        return 0.0;
    return volume * 100.0;
}

// ===========================================================================
// Shared state. The platform handles live inside the per-platform #if, so the
// rest of this file (the setters and the signals) is written once.
// ===========================================================================

struct nt::SystemMediaData
{
    nt::SystemMediaControls *owner = nullptr;

    QString title;
    QString artist;
    QString artUrl;
    qint64 durationMs = 0;
    qint64 positionMs = 0;
    double volumePercent = 100.0;
    MediaPlayback status = MediaPlayback::Stopped;
    bool shuffle = false;
    bool repeat = false;
    bool canNext = false;
    bool canPrevious = false;
    bool canSeek = false;
    quintptr windowHandle = 0;
    // Identity of the current track. Bumped when the video really changes, so
    // that a client knows the progress bar has to start over.
    qint64 trackNumber = 1;

    QString trackPath() const
    {
        return QStringLiteral("/org/mpris/MediaPlayer2/Track/%1")
            .arg(trackNumber);
    }

#if defined(Q_OS_LINUX) && defined(NEVERTUBE_HAVE_DBUS)
    QObject *mprisObject = nullptr;
    bool busOwned = false;
#elif defined(_WIN32) && defined(NEVERTUBE_WIN_SMTC)
    winrt::Windows::Media::SystemMediaTransportControls controls{nullptr};
    winrt::event_token buttonToken{};
    bool smtcReady = false;
    qint64 lastTimelineAt = 0;
    qint64 lastTimelinePos = -1;
#endif
};

namespace nt {

// Per-platform hooks. They are declared here and defined once per backend, so
// the setters below have no #if inside them.
void smPlatformInit(SystemMediaControls *owner, SystemMediaData *data);
void smPlatformShutdown(SystemMediaData *data);
void smPushTrack(SystemMediaData *data);
void smPushStatus(SystemMediaData *data);
void smPushVolume(SystemMediaData *data);
void smPushModes(SystemMediaData *data);
void smPushCapabilities(SystemMediaData *data);
void smPushPosition(SystemMediaData *data, bool force);
void smAttachWindow(SystemMediaData *data, quintptr windowHandle);

// The name of the app on the bus and in the desktop's file. It comes from the
// desktop file that main.cpp already set: under Flatpak the session bus ONLY
// lets the app own org.mpris.MediaPlayer2.$FLATPAK_ID (that is the default
// policy), and there the desktop file is named after the app id for the same
// reason.
QString appIdName()
{
    QString name = QGuiApplication::desktopFileName();
    if (name.endsWith(QLatin1String(".desktop")))
        name.chop(8);
    if (name.isEmpty())
        name = QStringLiteral("nevertube");
    return name;
}

QString mprisServiceName()
{
    return QStringLiteral("org.mpris.MediaPlayer2.") + appIdName();
}

} // namespace nt

// ===========================================================================
// Setters and the public API (platform independent)
// ===========================================================================

nt::SystemMediaControls::SystemMediaControls(QObject *parent)
    : QObject(parent), d(new SystemMediaData)
{
    d->owner = this;
    smPlatformInit(this, d);
    NT_DEBUG("E2E system media: backend=%s\n",
             systemMediaBackendName().toUtf8().constData());
}

nt::SystemMediaControls::~SystemMediaControls()
{
    smPlatformShutdown(d);
    delete d;
}

void nt::SystemMediaControls::setTrack(const QString &title,
                                       const QString &artist,
                                       const QString &artUrl,
                                       qint64 durationMs)
{
    const QString cleanTitle = title.trimmed();
    const QString cleanArtist = artist.trimmed();
    const qint64 cleanDuration = qMax<qint64>(0, durationMs);
    if (d->title == cleanTitle && d->artist == cleanArtist
        && d->artUrl == artUrl && d->durationMs == cleanDuration)
        return;
    // Another video: new identity. The duration arriving later (mpv reports it
    // once the medium is open) must NOT change it, or the progress bar of the
    // widget would restart when the length is known.
    if (!d->title.isEmpty() && d->title != cleanTitle)
        ++d->trackNumber;
    d->title = cleanTitle;
    d->artist = cleanArtist;
    d->artUrl = artUrl;
    d->durationMs = cleanDuration;
    if (d->durationMs == 0)
        d->positionMs = 0;
    smPushTrack(d);
}

void nt::SystemMediaControls::setStatus(MediaPlayback status)
{
    if (d->status == status)
        return;
    d->status = status;
    smPushStatus(d);
}

void nt::SystemMediaControls::setPosition(qint64 positionMs, qint64 durationMs)
{
    d->positionMs = qMax<qint64>(0, positionMs);
    if (durationMs > 0 && d->durationMs != durationMs) {
        d->durationMs = durationMs;
        smPushTrack(d); // the length is part of the metadata/timeline
    }
    smPushPosition(d, false);
}

void nt::SystemMediaControls::setVolume(double percent)
{
    const double clean = qMax(0.0, percent);
    if (qFuzzyCompare(d->volumePercent + 1.0, clean + 1.0))
        return;
    d->volumePercent = clean;
    smPushVolume(d);
}

void nt::SystemMediaControls::setModes(bool shuffle, bool repeat)
{
    if (d->shuffle == shuffle && d->repeat == repeat)
        return;
    d->shuffle = shuffle;
    d->repeat = repeat;
    smPushModes(d);
}

void nt::SystemMediaControls::setCapabilities(bool canGoNext,
                                              bool canGoPrevious,
                                              bool canSeek)
{
    if (d->canNext == canGoNext && d->canPrevious == canGoPrevious
        && d->canSeek == canSeek)
        return;
    d->canNext = canGoNext;
    d->canPrevious = canGoPrevious;
    d->canSeek = canSeek;
    smPushCapabilities(d);
}

void nt::SystemMediaControls::attachToWindow(quintptr nativeHandle)
{
    if (nativeHandle == 0 || d->windowHandle == nativeHandle)
        return;
    d->windowHandle = nativeHandle;
    smAttachWindow(d, nativeHandle);
}

bool nt::SystemMediaControls::active() const
{
#if defined(Q_OS_LINUX) && defined(NEVERTUBE_HAVE_DBUS)
    return d && d->busOwned;
#elif defined(_WIN32) && defined(NEVERTUBE_WIN_SMTC)
    return d && d->smtcReady;
#else
    return false;
#endif
}

// ===========================================================================
// Linux: MPRIS v2 over D-Bus
// ===========================================================================

#if defined(Q_OS_LINUX) && defined(NEVERTUBE_HAVE_DBUS)

namespace nt {

namespace {

// "/org/mpris/MediaPlayer2" is the object path the spec fixes; there is no
// per-app path.
const char *const kMprisPath = "/org/mpris/MediaPlayer2";
const char *const kMprisPlayerInterface = "org.mpris.MediaPlayer2.Player";

// MPRIS requires an explicit PropertiesChanged on org.freedesktop.DBus.
// Properties: QtDBus answers Get/GetAll by itself (from the Q_PROPERTYs of the
// adaptor) but it does NOT notify, so the widgets would keep the old title.
void notifyProperties(SystemMediaData *data, const QVariantMap &changed)
{
    if (!data->busOwned || changed.isEmpty())
        return;
    QDBusMessage signal = QDBusMessage::createSignal(
        QLatin1String(kMprisPath),
        QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("PropertiesChanged"));
    signal << QLatin1String(kMprisPlayerInterface) << changed << QStringList();
    QDBusConnection::sessionBus().send(signal);
}

} // namespace

// --- org.mpris.MediaPlayer2 (identity, and "come to the front") -----------
class MprisRootAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)

public:
    MprisRootAdaptor(SystemMediaData *data, QObject *parent)
        : QDBusAbstractAdaptor(parent), m_data(data)
    {
    }

    // The app is not a media player that can be closed from the widget:
    // closing it is not a media action. The spec accepts a false CanQuit.
    bool canQuit() const { return false; }
    bool canRaise() const { return true; }
    bool hasTrackList() const { return false; }
    QString identity() const { return QStringLiteral("NeverTube"); }
    // Base name WITHOUT the .desktop suffix, as the spec asks.
    QString desktopEntry() const { return appIdName(); }
    QStringList supportedUriSchemes() const
    {
        return {QStringLiteral("http"), QStringLiteral("https")};
    }
    QStringList supportedMimeTypes() const { return {}; }

public slots:
    void Raise()
    {
        // "Take me back to the app": what the media widget's button does.
        if (m_data->owner)
            m_data->owner->raiseRequested();
    }
    // Present for the introspection of every player, and harmless: with
    // CanQuit false no client should call it, and one that does gets nothing.
    void Quit() {}

private:
    SystemMediaData *m_data;
};

// --- org.mpris.MediaPlayer2.Player (the transport) ------------------------
class MprisPlayerAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QString LoopStatus READ loopStatus WRITE setLoopStatus)
    Q_PROPERTY(double Rate READ rate)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffleProperty)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolumeProperty)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ minimumRate)
    Q_PROPERTY(double MaximumRate READ maximumRate)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPause)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ canControl)

public:
    MprisPlayerAdaptor(SystemMediaData *data, QObject *parent)
        : QDBusAbstractAdaptor(parent), m_data(data)
    {
    }

    QString playbackStatus() const { return mprisPlaybackStatus(m_data->status); }
    QString loopStatus() const { return mprisLoopStatus(m_data->repeat); }
    void setLoopStatus(const QString &status)
    {
        if (!m_data->owner)
            return;
        // "Track" does not exist in the app (its repeat wraps the whole
        // queue), so both of them mean the same here.
        m_data->owner->repeatRequested(status == QLatin1String("Playlist")
                                       || status == QLatin1String("Track"));
    }
    double rate() const { return 1.0; }
    bool shuffle() const { return m_data->shuffle; }
    void setShuffleProperty(bool on)
    {
        if (m_data->owner)
            m_data->owner->shuffleRequested(on);
    }
    QVariantMap metadata() const
    {
        return mprisMetadata(m_data->title, m_data->artist, m_data->artUrl,
                             m_data->durationMs, m_data->trackPath());
    }
    double volume() const { return mprisVolume(m_data->volumePercent); }
    void setVolumeProperty(double volume)
    {
        if (m_data->owner)
            m_data->owner->volumeRequested(percentFromMprisVolume(volume));
    }
    // µs on the wire. It is read on demand: MPRIS does not want Position in
    // PropertiesChanged (it changes every second).
    qlonglong position() const { return m_data->positionMs * 1000; }
    double minimumRate() const { return 1.0; }
    double maximumRate() const { return 1.0; }
    bool canGoNext() const { return m_data->canNext; }
    bool canGoPrevious() const { return m_data->canPrevious; }
    bool canPlay() const { return true; }
    bool canPause() const { return true; }
    bool canSeek() const { return m_data->canSeek; }
    bool canControl() const { return true; }

public slots:
    void Next()
    {
        if (!m_data->canNext || !m_data->owner)
            return;
        m_data->owner->nextRequested();
    }
    void Previous()
    {
        if (!m_data->canPrevious || !m_data->owner)
            return;
        m_data->owner->previousRequested();
    }
    void Pause()
    {
        if (m_data->status != MediaPlayback::Playing || !m_data->owner)
            return;
        m_data->owner->pauseRequested();
    }
    void PlayPause()
    {
        if (m_data->owner)
            m_data->owner->playPauseRequested();
    }
    void Stop()
    {
        if (m_data->owner)
            m_data->owner->stopRequested();
    }
    void Play()
    {
        if (m_data->status == MediaPlayback::Playing || !m_data->owner)
            return;
        m_data->owner->playRequested();
    }
    void Seek(qlonglong offset)
    {
        if (!m_data->canSeek || !m_data->owner)
            return;
        const qint64 offsetMs = offset / 1000;
        m_data->owner->seekRelativeRequested(offsetMs);
        emit Seeked((m_data->positionMs + offsetMs) * 1000);
    }
    void SetPosition(const QDBusObjectPath &trackId, qlonglong position)
    {
        // The track id has to match the one we advertised, or the request is
        // for a video that is no longer playing.
        if (!m_data->owner || trackId.path() != m_data->trackPath())
            return;
        const qint64 ms = qMax<qint64>(0, position / 1000);
        m_data->owner->seekRequested(ms);
        emit Seeked(ms * 1000);
    }
    // The app does not open URIs on request for anyone on the bus.
    void OpenUri(const QString &) {}

signals:
    // Relayed to D-Bus by QDBusAbstractAdaptor: the widgets use it to move the
    // progress bar without waiting for the next poll.
    void Seeked(qlonglong position);

private:
    SystemMediaData *m_data;
};

// The object the two interfaces hang from. QtDBus exports the adaptors that
// are CHILDREN of it.
class MprisObject : public QObject
{
    Q_OBJECT

public:
    explicit MprisObject(SystemMediaData *data)
    {
        m_root = new MprisRootAdaptor(data, this);
        m_player = new MprisPlayerAdaptor(data, this);
    }

private:
    MprisRootAdaptor *m_root = nullptr;
    MprisPlayerAdaptor *m_player = nullptr;
};

void smPlatformInit(SystemMediaControls *owner, SystemMediaData *data)
{
    Q_UNUSED(owner)
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        NT_DEBUG("E2E system media: no session bus, MPRIS apagado\n");
        return;
    }
    auto *object = new MprisObject(data);
    if (!bus.registerObject(QLatin1String(kMprisPath), object,
                            QDBusConnection::ExportAdaptors)) {
        NT_DEBUG("E2E system media: registerObject fallo (%s)\n",
                 bus.lastError().message().toUtf8().constData());
        delete object;
        return;
    }
    // The bus name is what the desktop and the headphones look for. If it is
    // taken (another instance, another user) the object is dropped: a call to
    // that name would go to the OTHER process and this adaptor would never see
    // it, which is worse than having no MPRIS at all.
    if (!bus.registerService(mprisServiceName())) {
        NT_DEBUG("E2E system media: %s ocupado (%s)\n",
                 mprisServiceName().toUtf8().constData(),
                 bus.lastError().message().toUtf8().constData());
        bus.unregisterObject(QLatin1String(kMprisPath));
        delete object;
        return;
    }
    data->mprisObject = object;
    data->busOwned = true;
    NT_DEBUG("E2E system media: MPRIS v2 %s\n",
             mprisServiceName().toUtf8().constData());
}

void smPlatformShutdown(SystemMediaData *data)
{
    if (!data->busOwned)
        return;
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.unregisterService(mprisServiceName());
    bus.unregisterObject(QLatin1String(kMprisPath));
    data->busOwned = false;
    delete data->mprisObject;
    data->mprisObject = nullptr;
}

void smAttachWindow(SystemMediaData *data, quintptr windowHandle)
{
    Q_UNUSED(data)
    Q_UNUSED(windowHandle)
    // MPRIS is not tied to a window.
}

void smPushTrack(SystemMediaData *data)
{
    QVariantMap changed;
    changed.insert(QStringLiteral("Metadata"),
                   mprisMetadata(data->title, data->artist, data->artUrl,
                                 data->durationMs, data->trackPath()));
    notifyProperties(data, changed);
}

void smPushStatus(SystemMediaData *data)
{
    QVariantMap changed;
    changed.insert(QStringLiteral("PlaybackStatus"),
                   mprisPlaybackStatus(data->status));
    notifyProperties(data, changed);
}

void smPushVolume(SystemMediaData *data)
{
    QVariantMap changed;
    changed.insert(QStringLiteral("Volume"), mprisVolume(data->volumePercent));
    notifyProperties(data, changed);
}

void smPushModes(SystemMediaData *data)
{
    QVariantMap changed;
    changed.insert(QStringLiteral("LoopStatus"), mprisLoopStatus(data->repeat));
    changed.insert(QStringLiteral("Shuffle"), data->shuffle);
    notifyProperties(data, changed);
}

void smPushCapabilities(SystemMediaData *data)
{
    QVariantMap changed;
    changed.insert(QStringLiteral("CanGoNext"), data->canNext);
    changed.insert(QStringLiteral("CanGoPrevious"), data->canPrevious);
    changed.insert(QStringLiteral("CanSeek"), data->canSeek);
    notifyProperties(data, changed);
}

void smPushPosition(SystemMediaData *data, bool force)
{
    Q_UNUSED(data)
    Q_UNUSED(force)
    // MPRIS deliberately leaves Position out of PropertiesChanged: clients
    // read the property (which is answered from memory) when they paint the
    // progress bar.
}

} // namespace nt

// ===========================================================================
// Windows 10/11: System Media Transport Controls (the media flyout)
// ===========================================================================

#elif defined(_WIN32) && defined(NEVERTUBE_WIN_SMTC)

namespace nt {

namespace {

using winrt::Windows::Media::MediaPlaybackStatus;
using winrt::Windows::Media::MediaPlaybackType;
using winrt::Windows::Media::SystemMediaTransportControls;
using winrt::Windows::Media::SystemMediaTransportControlsButton;
using winrt::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs;
using winrt::Windows::Media::SystemMediaTransportControlsTimelineProperties;
using winrt::Windows::Foundation::TimeSpan;

// 100 ns ticks, which is what a WinRT TimeSpan is.
TimeSpan ticks(qint64 value)
{
    return TimeSpan{value * 10};
}

} // namespace

void smPlatformInit(SystemMediaControls *owner, SystemMediaData *data)
{
    Q_UNUSED(owner)
    Q_UNUSED(data)
    // Nothing yet: SMTC needs the HWND (smAttachWindow).
}

void smAttachWindow(SystemMediaData *data, quintptr windowHandle)
{
    if (data->smtcReady || windowHandle == 0)
        return;
    // In headless/offscreen test runs there is no desktop session or valid shell.
    if (QGuiApplication::platformName() == QLatin1String("offscreen"))
        return;
    HWND hwnd = reinterpret_cast<HWND>(windowHandle);
    if (!IsWindow(hwnd))
        return;
    try {
        // Qt already initialized COM on the GUI thread (OLE, apartment
        // threaded). This is only to be sure; a different apartment is not an
        // error for the calls below.
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (...) {
    }
    try {
        auto interop = winrt::get_activation_factory<SystemMediaTransportControls,
                                                     ISystemMediaTransportControlsInterop>();
        void *raw = nullptr;
        winrt::check_hresult(interop->GetForWindow(
            hwnd,
            winrt::guid_of<SystemMediaTransportControls>(),
            &raw));
        winrt::attach_abi(data->controls, raw);

        data->controls.IsEnabled(true);
        data->controls.IsPlayEnabled(true);
        data->controls.IsPauseEnabled(true);
        data->controls.IsStopEnabled(true);
        data->controls.IsNextEnabled(false);
        data->controls.IsPreviousEnabled(false);
        data->controls.PlaybackStatus(MediaPlaybackStatus::Stopped);

        // The keys of the keyboard and of the headphones arrive here while
        // NeverTube is the current media app (Windows arbitrates between the
        // apps, like it does for Chrome): it is the same thing the media
        // flyout's buttons send.
        data->buttonToken = data->controls.ButtonPressed(
            [data](SystemMediaTransportControls const &,
                   SystemMediaTransportControlsButtonPressedEventArgs const &args) {
                const SystemMediaTransportControlsButton button = args.Button();
                // Queued: the handler may run inside a COM call and the player
                // must not be driven from there.
                QMetaObject::invokeMethod(
                    data->owner,
                    [data, button]() {
                        SystemMediaControls *owner = data->owner;
                        if (!owner)
                            return;
                        switch (button) {
                        case SystemMediaTransportControlsButton::Play:
                            owner->playRequested();
                            break;
                        case SystemMediaTransportControlsButton::Pause:
                            owner->pauseRequested();
                            break;
                        case SystemMediaTransportControlsButton::Stop:
                            owner->stopRequested();
                            break;
                        case SystemMediaTransportControlsButton::Next:
                            owner->nextRequested();
                            break;
                        case SystemMediaTransportControlsButton::Previous:
                            owner->previousRequested();
                            break;
                        default:
                            break;
                        }
                    },
                    Qt::QueuedConnection);
            });
        data->smtcReady = true;
        NT_DEBUG("E2E system media: SMTC listo para hwnd=%p\n",
                 reinterpret_cast<void *>(windowHandle));
    } catch (winrt::hresult_error const &error) {
        data->smtcReady = false;
        NT_DEBUG("E2E system media: SMTC no disponible (0x%08x)\n",
                 static_cast<unsigned>(error.code()));
    }
}

void smPlatformShutdown(SystemMediaData *data)
{
    if (!data->smtcReady)
        return;
    try {
        data->controls.ButtonPressed(data->buttonToken);
        data->controls.IsEnabled(false);
        data->controls = nullptr;
    } catch (...) {
    }
    data->smtcReady = false;
}

void smPushTrack(SystemMediaData *data)
{
    if (!data->smtcReady)
        return;
    try {
        auto updater = data->controls.DisplayUpdater();
        updater.Type(MediaPlaybackType::Video);
        auto video = updater.VideoProperties();
        const QString title = data->title.isEmpty()
            ? QStringLiteral("NeverTube")
            : data->title;
        video.Title(winrt::hstring(title.toStdWString()));
        video.Subtitle(winrt::hstring(data->artist.toStdWString()));
        if (!data->artUrl.isEmpty()) {
            updater.Thumbnail(
                winrt::Windows::Storage::Streams::RandomAccessStreamReference::
                    CreateFromUri(winrt::Windows::Foundation::Uri(
                        winrt::hstring(data->artUrl.toStdWString()))));
        }
        updater.Update();
    } catch (winrt::hresult_error const &) {
        // The artwork is cosmetic (a thumbnail that does not load, a URI the
        // system refuses): the transport keeps working.
    }
    smPushPosition(data, true);
}

void smPushStatus(SystemMediaData *data)
{
    if (!data->smtcReady)
        return;
    MediaPlaybackStatus status = MediaPlaybackStatus::Stopped;
    if (data->status == MediaPlayback::Playing)
        status = MediaPlaybackStatus::Playing;
    else if (data->status == MediaPlayback::Paused)
        status = MediaPlaybackStatus::Paused;
    try {
        data->controls.PlaybackStatus(status);
    } catch (winrt::hresult_error const &) {
    }
    // The system extrapolates the position from the moment of the update, so
    // the timeline is refreshed whenever the state changes.
    smPushPosition(data, true);
}

void smPushVolume(SystemMediaData *data)
{
    Q_UNUSED(data)
    // The flyout has no volume of its own: Windows drives the audio endpoint.
}

void smPushModes(SystemMediaData *data)
{
    Q_UNUSED(data)
    // SMTC has no shuffle/repeat state.
}

void smPushCapabilities(SystemMediaData *data)
{
    if (!data->smtcReady)
        return;
    try {
        data->controls.IsNextEnabled(data->canNext);
        data->controls.IsPreviousEnabled(data->canPrevious);
    } catch (winrt::hresult_error const &) {
    }
}

void smPushPosition(SystemMediaData *data, bool force)
{
    if (!data->smtcReady)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Every position tick would be a COM call per second for a bar that the
    // system interpolates by itself. It is refreshed on a seek or every 5 s.
    const bool seeked = data->lastTimelinePos < 0
        || qAbs(data->positionMs - data->lastTimelinePos) > 5000;
    if (!force && !seeked && now - data->lastTimelineAt < 5000)
        return;
    data->lastTimelineAt = now;
    data->lastTimelinePos = data->positionMs;
    const qint64 endMs = qMax<qint64>(data->durationMs, data->positionMs);
    try {
        SystemMediaTransportControlsTimelineProperties timeline;
        timeline.StartTime(ticks(0));
        timeline.MinSeekTime(ticks(0));
        timeline.Position(ticks(data->positionMs));
        timeline.EndTime(ticks(endMs));
        timeline.MaxSeekTime(ticks(endMs));
        data->controls.UpdateTimelineProperties(timeline);
    } catch (winrt::hresult_error const &) {
    }
}

} // namespace nt

// ===========================================================================
// Anywhere else (and the Windows 7 build, where SMTC does not exist): the
// module is a no-op. Windows 7 still gets the hardware keys through
// WM_APPCOMMAND and the registered hot key (MainWindow::nativeEvent).
// ===========================================================================

#else

namespace nt {

void smPlatformInit(SystemMediaControls *owner, SystemMediaData *data)
{
    Q_UNUSED(owner)
    Q_UNUSED(data)
}

void smPlatformShutdown(SystemMediaData *data) { Q_UNUSED(data) }

void smAttachWindow(SystemMediaData *data, quintptr windowHandle)
{
    Q_UNUSED(data)
    Q_UNUSED(windowHandle)
}

void smPushTrack(SystemMediaData *data) { Q_UNUSED(data) }
void smPushStatus(SystemMediaData *data) { Q_UNUSED(data) }
void smPushVolume(SystemMediaData *data) { Q_UNUSED(data) }
void smPushModes(SystemMediaData *data) { Q_UNUSED(data) }
void smPushCapabilities(SystemMediaData *data) { Q_UNUSED(data) }

void smPushPosition(SystemMediaData *data, bool force)
{
    Q_UNUSED(data)
    Q_UNUSED(force)
}

} // namespace nt

#endif

// ===========================================================================
// Which backend this binary carries
// ===========================================================================

nt::SystemMediaBackend nt::systemMediaBackend()
{
#if defined(Q_OS_LINUX) && defined(NEVERTUBE_HAVE_DBUS)
    return SystemMediaBackend::Mpris;
#elif defined(_WIN32) && defined(NEVERTUBE_WIN_SMTC)
    return SystemMediaBackend::WindowsSmtc;
#else
    return SystemMediaBackend::None;
#endif
}

QString nt::systemMediaBackendName()
{
    switch (systemMediaBackend()) {
    case SystemMediaBackend::Mpris:
        return QStringLiteral("MPRIS");
    case SystemMediaBackend::WindowsSmtc:
        return QStringLiteral("SMTC");
    case SystemMediaBackend::None:
        break;
    }
    return QStringLiteral("none");
}

#if defined(Q_OS_LINUX) && defined(NEVERTUBE_HAVE_DBUS)
#  include "system_media.moc"
#endif
