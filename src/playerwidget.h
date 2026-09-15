#ifndef PLAYERWIDGET_H
#define PLAYERWIDGET_H

#include <QOpenGLWidget>

#include <mpv/client.h>
#include <mpv/render.h>

class PlayerWidget : public QOpenGLWidget
{
    Q_OBJECT
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)

public:
    explicit PlayerWidget(QWidget *parent = nullptr);
    ~PlayerWidget() override;

    double position() const { return m_position; }
    double duration() const { return m_duration; }
    QString state() const { return m_state; }
    double speed() const;

    // Acceso de solo lectura para selftest --selftest-player.
    QString hwdec() const { return m_hwdec; }
    int redrawIntervalMs() const { return m_redrawTimerActive ? 33 : 0; }

public slots:
    void playUrl(const QString &url, double startAt = 0.0);
    void playYtdl(const QString &watchUrl, int maxHeight);
    void stop();
    void setPaused(bool paused);
    void seek(double seconds);
    void setVolume(double volume);
    void setSubVisibility(bool visible);
    void setAudioLanguage(const QString &lang);

signals:
    void positionChanged();
    void durationChanged();
    void stateChanged();
    void mediaError(const QString &message);
    void videoAspectChanged(double aspect);
    void videoSizeChanged(int width, int height);

protected:
    void initializeGL() override;
    void paintGL() override;

private slots:
    void poll();
    void onFrameReady();

private:
    static void mpvWakeup(void *ctx);
    static void mpvUpdate(void *ctx);
    void handleEvent(mpv_event *event);
    void emitAspectIfReady();

    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_rctx = nullptr;
    int m_logFrames = 0;
    double m_position = 0.0;
    double m_duration = 0.0;
    QString m_state;
    QString m_hwdec;
    int64_t m_videoW = 0;
    int64_t m_videoH = 0;
    double m_aspect = 0.0;
    QString m_audioLang;
    bool m_redrawTimerActive = false;
};

#endif