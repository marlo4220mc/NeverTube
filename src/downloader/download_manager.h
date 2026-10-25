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

// The downloader of the app: it takes a video id, asks for the adaptive
// formats, makes the plan, downloads the segments in parallel and muxes
// them into one mkv.
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
    // The formats are asked for first: the dialog of the download needs the
    // variants the video has (and their size), so the user picks the
    // resolution BEFORE anything is downloaded.
    // `maxHeight` is the cap of the resolution of the DOWNLOAD (0 = no
    // cap). It is NOT the cap of the player: the download is a file and the
    // person asked for it.
    void start(const QString &videoId, const QString &title,
               const QString &channel, int maxHeight,
               int durationSeconds = 0, int forcedHeight = 0,
               const QString &forcedCodec = QString(),
               bool forcedAudioOnly = false);
    // The resolutions the dialog offers (the ones of the player).
    void setAvailableHeights(const QVector<int> &heights) { m_availableHeights = heights; }

    // Starts the download AGAIN after a failure.
    //
    // It does NOT delete the row of the repository: the index of the
    // fragments that are already on the disk lives in it, and deleting it
    // made the download start from zero (the bytes that were already
    // downloaded were thrown away). The fragments that were half written
    // are discarded by the job itself (it checks the size of each one).
    bool retry(const QString &videoId);

    // Pauses the download in progress: the fragments that are already on
    // the disk are kept (the row keeps its index), so continuing is the
    // same path as the retry.
    //
    // The state "paused" is the one the page of Descargas shows with the
    // button of "Continuar".
    void pause(const QString &videoId);
    // Continues a paused download (the same as the retry: it resumes).
    void resume(const QString &videoId);


    // The download in progress, if any.
    bool isBusy() const { return m_job != nullptr; }
    QString currentVideoId() const { return m_currentId; }

    // The folder where the files are written.
    void setDirectory(const QString &dir) { m_dir = dir; }
    // The number of connections of the downloads (the setting of Ajustes).
    void setConcurrency(int n) { m_concurrency = n; }
    QString directory() const { return m_dir; }
    // When video is playing (player or mini player), download workers run at
    // below-normal priority to avoid stealing CPU from realtime playback. The
    // change is recoverable: closing the player immediately restores normal
    // priority for future workers.
    void setPlaybackActive(bool active);

    // The default folder: the folder of downloads of the SYSTEM (which
    // comes already translated: "Descargas", "Downloads") plus a subfolder
    // of the app, so the files are not mixed with the ones of the user.
    static QString defaultDirectory();

    // The downloads, newest first.
    QVector<DownloadInfo> downloads() const;

    // Removes a download from the list and deletes its files.
    bool remove(const QString &videoId);
    // Removes the download from the list but KEEPS the file on the disk: it
    // is what the dialog of the delete offers for a finished download
    // ("only from the list").
    bool removeFromList(const QString &videoId);

private:
    bool m_playbackActive = false;

  private:
    // True when the path is inside the folder of the downloads: the app only
    // deletes its own files.
    bool isInsideDownloadFolder(const QString &path) const;

  public:

    // The state of one download, if it is in the list.
    bool state(const QString &videoId, DownloadInfo &out) const;

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
    // The adaptive formats: they carry BOTH tracks (video and audio) with
    // their direct URL, the initRange and the indexRange of each one.
    void onAdaptiveFormats(const QString &videoId, const QJsonArray &formats);
    void onJobFinished(const QString &path);
    void onJobFailed(const QString &reason);
    // The progress is saved at most every kSaveEveryMs. The job reports the
    // progress on EVERY chunk that arrives (~64 KB), and writing the row to
    // the database (a SELECT + an INSERT with a commit) per chunk is what
    // made the interface stutter while downloading. The events that matter
    // (a fragment done, pause, finish) force the save.
    static constexpr int kSaveEveryMs = 700;
    qint64 m_lastSaveMs = 0;
    void saveProgress(bool force = false);

    Yt *m_yt = nullptr;
    // The window: the dialog of the download is an overlay child of it (a
    // window of its own would steal the pointer from the user).
    QWidget *m_window = nullptr;
    DownloadRepo *m_repo = nullptr;
    DownloadJob *m_job = nullptr;
    QString m_currentId;
    QString m_dir;
    QString m_audioLang;
    // The resolutions the dialog offers: the ones the player already knows
    // this video has (it hides the ones that are not available).
    QVector<int> m_availableHeights;
    QString m_title;
    QString m_channel;
    // The cap of the resolution of the download (0 = no cap: the person
    // asked for a file and the menu shows the real sizes).
    int m_maxHeight = 0;
    // The number of connections (0 = the default of the transport).
    int m_concurrency = 0;
    // The download that was CANCELLED with the X of its row: the callbacks
    // of the job (finished/failed) arrive AFTER the removal (the event loop
    // delivers them later) and they wrote the row again, so the download
    // came back to the list. With this flag the callbacks know that the
    // download was cancelled and they do NOT write anything.
    QString m_cancelled;
    int m_durationSeconds = 0;  // of the video being downloaded
    // What the command line asked for (0 = ask the user).
    int m_forcedHeight = 0;
    // The codec the user picked in the submenu of the dialog (empty = the
    // default one, the smallest).
    QString m_forcedCodec;
    bool m_forcedAudioOnly = false;
    // When the page was last updated (the progress is reported several
    // times per second and the page is repainted at most every 250 ms).
    qint64 m_lastUiUpdateMs = 0;
};

} // namespace dl
