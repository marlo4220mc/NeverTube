#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

// Integration with the OPERATING SYSTEM's media controls, which is what makes
// the media keys of a keyboard and the buttons of Bluetooth headphones work —
// including with the window minimized, on another workspace or on the lock
// screen — and what puts NeverTube in the desktop's media widget.
//
// Chrome and Spotify do exactly this and nothing else; there is no private
// protocol:
//
//   Linux   MPRIS v2 over D-Bus (org.mpris.MediaPlayer2.<appId>). The desktop
//           (GNOME, KDE, Hyprland, ...) or PipeWire/BlueZ translates the
//           headphone button (AVRCP) into a D-Bus call to whoever owns that
//           name. It is also what draws the player in the top bar and in the
//           lock screen.
//   Windows The System Media Transport Controls (Win10/11): the media flyout
//           with the artwork, the transport buttons and the timeline. On
//           Windows 7 (where SMTC does not exist) the hardware keys arrive as
//           WM_APPCOMMAND to the window and as a registered hot key while the
//           window is in the background.
//   other   nothing (the pure helpers below still work: the selftest runs
//           everywhere).
//
// The rules that matter, and that this module obeys:
//   - It NEVER touches playback directly: it emits "the system asked for X"
//     and MainWindow does it through PlayerController, the same public API the
//     window's own buttons use. The protected player core is not involved.
//   - All of it is optional and reversible: if there is no session bus, no
//     D-Bus plugin, no SMTC or no window yet, the module degrades to a no-op
//     and says so in the E2E log. Nothing is ever required for playback.

namespace nt {

// The state the system draws, which are the three values MPRIS can carry. The
// app's own "phase" has more ("loading", "error", "stopped"): anything that is
// not playing or paused is Stopped, which is what the spec asks of a player
// with nothing in the pipe.
enum class MediaPlayback { Stopped, Paused, Playing };

// --- Pure conversions (offline, checked by --selftest-mpris) --------------

// org.mpris.MediaPlayer2.Player.PlaybackStatus.
QString mprisPlaybackStatus(MediaPlayback state);
// LoopStatus. The app's repeat goes back to the FIRST video of the queue when
// the last one ends, so the MPRIS equivalent is "Playlist", never "Track" (a
// queue of one video looping is "Playlist" for MPRIS too).
QString mprisLoopStatus(bool repeat);
// mpris:trackid, which has to travel as a D-Bus OBJECT PATH ('o') and not as a
// string: clients compare it against the path of the last signal. It is a
// QDBusObjectPath where QtDBus exists (Linux) and a plain string elsewhere,
// where the type cannot be expressed.
QVariant mprisTrackId(const QString &trackPath);
// org.mpris.MediaPlayer2.Player.Metadata (a{sv}).
//
// durationMs = 0 means "not known" and the length is LEFT OUT: a length of 0
// makes the widgets draw a finished 0 s track instead of an unbounded one.
// An empty title falls back to the app name, because a client with an empty
// mpris:title shows a blank row.
QVariantMap mprisMetadata(const QString &title, const QString &artist,
                          const QString &artUrl, qint64 durationMs,
                          const QString &trackPath);
// Volume goes 0..1 in MPRIS and 0..100 in the app (it may pass 100 with the
// volume boost, which MPRIS allows). One conversion each way so that the
// overlay, the keyboard and the system widget cannot disagree.
double mprisVolume(double percent);
double percentFromMprisVolume(double volume);

// --- The backend actually compiled here ----------------------------------

enum class SystemMediaBackend { None, Mpris, WindowsSmtc };

SystemMediaBackend systemMediaBackend();
// "MPRIS", "SMTC" or "none". For the E2E log and for the selftest.
QString systemMediaBackendName();

// The D-Bus name the desktop and the headphones call: it comes from the
// desktop file that main.cpp already set (desktopFileName), because that is
// the ONLY name the session bus lets a sandboxed app own
// (org.mpris.MediaPlayer2.$FLATPAK_ID) and the one the desktop widget matches
// against its .desktop. Empty outside Linux.
QString mprisServiceName();

struct SystemMediaData;

class SystemMediaControls : public QObject
{
    Q_OBJECT

public:
    explicit SystemMediaControls(QObject *parent = nullptr);
    ~SystemMediaControls() override;

    // The whole state, pushed by MainWindow whenever the player changes. They
    // are cheap and idempotent: setting the same value does not produce D-Bus
    // traffic nor a SMTC call.
    void setTrack(const QString &title, const QString &artist,
                  const QString &artUrl, qint64 durationMs);
    void setStatus(MediaPlayback status);
    // Position and duration (for the system's progress bar and for the
    // Position property). It is called on every position tick, so the Windows
    // side refreshes its timeline at most once every few seconds.
    void setPosition(qint64 positionMs, qint64 durationMs);
    void setVolume(double percent);
    void setModes(bool shuffle, bool repeat);
    void setCapabilities(bool canGoNext, bool canGoPrevious, bool canSeek);

    // Windows: SMTC is created FOR A WINDOW (GetForWindow), so it cannot exist
    // before there is one. MainWindow calls this the first time it is shown.
    // On Linux this is a no-op.
    void attachToWindow(quintptr nativeHandle);

    // Whether the system integration is REALLY registered: the MPRIS name is
    // on the session bus, or the SMTC object exists. False when there is no
    // bus, no D-Bus plugin, no window yet, or the name is already taken (a
    // second instance would otherwise steal the calls of the first one).
    // MainWindow keeps working with the window's own keys either way.
    bool active() const;

signals:
    // "The system asked for this". They are what the media keys, the headphone
    // buttons, the desktop widget and (on Windows) the media flyout send.
    void playRequested();
    void pauseRequested();
    void playPauseRequested();
    void stopRequested();
    void nextRequested();
    void previousRequested();
    // Absolute, in ms (MPRIS SetPosition and the SMTC seek bar).
    void seekRequested(qint64 positionMs);
    // Relative, in ms, positive or negative (MPRIS Seek).
    void seekRelativeRequested(qint64 offsetMs);
    // 0..100, already converted from MPRIS's 0..1.
    void volumeRequested(double percent);
    void shuffleRequested(bool on);
    void repeatRequested(bool on);
    void raiseRequested();

private:
    SystemMediaData *d = nullptr;
};

} // namespace nt
