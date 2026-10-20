#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include "segment_plan.h"

class DownloadTransport;

namespace dl {

// One download: it fetches the FRAGMENTS of the video and of the audio (in
// parallel, by byte range), writes them into a resource, and muxes them into
// one file.
//
// The unit of the work is the fragment (the sidx of the resource gives its
// exact boundary) and it is what makes the download fast: YouTube throttles a
// request by its size, and the same resource asked for whole runs at 0.011
// MB/s while a 2 MB range of it runs at 14 MB/s.
//
// The fragments are written as they arrive (one file each, inside a
// temporary folder), so an interrupted download keeps the bytes and can be
// resumed.
class DownloadJob : public QObject
{
    Q_OBJECT

public:
    explicit DownloadJob(QObject *parent = nullptr);
    ~DownloadJob() override;

    // Starts the download of `plan`. The plan has to come from
    // planFromFormats() and be valid.
    //
    // `alreadyDone` are the indexes of the fragments that are already on
    // disk from a previous attempt: they are not fetched again, which is
    // what makes the download resumable. The video ones are positive and
    // the audio ones are negative (-1 - index).
    void start(const Plan &plan, const QString &videoId,
               const QVector<int> &alreadyDone = QVector<int>());

    // Stops the download. What is already on disk stays there.
    void stop();

    bool isRunning() const { return m_running; }

    // How many bytes are already written.
    qint64 bytesDone() const { return m_bytesDone; }
    // The expected size (the real one, from the contentLength).
    qint64 bytesTotal() const { return m_bytesTotal; }
    // The speed of the download, in bytes per second (0 when it cannot be
    // measured yet). It is the average since the start, not the speed of the
    // last instant: the last instant jumps around with every chunk.
    qint64 bytesPerSecond() const { return m_bytesPerSecond; }
    // The progress, 0..1, by FRAGMENT: the total of a download is not known
    // until it is done, and a fraction of the bytes would make the bar jump
    // backwards.
    double progress() const;
    // The indexes of the fragments already downloaded (video and audio).
    QVector<int> doneSegments() const;

    // Where the file is written.
    QString outputPath() const { return m_outputPath; }

    // The folder where the fragments and the final file live.
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
    // A transport that is FREE. A transport that is downloading something
    // cannot be reused: DownloadTransport::get() aborts what is in flight,
    // and the request that was aborted never answers.
    DownloadTransport *takeFreeTransport();
    // Fetches the index (the sidx) of a track and the fragments it says.
    void fetchTrack(const QString &track, const Track &t);
    void onIndex(const QString &track, const QVector<Range> &ranges);
    void startNext();
    void onFragmentDone(const QString &track, int index, qint64 size);
    bool writeFile(const QString &path, const QByteArray &data);
    QString fragmentPath(const QString &track, int index) const;
    // Writes the resource: the init segment first (it is the header of the
    // file) and then the fragments IN ORDER (the order is what makes it a
    // playable stream).
    bool assembleTrack(const QString &path, const QString &track,
                       const QVector<Range> &ranges, const QString &initPath);
    void maybeFinishSegments();
    void startMux();
    void sampleSpeed();
    qint64 bytesOnDisk() const;
    void setStatus(const QString &status, const QString &error = QString());

    Plan m_plan;
    QString m_videoId;
    QString m_dir;
    QString m_tempDir;

    QVector<DownloadTransport *> m_pool;
    QVector<bool> m_videoDone;
    QVector<bool> m_audioDone;
    QVector<qint64> m_videoSizes;
    QVector<qint64> m_audioSizes;
    QVector<int> m_alreadyDone;
    QVector<Range> m_videoRanges;
    QVector<Range> m_audioRanges;
    int m_nextVideo = 0;
    int m_nextAudio = 0;
    int m_inFlight = 0;
    // The init and the index of each track are requests of their own: they
    // are counted here so the file is not assembled before they arrive.
    int m_extraPending = 0;
    bool m_running = false;
    bool m_muxing = false;

    qint64 m_bytesDone = 0;
    qint64 m_bytesTotal = 0;
    qint64 m_bytesPerSecond = 0;
    qint64 m_lastSampleBytes = 0;
    qint64 m_lastSampleMs = 0;

    QString m_videoPath;
    QString m_audioPath;
    QString m_outputPath;
    QString m_videoInitPath;
    QString m_audioInitPath;
    dl::DownloadKind m_kind = dl::DownloadKind::Video;
    QString m_status;
    QString m_error;
};

} // namespace dl
