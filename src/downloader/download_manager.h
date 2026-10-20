#pragma once

#include <QObject>
#include <QJsonArray>
#include <QString>
#include <QVector>

#include "../db/repositories.h"
#include "segment_plan.h"

class QWidget;
class Yt;

namespace dl {

class DownloadJob;

// The downloader of the app: it takes a video id, asks for the master
// playlist, makes the plan, downloads the segments in parallel and muxes
// them into one mp4.
//
// It keeps ONE download at a time (decision D15): the throttle of YouTube is
// per request, so eight parallel connections plus the player would eat the
// bandwidth, and the 403 gate that AGENTS.md documents is a real risk while
// downloading.
//
// The state of every download lives in the `downloads` table, so it survives
// a restart and the page can show what was done.
class DownloadManager : public QObject
{
    Q_OBJECT

public:
    DownloadManager(Yt *yt, DownloadRepo *repo, QWidget *window,
                    QObject *parent = nullptr);
    ~DownloadManager() override;

    // Starts the download of a video. When it is already in the list it is
    // NOT queued again: the state is shown instead.
    //
    // The master is asked for first: the dialog of the download needs the
    // variants it has (and their size), so the user picks the resolution
    // BEFORE anything is downloaded.
    void start(const QString &videoId, const QString &title,
               const QString &channel, int maxHeight,
               int durationSeconds = 0, int forcedHeight = 0,
               bool forcedAudioOnly = false);
    // The resolutions the dialog offers (the ones of the player).
    void setAvailableHeights(const QVector<int> &heights) { m_availableHeights = heights; }


    // The download in progress, if any.
    bool isBusy() const { return m_job != nullptr; }
    QString currentVideoId() const { return m_currentId; }

    // The folder where the files are written.
    void setDirectory(const QString &dir) { m_dir = dir; }
    QString directory() const { return m_dir; }

    // The default folder: the folder of downloads of the SYSTEM (which
    // comes already translated: "Descargas", "Downloads") plus a subfolder
    // of the app, so the files are not mixed with the ones of the user.
    static QString defaultDirectory();

    // The downloads, newest first.
    QVector<DownloadInfo> downloads() const;

    // Removes a download from the list and deletes its files.
    bool remove(const QString &videoId);

    // The state of one download, if it is in the list.
    bool state(const QString &videoId, DownloadInfo &out) const;

    // Whether the system has an ffmpeg (needed to join the two tracks).
    static bool hasFfmpeg();

signals:
    // The list changed (a download was added, moved or removed).
    void changed();
    // The progress of the download in progress.
    void progressChanged(const QString &videoId, qint64 bytesDone,
                         qint64 bytesTotal);
    // The download in progress finished: the path of the mp4.
    void finished(const QString &videoId, const QString &path);
    // The download in progress failed (the reason).
    void failed(const QString &videoId, const QString &reason);

private:
    void onMasterReady(const QString &videoId, const QByteArray &masterBody);
    // The adaptive formats: the AUDIO comes from here (a clean m4a), because
    // the audio of the master is AAC in LATM and the FFmpeg of this system
    // cannot mux that.
    void onAdaptiveFormats(const QString &videoId, const QJsonArray &formats);
    void onFormatsReady(const QString &videoId, const QJsonArray &formats);
    // Builds the plan and starts the download. It waits for the master AND
    // the adaptive formats: they arrive in any order.
    void buildPlan();
    void onJobFinished(const QString &path);
    void onJobFailed(const QString &reason);
    void saveProgress();

    Yt *m_yt = nullptr;
    // The window: the dialog of the download is an overlay child of it (a
    // window of its own would steal the pointer from the user).
    QWidget *m_window = nullptr;
    DownloadRepo *m_repo = nullptr;
    DownloadJob *m_job = nullptr;
    QString m_currentId;
    QString m_dir;
    QString m_audioLang;
    // The adaptive formats of the video: the audio comes from here.
    QJsonArray m_adaptiveFormats;
    QByteArray m_masterBody;
    // The resolutions the dialog offers: the ones the player already knows
    // this video has (it hides the ones that are not available).
    QVector<int> m_availableHeights;
    QString m_title;
    QString m_channel;
    int m_maxHeight = 1080;
    int m_durationSeconds = 0;  // of the video being downloaded
    // What the command line asked for (0 = ask the user).
    int m_forcedHeight = 0;
    bool m_forcedAudioOnly = false;
    bool m_masterConnected = false;
    // When the page was last updated (the progress is reported several
    // times per second and the page is repainted at most every 250 ms).
    qint64 m_lastUiUpdateMs = 0;
};

} // namespace dl
