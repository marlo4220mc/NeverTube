#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include "../db/repositories.h"

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
    DownloadManager(Yt *yt, DownloadRepo *repo, QObject *parent = nullptr);
    ~DownloadManager() override;

    // Starts the download of a video. When it is already in the list it is
    // NOT queued again: the state is shown instead.
    void start(const QString &videoId, const QString &title,
               const QString &channel, int maxHeight);

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
    void onJobFinished(const QString &path);
    void onJobFailed(const QString &reason);
    void saveProgress();

    Yt *m_yt = nullptr;
    DownloadRepo *m_repo = nullptr;
    DownloadJob *m_job = nullptr;
    QString m_currentId;
    QString m_dir;
    QString m_title;
    QString m_channel;
    int m_maxHeight = 1080;
    bool m_masterConnected = false;
};

} // namespace dl
