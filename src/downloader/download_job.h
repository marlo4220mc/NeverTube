#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include "segment_plan.h"
#include "download_transport.h"

namespace dl {

// One download: it fetches the segments of the master (the video) and of the
// chosen audio rendition, writes them into two partial files, and muxes them
// into one mp4.
//
// The partials are written SPARSELY: the file is created empty and every
// segment is written at its offset, so a 300 MB download does not reserve
// 300 MB of disk at the start and a download that is interrupted keeps what
// it already has (which is what makes the resume possible).
//
// The unit of the work is the SEGMENT (the master is already fragmented by
// YouTube with segments of ~1.3 MB, and the throttle allows ~8 MB per
// request), so the state is a set of segment indexes.
class DownloadJob : public QObject
{
    Q_OBJECT

public:
    explicit DownloadJob(QObject *parent = nullptr);
    ~DownloadJob() override;

    // Starts the download of `plan`. The plan has to come from
    // planFromMaster() and be valid.
    //
    // The plan carries the URL of the media playlists but not their
    // segments: those live in a second playlist (the media one), which the
    // job fetches first. The plan then is completed with the real segments.
    //
    // `alreadyDone` are the indexes of the segments that are already on disk
    // from a previous attempt: they are not fetched again, which is what
    // makes the download resumable.
    void start(const Plan &plan, const QString &videoId,
               const QVector<int> &alreadyDone = QVector<int>());

    // Stops the download. What is already on disk stays there.
    void stop();

    bool isRunning() const { return m_running; }

    // How many bytes are already written in the partials.
    qint64 bytesDone() const { return m_bytesDone; }
    // The expected size, from the BANDWIDTH of the master.
    qint64 bytesTotal() const { return m_bytesTotal; }
    // The indexes of the segments already downloaded (video and audio), in
    // the same encoding the resume uses.
    QVector<int> doneSegments() const;
    // The indexes of the segments that are already on disk from a previous
    // attempt (the resume). The video ones are their own index (0, 1, 2...)
    // and the audio ones are -(index + 2), so the 0 of the video and the 0 of
    // the audio do not collide and neither of them is lost.
    QVector<int> m_alreadyDone;

    // Where the partials and the final file are.
    QString videoPartialPath() const { return m_videoPath; }
    QString audioPartialPath() const { return m_audioPath; }
    QString outputPath() const { return m_outputPath; }

    // The directory where the partials and the final file live.
    void setDirectory(const QString &dir) { m_dir = dir; }
    QString directory() const { return m_dir; }

    // The state, in the vocabulary of the repository (pending, downloading,
    // paused, muxing, done, error).
    QString status() const { return m_status; }
    QString error() const { return m_error; }

signals:
    void progressChanged(qint64 bytesDone, qint64 bytesTotal);
    void segmentDone(int index);
    void statusChanged(const QString &status, const QString &error);
    void finished(const QString &outputPath);
    void failed(const QString &error);

private:
    void buildTransportPool();
    void fetchMediaPlaylists();
    void onPlaylist(const QString &track,
                    const DownloadTransport::Response &response);
    void fetchInitSegment();
    void startNext();
    void onSegment(const QString &track, int index,
                   const QByteArray &data);
    void writeSegment(const QString &track, int index, const QByteArray &data);
    // The file of one segment inside the temporary folder of the download.
    QString segmentPath(const QString &track, int index) const;
    // Writes the segments that are on disk, in order, into the partial.
    bool assemblePartial(const QString &path, const QString &track,
                         const QByteArray &initData);
    void maybeFinishSegments();
    void startMux();
    void setStatus(const QString &status, const QString &error = QString());

    Plan m_plan;
    QString m_videoId;
    QString m_dir;

    QVector<DownloadTransport *> m_pool;
    dl::InitSegment m_videoInit;
    dl::InitSegment m_audioInit;
    QByteArray m_videoInitData;
    QByteArray m_audioInitData;
    QVector<QByteArray> m_videoData;  // the bytes of each video segment
    QVector<QByteArray> m_audioData;
    QVector<bool> m_videoDone;
    QVector<bool> m_audioDone;
    int m_nextVideo = 0;
    int m_nextAudio = 0;
    int m_inFlight = 0;
    bool m_running = false;
    bool m_muxing = false;

    qint64 m_bytesDone = 0;
    qint64 m_bytesTotal = 0;

    QString m_videoPath;
    QString m_audioPath;
    QString m_tempDir;
    QString m_outputPath;
    QString m_status;
    QString m_error;
};

} // namespace dl
