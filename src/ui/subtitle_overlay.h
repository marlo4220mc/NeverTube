#pragma once

#include <QNetworkAccessManager>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>

namespace vtt {

// A cue parsed from a WebVTT file.
struct Cue {
    double startMs = 0.0;  // absolute start in milliseconds
    double endMs   = 0.0;  // absolute end in milliseconds
    QString text;          // plain text (newlines joined with space)
};

// Pure WebVTT parser (the subset YouTube delivers): inline tags and word
// timing marks are stripped, multi-line cues are joined with spaces, the
// timings are kept untouched (YouTube can jump inside a cue).
QVector<Cue> parse(const QByteArray &data);

// Index of the cue containing posMs, or -1. A cue is active while
// [startMs, endMs); explicit jumps inside a cue are preserved because the
// whole cue stays active.
int cueAt(const QVector<Cue> &cues, double posMs);

}  // namespace vtt

// SubtitleOverlay: Qt-side subtitle renderer for the player's transparent
// overlay.  It draws a single line of white text with a thin black shadow
// at the bottom centre of the video — no libass, no mpv OSD, no extra
// render passes.  The timer fires from the main thread only when the cue
// index actually changes, so at rest (same cue or no subtitle) there is no
// work at all.
class SubtitleOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit SubtitleOverlay(QWidget *parent = nullptr);

    // Load the subtitle track at url (WebVTT from YouTube timedtext endpoint).
    // Fires downloadFinished() when ready; already-loaded cues are discarded.
    void loadUrl(const QString &url);

    // Clear the loaded cues and hide the overlay.
    void clear();

    // Show/hide the rendered subtitle text (does not clear cues).
    void setVisible(bool v);

    // Feed the current playback position (seconds) so the overlay picks the
    // right cue.  Called on every position tick from WatchPage.
    void setPosition(double positionSecs);

signals:
    void downloadFinished(bool ok);

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *ev) override;

private slots:
    void onTick();

private:
    QNetworkAccessManager m_nam;
    QVector<vtt::Cue> m_cues;
    QTimer m_timer;          // 100 ms poll; only running while visible + cues loaded
    double m_posMs = 0.0;    // current position in milliseconds
    int    m_activeCue = -1; // index into m_cues of the active cue, -1 = none
    QString m_currentText;   // text currently painted
    bool m_show = false;
};
