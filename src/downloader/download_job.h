#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QHash>
#include <QVector>

#include <atomic>

#include "arrival_gate.h"
#include "download_transport.h"
#include "segment_plan.h"

class QThread;

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

    // How many times a fragment is asked again when it fails (a timeout or
    // a connection that dies). With a saturated IP some requests do not
    // answer at all, and the whole download used to fail for ONE of them.
    static constexpr int kFragmentRetries = 3;

    // The same for the two requests of the arrival phase (the init segment and
    // the index). It is NOT cosmetic: without the index the plan has no
    // fragments and the download ended up reporting "done" a file of 891
    // bytes (the init only), and without the init the file has no header.
    static constexpr int kArrivalRetries = 3;

    // Where the file is written.
    QString outputPath() const { return m_outputPath; }

    // The folder where the fragments and the final file live.
    void setDirectory(const QString &dir) { m_dir = dir; }
    // The number of connections of the pool (the setting of Ajustes).
    void setConcurrency(int n) { m_concurrency = n; }
    // When video playback is active, download transport threads run at
    // below-normal priority so the player keeps real-time responsiveness.
    void setPlaybackActive(bool active);
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
    // The signed URL ran out of its budget (the server answered 401/403):
    // the formats have to be asked again.
    void urlExpired();

private:
    void buildTransportPool();
    // A transport that is FREE. A transport that is downloading something
    // cannot be reused: DownloadTransport::get() aborts what is in flight,
    // and the request that was aborted never answers.
    DownloadTransport *takeFreeTransport();
    // Sends a request to the WORKER thread of the transports (they live
    // there: the socket, the TLS and the buffers are theirs) and marks the
    // transport as busy until its answer comes back.
    void launch(DownloadTransport *t, const QUrl &url, qint64 rangeStart,
                qint64 rangeEnd, DownloadTransport::DoneFn done,
                DownloadTransport::ProgressFn progress, qint64 expectedBytes,
                const QString &sinkPath = QString());
    // Aborts and destroys the transports IN THEIR OWN thread and stops it.
    void shutdownTransportPool();
    // Fetches the index (the sidx) of a track and the fragments it says.
    void fetchTrack(const QString &track, const Track &t);
    // Launches the requests of the arrival phase that are waiting for a free
    // transport (with one connection the second request of a track has to
    // wait for the first one).
    void pumpArrival();
    // A request of the arrival phase that is planned but not launched yet.
    struct ArrivalTask
    {
        QString track;
        bool isInit = true;   // false = the index
        qint64 indexStart = 0;
        // How many times this request was asked already. The server sometimes
        // answers NOTHING to the first requests of a download (measured: zero
        // bytes in 12 s), so they are asked again.
        int tries = 0;
    };
    QVector<ArrivalTask> m_arrivalQueue;
    // A request of the arrival phase failed. It is asked again (up to
    // kArrivalRetries) and, when the tries run out, the download FAILS: it is
    // not possible to build the file without the init segment or the index.
    void onArrivalFailed(const ArrivalTask &task,
                         const DownloadTransport::Response &r);
    void startNext();
    void onFragmentDone(const QString &track, int index);
    bool writeFile(const QString &path, const QByteArray &data);
    QString fragmentPath(const QString &track, int index) const;
    // Writes the resource: the init segment first (it is the header of the
    // file) and then the fragments IN ORDER (the order is what makes it a
    // playable stream).
    bool assembleTrack(const QString &path, const QString &track,
                       const QVector<Range> &ranges, const QString &initPath);
    void maybeFinishSegments();
    // The assembly of the tracks and the mux, in their OWN thread (they are
    // blocking work over ~1 GB: in the thread of the interface they froze the
    // app when a download ended).
    void startFinalize();
    // The result of that thread, back in the thread of the interface.
    void onFinalizeDone(bool assembled, bool ok, const QString &error,
                        const QString &outputPath);
    void sampleSpeed();
    void setStatus(const QString &status, const QString &error = QString());

    Plan m_plan;
    QString m_videoId;
    QString m_dir;
    QString m_tempDir;

    QVector<DownloadTransport *> m_pool;
    // The transports that are downloading something RIGHT NOW. It is the
    // job's (not the transport's flag) because the transport writes its state
    // in the worker thread and this one is read from the thread of the job.
    QSet<DownloadTransport *> m_busy;
    // The thread of the transports and the object that lives in it (it is
    // the context used to run the cleanup INSIDE that thread).
    QThread *m_thread = nullptr;
    QObject *m_home = nullptr;
    // The thread of the FINALIZE phase (the assembly and the mux). It is
    // separate from the pool: a pause must not interrupt a mux that is
    // writing the file.
    QThread *m_finalThread = nullptr;
    QVector<bool> m_videoDone;
    QVector<bool> m_audioDone;
    QVector<qint64> m_videoSizes;
    QVector<qint64> m_audioSizes;
    QVector<int> m_alreadyDone;
    // The size of the pool of transports (the connections).
    int m_concurrency = 0;
    // How many times each fragment has been asked (the ones that failed are
    // asked again, up to kFragmentRetries).
    QHash<int, int> m_fragmentTries;
    // The bytes reported by the transport for the fragment that is being
    // fetched NOW, by mark. They are what makes the bar advance without
    // asking the disk: the old code measured EVERY fragment (QFileInfo) on
    // every fragment that finished, which is O(n^2) stats in the thread of
    // the interface (n = the fragments of the video, hundreds).
    QHash<int, qint64> m_fragmentBytes;
    // The mark of a fragment (the video ones are 0, 1, 2... and the audio
    // ones are -(2), -(3)...).
    static int markOf(const QString &track, int index);
    QVector<Range> m_videoRanges;
    QVector<Range> m_audioRanges;
    int m_nextVideo = 0;
    int m_nextAudio = 0;
    int m_inFlight = 0;
    // The requests of the ARRIVAL phase (the init and the index of each
    // track) that are still in flight. The fragments only start when ALL of
    // them are done (the ranges of the video are not known until its index
    // arrives).
    //
    // It is a GATE and not a plain counter: the counter is only touched in
    // ONE place, so the order of the answers does not matter. Before, it
    // was decremented in two places and only one of them checked it, so if
    // the last answer was an init (and not an index) the counter went to
    // zero with nobody noticing and the download never started.
    ArrivalGate m_arrival;
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
    // Read by the pool thread and by the finalize thread while they run, so
    // it is atomic and not a plain bool.
    std::atomic<bool> m_playbackActive{false};
};

} // namespace dl
