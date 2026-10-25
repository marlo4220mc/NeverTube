#pragma once

#include <QObject>
#include <QJsonArray>
#include <QQueue>
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
    //
    // `key` is the IDENTITY of the row (the video id, or "<videoId>-audio"
    // for its mp3): the video id is read from the row, so an mp3 resumes as
    // an mp3 and not as a video that does not exist.
    bool retry(const QString &key);

    // Pauses the download in progress: the fragments that are already on
    // the disk are kept (the row keeps its index), so continuing is the
    // same path as the retry.
    //
    // The state "paused" is the one the page of Descargas shows with the
    // button of "Continuar".
    void pause(const QString &key);
    // Continues a paused download (the same as the retry: it resumes).
    void resume(const QString &key);

    // Pauses every download that is running or waiting. The one in course is
    // stopped and the queued ones stop waiting: they all go to "paused", which
    // is the state that the person can see.
    void pauseAll();
    // Continues every paused download. The first one starts and the rest wait
    // in the queue (one download at a time), which is the same path as
    // pressing "Continuar" on each one.
    void resumeAll();

    // Deletes the folders of fragments that have no row any more (a download
    // deleted by a version that did not remove them, or a database that was
    // restored): they are hundreds of MB that nothing will ever use. Returns
    // how many were removed. Only INSIDE the folder of the downloads.
    int sweepOrphanParts();

    // Starts again the downloads that were left half way: the rows that say
    // "pending" (they waited in the queue of a previous session and nobody
    // read them) and the ones that say "downloading"/"muxing" (the process
    // ended while they ran). It is called once at startup: without it a row
    // written by the queue stayed in "pending" FOREVER, because the queue
    // only lives in memory and nothing reads those rows when the app opens.
    //
    // The "paused" and "error" rows are NOT touched: those are a decision of
    // the person (continue / retry).
    void resumePending();


    // The download in progress, if any. A download that is WAITING for its
    // formats has no job yet but its identity is taken: it is just as busy
    // as the one with a job (start() and processQueue() already decide by
    // those two conditions together).
    bool isBusy() const
    {
        return m_job != nullptr || !m_currentId.isEmpty();
    }
    // The IDENTITY of the download in progress (the video id, or
    // "<videoId>-audio" for its mp3), not the video id.
    QString currentKey() const { return m_currentId; }

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
    bool remove(const QString &key);
    // Removes the download from the list but KEEPS the file on the disk: it
    // is what the dialog of the delete offers for a finished download
    // ("only from the list").
    bool removeFromList(const QString &key);

private:
    bool m_playbackActive = false;

  private:
    // True when the path is inside the folder of the downloads: the app only
    // deletes its own files.
    bool isInsideDownloadFolder(const QString &path) const;

    // Stops the download of that row and forgets it: the job (if there is
    // one) is stopped and released, and the identity is cleared so that a
    // late answer of the formats (or of the job) is ignored. It is what the
    // X and the pause need: without it the download came back to life (the
    // formats arrived AFTER the removal and the job was created for a row
    // that did not exist any more).
    //
    // `key` is the IDENTITY of the row. Returns true when that row was the
    // one in flight (with a job, or waiting for its formats).
    bool cancelInFlight(const QString &key);

    // The mp3 rows of the first version of the downloader shared the row (and
    // the folder of the fragments) of the video: their key is the plain video
    // id and `quality` is "mp3". The mp3 has its own key now, so such a row
    // is migrated here, ONCE and with its folder: otherwise it would be
    // resumed as a VIDEO, and if it were resumed as an mp3 under the old key
    // it would collide with the video of the same id. `info` is left with the
    // key of the row it belongs to (the same one when there is nothing to
    // migrate).
    void migrateLegacyAudioRow(DownloadInfo &info);

  public:

    // The state of one download, if it is in the list (`key` is its
    // identity: the video id, or "<videoId>-audio" for its mp3).
    bool state(const QString &key, DownloadInfo &out) const;

signals:
    // The list changed (a download was added, moved or removed).
    void changed();
    // The progress of the download in progress.
    void progressChanged(const QString &key, qint64 bytesDone,
                         qint64 bytesTotal);
    // The download in progress finished: the path of the mp4.
    void finished(const QString &key, const QString &path);
    // The download in progress failed (the reason).
    void failed(const QString &key, const QString &reason);

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
    // The IDENTITY of the download in progress (the video id, or
    // "<videoId>-audio" for its mp3): it is the key of the row and the name of
    // the temporary folder and of the partial files.
    QString m_currentId;
    // The ID of THE VIDEO of the download in progress. It is NOT the key: the
    // formats arrive with the video id (YouTube does not know about
    // "-audio"), so the answer is matched against THIS one while the row is
    // read and written with the key. Matching the answer against the key was
    // the mp3 that stayed "pending" forever: the answer came as "abc" and the
    // current one was "abc-audio".
    QString m_currentVideoId;
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
    // The NUMBER of the job in progress. The callbacks of a job (progress,
    // finished, failed, urlExpired) are delivered by the event loop, so a
    // callback of a job that was already cancelled, paused or replaced can
    // arrive when another download is running (or when there is none). The
    // callbacks capture the number of THEIR job and ignore themselves when it
    // is no longer the current one: without this a paused or deleted download
    // wrote its state into the row of the next one (the "pending forever" and
    // the row that came back to the list).
    quint64 m_jobSerial = 0;
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

    // Queue of pending downloads.
    struct DownloadRequest {
        QString videoId;
        QString title;
        QString channel;
        int maxHeight;
        int durationSeconds;
        int forcedHeight;
        QString forcedCodec;
        bool forcedAudioOnly;
    };
    QQueue<DownloadRequest> m_queue;

    // Processes the next download in the queue.
    void processQueue();
    // Takes a request out of the queue (the row was removed or deleted): the
    // download that the person cancelled must not come back when the current
    // one ends.
    void dropQueued(const QString &key);
    // The row of a download that is QUEUED (not started yet): it is written
    // at once so the list shows it as "pending" instead of showing nothing at
    // all until the current download ends.
    void writePendingRow(const QString &key, const QString &videoId,
                         const QString &title, const QString &channel);
};

} // namespace dl
