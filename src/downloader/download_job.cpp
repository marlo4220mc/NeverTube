#include "download_job.h"

#include "../i18n.h"

#include "download_transport.h"
#include "muxer.h"

#include "../debug.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QUrl>

namespace dl {

namespace {
// How many bytes are asked for when the index (the sidx) is fetched. The
// indexRange of the format does NOT cover the whole box (measured: it says
// 800 bytes and the box is 811, with 64 references of 12 bytes each), so a
// window that covers it is asked for and the box is found inside it.
constexpr qint64 kIndexWindowBytes = 16 * 1024;

// The size of the pieces of a track that has no index. It has to be under
// the ~8 MB that YouTube's throttle allows per request (measured: 2 MB runs
// at 14 MB/s).
constexpr qint64 kChunkBytes = 2 * 1024 * 1024;

// How long stop() waits for the thread of the transports to finish its
// cleanup. The posted work only aborts sockets and deletes objects: it does
// not wait for the network, so this is a ceiling, not a delay.
constexpr int kShutdownWaitMs = 2000;

// The path of one fragment. It is a free function (and not the member
// fragmentPath) because the assembly runs in the FINALIZE thread, which
// cannot read the members of the job: those belong to the thread of the
// interface.
QString fragmentPathIn(const QString &tempDir, const QString &track, int index)
{
    return tempDir + QLatin1Char('/') + track + QLatin1Char('_')
           + QString::number(index) + QStringLiteral(".part");
}

// Writes the resource: the init segment first (it is the header of the file)
// and then the fragments IN ORDER (the order is what makes it a playable
// stream).
//
// It runs in the FINALIZE thread: reading ~1 GB of fragments and writing them
// again from the thread of the interface is what froze the app for 20.8 s
// when a download ended (measured, a 58 minute video at 1080p).
bool assembleTrackFiles(const QString &tempDir, const QString &path,
                        const QString &track, const QVector<Range> &ranges,
                        const QString &initPath)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QFile init(initPath);
    if (init.exists() && init.open(QIODevice::ReadOnly)) {
        while (!init.atEnd())
            file.write(init.read(256 * 1024));
        init.close();
    }
    for (int i = 0; i < ranges.size(); ++i) {
        QFile fragment(fragmentPathIn(tempDir, track, i));
        if (!fragment.exists())
            continue;
        if (!fragment.open(QIODevice::ReadOnly)) {
            file.close();
            return false;
        }
        while (!fragment.atEnd())
            file.write(fragment.read(256 * 1024));
        fragment.close();
    }
    file.close();
    return true;
}

// What the FINALIZE thread needs to do its work. It is a copy (and not a
// pointer to the job): the thread never reads the members of the job, which
// the thread of the interface keeps touching.
struct FinalizeInput
{
    QString tempDir;
    QString videoPath;
    QString audioPath;
    QString videoInitPath;
    QString audioInitPath;
    QString outputPath;
    QVector<Range> videoRanges;
    QVector<Range> audioRanges;
    bool hasAudio = false;
    bool audioOnly = false;
    bool keepPartials = false;
};

// What it returns. `stage` says WHERE it failed: the two failures are
// different messages in the page of Descargas.
struct FinalizeResult
{
    bool assembled = true;   // false = the resource could not be written
    bool ok = false;         // the mux worked and the file is there
    QString error;
    QString outputPath;
};
}

DownloadJob::DownloadJob(QObject *parent)
    : QObject(parent)
    , m_status(QStringLiteral("pending"))
{
}

DownloadJob::~DownloadJob()
{
    stop();
    // The finalize thread writes the file (~1 GB): it is JOINED and not
    // killed, because killing it in the middle would leave a half written
    // video. It is the same wait the app did before, only it no longer
    // freezes the interface while the download runs.
    if (m_finalThread) {
        m_finalThread->wait();
        delete m_finalThread;
        m_finalThread = nullptr;
    }
}

void DownloadJob::setStatus(const QString &status, const QString &error)
{
    m_status = status;
    m_error = error;
    emit statusChanged(m_status, m_error);
}

void DownloadJob::start(const Plan &plan, const QString &videoId,
                        const QVector<int> &alreadyDone)
{
    stop();
    if (!plan.valid) {
        setStatus(QStringLiteral("error"),
                  plan.error.isEmpty() ? QStringLiteral("plan invalido") : plan.error);
        emit failed(m_error);
        return;
    }

    m_plan = plan;
    m_kind = plan.kind;
    m_videoId = videoId;
    m_dir = m_dir.isEmpty() ? QDir::tempPath() : m_dir;
    QDir().mkpath(m_dir);

    // Each fragment goes into its own file inside a temporary folder: the
    // bytes are on the disk as they arrive, so an interrupted download keeps
    // them and can be resumed.
    m_tempDir = m_dir + QLatin1Char('/') + m_videoId + QStringLiteral(".parts");
    QDir().mkpath(m_tempDir);

    // The final file: MKV for the video. The audio of the HLS master is AAC
    // in ADTS (LATM) and an mp4 wants it in ASC: the bitstream filter that
    // does the conversion fails with "Not yet implemented in FFmpeg" in the
    // FFmpeg of this system. Matroska takes the ADTS as it is. For the
    // audio only it is an mp3.
    m_videoPath = m_dir + QLatin1Char('/') + m_videoId + QStringLiteral(".video.mp4");
    m_audioPath = m_dir + QLatin1Char('/') + m_videoId + QStringLiteral(".audio.m4a");
    m_outputPath = m_dir + QLatin1Char('/') + m_videoId
                   + (m_kind == dl::DownloadKind::Audio ? QStringLiteral(".mp3")
                                                       : QStringLiteral(".mkv"));
    m_videoInitPath = m_tempDir + QStringLiteral("/video.init");
    m_audioInitPath = m_tempDir + QStringLiteral("/audio.init");

    m_alreadyDone = alreadyDone;
    m_nextVideo = 0;
    m_nextAudio = 0;
    m_inFlight = 0;
    m_arrival.reset();
    m_arrivalQueue.clear();
    m_bytesDone = 0;
    m_bytesTotal = plan.estimatedBytes;
    m_muxing = false;
    m_running = true;

    setStatus(QStringLiteral("downloading"));
    buildTransportPool();
    if (m_kind == dl::DownloadKind::Audio) {
        // Only the audio: the video is not fetched at all.
        fetchTrack(QStringLiteral("audio"), plan.audio);
    } else {
        fetchTrack(QStringLiteral("video"), plan.video);
        fetchTrack(QStringLiteral("audio"), plan.audio);
    }
}

void DownloadJob::stop()
{
    if (m_pool.isEmpty() && !m_running && !m_thread)
        return;
    shutdownTransportPool();
    m_running = false;
    m_inFlight = 0;
    // A mux in progress is NOT paused: it is writing the file and stopping it
    // half way would leave a broken video. The state stays "muxing" and the
    // result arrives when the thread ends.
    if (m_muxing)
        return;
    if (m_status == QStringLiteral("downloading")
        || m_status == QStringLiteral("muxing"))
        setStatus(QStringLiteral("paused"));
}

void DownloadJob::buildTransportPool()
{
    shutdownTransportPool();
    // The transports work in their OWN thread. This is what keeps the frames
    // of the player, which are painted from the thread of the interface: with
    // 8 connections there, the main thread was decrypting TLS, copying the
    // buffers and writing the fragments while it had to paint, and the video
    // dropped frames for as long as the download lasted.
    //
    // The callbacks still come back HERE (setCallbackContext), so the state
    // of the job (the counters, the queue, the file of the fragment) is only
    // touched by the thread of the interface.
    m_thread = new QThread();
    m_thread->setObjectName(QStringLiteral("download-transport"));
    m_thread->start();
    m_home = new QObject();
    m_home->moveToThread(m_thread);

    // The setting of Ajustes (the connections) is what decides the size of
    // the pool. It used to be ignored: buildTransportPool() asked for
    // defaultConcurrency() always, so the setting did nothing.
    const int n = m_concurrency > 0 ? m_concurrency
                                    : DownloadTransport::defaultConcurrency();
    for (int i = 0; i < n; ++i) {
        // No parent: an object with a parent cannot change of thread.
        DownloadTransport *t = new DownloadTransport();
        t->moveToThread(m_thread);
        t->setCallbackContext(this);
        m_pool.append(t);
    }
}

// The cleanup has to run INSIDE the thread of the transports: destroying an
// object from another thread while its socket is alive is a crash waiting for
// a readyRead. The work is posted to m_home (which lives in that thread) and
// the thread is asked to quit at the END of that same call, so the deletes
// cannot be left pending.
//
// The wait is bounded and short: the posted work only aborts sockets and
// destroys objects (it never waits for the network), so in practice this is a
// few milliseconds. It is what lets the transports be gone when stop()
// returns, instead of leaving a thread and a home behind every pause.
void DownloadJob::shutdownTransportPool()
{
    const QVector<DownloadTransport *> pool = m_pool;
    m_pool.clear();
    m_busy.clear();
    QThread *thread = m_thread;
    QObject *home = m_home;
    m_thread = nullptr;
    m_home = nullptr;

    if (!thread) {
        qDeleteAll(pool);
        return;
    }

    if (home && !pool.isEmpty()) {
        QMetaObject::invokeMethod(home, [thread, pool]() {
            for (DownloadTransport *t : pool) {
                t->abort();
                delete t;
            }
            thread->quit();
        }, Qt::QueuedConnection);
    } else {
        qDeleteAll(pool);
        thread->quit();
    }

    if (!thread->wait(kShutdownWaitMs)) {
        // It did not stop in the bounded wait (it should never happen: the
        // posted work does not touch the network). Rather than destroying a
        // running QThread (a crash), it is left to delete itself when it
        // ends.
        QObject::connect(thread, &QThread::finished, thread, [thread, home]() {
            delete home;
            thread->deleteLater();
        }, Qt::QueuedConnection);
        return;
    }
    delete home;
    delete thread;
}

void DownloadJob::launch(DownloadTransport *t, const QUrl &url,
                         qint64 rangeStart, qint64 rangeEnd,
                         DownloadTransport::DoneFn done,
                         DownloadTransport::ProgressFn progress,
                         qint64 expectedBytes)
{
    if (!t)
        return;
    m_busy.insert(t);
    const QString ua = DownloadTransport::userAgent();
    QMetaObject::invokeMethod(t, [t, url, rangeStart, rangeEnd, ua, done,
                                  progress, expectedBytes]() {
        t->get(url, rangeStart, rangeEnd, ua, done, progress, expectedBytes);
    }, Qt::QueuedConnection);
}

// A transport that is FREE.
//
// A transport that is downloading something cannot be reused: 
// DownloadTransport::get() aborts what is in flight, and the request that
// was aborted never answers (its callback is not called), so the counter of
// the pending requests never goes down and the download never starts.
DownloadTransport *DownloadJob::takeFreeTransport()
{
    for (DownloadTransport *t : m_pool) {
        if (t && !m_busy.contains(t))
            return t;
    }
    return nullptr;
}

// Fetches the index (the sidx) of a track: it says where every fragment
// starts. The init goes in the same request window (it is just before the
// index).
void DownloadJob::fetchTrack(const QString &track, const Track &t)
{
    if (!t.valid || t.url.isEmpty()) {
        // The track does not exist (or has no URL): there is nothing to
        // wait for and nothing to count.
        return;
    }

    // Two requests per track: the init segment (the header of the file) and
    // the index (the sidx, which says where every fragment starts).
    //
    // They are COUNTED when they are PLANNED, not when they are launched:
    // with one connection the second request of a track has to wait for the
    // first one, and the phase is only over when all of them have answered.
    ArrivalTask initTask;
    initTask.track = track;
    initTask.isInit = true;
    m_arrivalQueue.append(initTask);

    ArrivalTask indexTask;
    indexTask.track = track;
    indexTask.isInit = false;
    indexTask.indexStart = t.indexStart > 0 ? t.indexStart : t.initEnd + 1;
    m_arrivalQueue.append(indexTask);

    m_arrival.add();
    m_arrival.add();
    pumpArrival();
}

// Launches the arrival requests that are waiting for a free transport.
void DownloadJob::onArrivalFailed(const ArrivalTask &task,
                                  const DownloadTransport::Response &r)
{
    // The arrival phase is over (a pause): nothing to re-ask.
    if (!m_running) {
        m_arrival.done();
        return;
    }
    // The signed URL ran out of its budget: the download is resolved again
    // with fresh URLs. It is not a failure of the video.
    if (r.status == 401 || r.status == 403) {
        m_arrival.done();
        m_running = false;
        setStatus(QStringLiteral("expired"));
        emit urlExpired();
        return;
    }
    if (task.tries + 1 < kArrivalRetries) {
        ArrivalTask again = task;
        ++again.tries;
        m_arrivalQueue.prepend(again);
        // add() BEFORE done(): the gate counts the planned requests and it
        // must not see zero (that would start the fragments with no index).
        m_arrival.add();
        m_arrival.done();
        pumpArrival();
        return;
    }
    const QString msg = r.error.isEmpty() ? nt::tr("error.dlPartFetch")
                                          : r.error;
    m_arrival.done();
    m_running = false;
    setStatus(QStringLiteral("error"), msg);
    emit failed(msg);
}

void DownloadJob::pumpArrival()
{
    while (!m_arrivalQueue.isEmpty()) {
        DownloadTransport *t = takeFreeTransport();
        if (!t)
            return;   // no free transport: the rest waits
        const ArrivalTask task = m_arrivalQueue.takeFirst();

        if (task.isInit) {
            const Track &tr = (task.track == QLatin1String("video"))
                ? m_plan.video : m_plan.audio;
            launch(t, QUrl(tr.url), tr.initStart, tr.initEnd,
                   [this, task, t]
                   (const DownloadTransport::Response &r) {
                m_busy.remove(t);
                if (!r.isOk()) {
                    // It is NOT ignored: an init that never arrives leaves
                    // the file without its header.
                    onArrivalFailed(task, r);
                    return;
                }
                const QString path = (task.track == QLatin1String("video"))
                    ? m_videoInitPath : m_audioInitPath;
                writeFile(path, r.body);
                // The counter is touched in ONE place (the gate), so the
                // order of the answers does not matter.
                m_arrival.done();
                if (m_arrival.takeFinished())
                    startNext();
                else
                    pumpArrival();
            }, nullptr, tr.initEnd - tr.initStart + 1);
        } else {
            const Track &tr = (task.track == QLatin1String("video"))
                ? m_plan.video : m_plan.audio;
            const qint64 indexStart = task.indexStart;
            const qint64 indexEnd = indexStart + kIndexWindowBytes - 1;
            launch(t, QUrl(tr.url), indexStart, indexEnd,
                   [this, task, indexStart, tr, t]
                   (const DownloadTransport::Response &r) {
                m_busy.remove(t);
                if (!r.isOk()) {
                    // Without the index there are NO ranges: the download
                    // used to continue anyway and report "done" an empty file.
                    onArrivalFailed(task, r);
                    return;
                }
                const QString track = task.track;
                double total = 0.0;
                QVector<Range> ranges = rangesFromSidx(r.body, indexStart, &total);
                if (ranges.isEmpty()) {
                    // The track has NO index (a WebM/Opus one: its clusters
                    // are not indexed). It is cut in pieces of 2 MB instead,
                    // which is what makes it downloadable: YouTube throttles a
                    // request by its size.
                    ranges = rangesWithoutIndex(tr.contentLength, kChunkBytes);
                }
                if (track == QLatin1String("video")) {
                    m_videoRanges = ranges;
                    m_videoDone = QVector<bool>(ranges.size(), false);
                    m_videoSizes = QVector<qint64>(ranges.size(), 0);
                } else {
                    m_audioRanges = ranges;
                    m_audioDone = QVector<bool>(ranges.size(), false);
                    m_audioSizes = QVector<qint64>(ranges.size(), 0);
                }
                m_arrival.done();
                if (m_arrival.takeFinished())
                    startNext();
                else
                    pumpArrival();
            }, nullptr, indexEnd - indexStart + 1);
        }
    }
}

QString DownloadJob::fragmentPath(const QString &track, int index) const
{
    return fragmentPathIn(m_tempDir, track, index);
}

bool DownloadJob::writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(data);
    file.close();
    return true;
}

QVector<int> DownloadJob::doneSegments() const
{
    QVector<int> out;
    for (int i = 0; i < m_videoDone.size(); ++i)
        if (m_videoDone.at(i))
            out.append(i);
    for (int i = 0; i < m_audioDone.size(); ++i)
        if (m_audioDone.at(i))
            out.append(-(i + 2));
    return out;
}

// The mark of a fragment: the video ones are 0, 1, 2... and the audio ones
// are -(2), -(3)... (the same encoding that the index of the repository
// uses).
int DownloadJob::markOf(const QString &track, int index)
{
    return track == QLatin1String("video") ? index : -(index + 2);
}

void DownloadJob::startNext()
{
    if (!m_running || m_arrival.pending() > 0)
        return;

    for (DownloadTransport *t : m_pool) {
        // The busy flag is the job's (m_busy) and not the transport's: the
        // request is POSTED to the thread of the transport, so at this moment
        // the transport does not know yet that it is working. Asking it here
        // would hand out the same transport twice, and the second get() aborts
        // the first (a fragment that never answers and a counter that never
        // comes down).
        if (!t || m_busy.contains(t))
            continue;

        // The next fragment that is NOT on the disk yet. The ones that ARE
        // on the disk are not asked again (that is what makes the download
        // resumable) and the transport takes the next one: the old code
        // went to the next transport with `continue`, so with the first
        // eight fragments already downloaded it asked for NOTHING and the
        // download stayed still FOREVER (nothing was in flight, so nothing
        // called startNext again).
        bool isVideo = false;
        int index = -1;
        Range range;
        bool found = false;
        for (int guard = 0;
             guard < m_videoRanges.size() + m_audioRanges.size(); ++guard) {
            if (m_kind == dl::DownloadKind::Audio) {
                if (m_nextAudio >= m_audioRanges.size())
                    break;
                isVideo = false;
                index = m_nextAudio;
                range = m_audioRanges.at(index);
            } else if (m_nextVideo < m_videoRanges.size()) {
                isVideo = true;
                index = m_nextVideo;
                range = m_videoRanges.at(index);
            } else if (m_nextAudio < m_audioRanges.size()) {
                isVideo = false;
                index = m_nextAudio;
                range = m_audioRanges.at(index);
            } else {
                break;
            }
            ++(isVideo ? m_nextVideo : m_nextAudio);

            const QString track = isVideo ? QStringLiteral("video")
                                          : QStringLiteral("audio");
            const int mark = isVideo ? index : -(index + 2);
            if (m_alreadyDone.contains(mark)) {
                // It is on the disk: it is not asked again, and the
                // transport takes the next fragment.
                QVector<bool> *flags = isVideo ? &m_videoDone : &m_audioDone;
                if (index < flags->size()) {
                    (*flags)[index] = true;
                    QVector<qint64> *sizes =
                        isVideo ? &m_videoSizes : &m_audioSizes;
                    if (index < sizes->size())
                        (*sizes)[index] =
                            QFileInfo(fragmentPath(track, index)).size();
                }
                m_bytesDone = bytesOnDisk();
                emit progressChanged(m_bytesDone, m_bytesTotal);
                continue;
            }
            found = true;
            break;
        }
        if (!found)
            break;

        const QString track = isVideo ? QStringLiteral("video")
                                      : QStringLiteral("audio");
        ++m_inFlight;
        // The file of the fragment is opened HERE and the bytes are written
        // to it as they arrive: that is what makes the bar advance smoothly
        // (and the bytes are on the disk even if the download is killed).
        QFile *file = new QFile(fragmentPath(track, index));
        if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            delete file;
            --m_inFlight;
            const QString msg = QStringLiteral("no se pudo abrir el archivo del fragmento");
            setStatus(QStringLiteral("error"), msg);
            m_running = false;
            emit failed(msg);
            return;
        }
        QFile *const filePtr = file;
        const QString url = isVideo ? m_plan.video.url : m_plan.audio.url;
        launch(t, QUrl(url), range.start, range.end,
               [this, track, index, filePtr, t]
               (const DownloadTransport::Response &r) {
                   m_busy.remove(t);
                   --m_inFlight;
                   filePtr->close();
                   delete filePtr;
                   if (!r.isOk()) {
                       // The fragment is asked AGAIN: with a saturated IP
                       // some requests do not answer at all (a timeout),
                       // and the whole download used to fail for ONE of
                       // them. After a few tries it really fails.
                       const int tries =
                           ++m_fragmentTries[markOf(track, index)];
                       if (m_running && tries < kFragmentRetries) {
                           if (track == QLatin1String("video"))
                               m_nextVideo = index;
                           else
                               m_nextAudio = index;
                           startNext();
                           return;
                       }
                       if (m_running) {
                           // The signed URL ran out of its budget (the
                           // server answers 401/403): the download has to
                           // ask for the formats again. It is NOT an error
                           // of the video: it is an expired URL.
                           if (r.status == 401 || r.status == 403) {
                               setStatus(QStringLiteral("expired"));
                               m_running = false;
                               emit urlExpired();
                               return;
                           }
                           const QString msg = r.error.isEmpty()
                               ? nt::tr("error.dlPartFetch")
                               : r.error;
                           setStatus(QStringLiteral("error"), msg);
                           m_running = false;
                           emit failed(msg);
                       }
                       return;
                   }
                   m_fragmentTries.remove(markOf(track, index));
                   onFragmentDone(track, index, r.body.size());
               },
               [this, filePtr](const QByteArray &chunk) {
                   if (filePtr)
                       filePtr->write(chunk);
                   m_bytesDone += chunk.size();
                   emit progressChanged(m_bytesDone, m_bytesTotal);
               }, range.size());
    }

    if (m_inFlight == 0 && m_arrival.pending() == 0)
        maybeFinishSegments();
}

void DownloadJob::onFragmentDone(const QString &track, int index, qint64 size)
{
    if (!m_running)
        return;
    QVector<bool> *flags = (track == QLatin1String("video"))
        ? &m_videoDone : &m_audioDone;
    if (index >= 0 && index < flags->size()) {
        (*flags)[index] = true;
        QVector<qint64> *sizes = (track == QLatin1String("video"))
            ? &m_videoSizes : &m_audioSizes;
        if (index < sizes->size())
            (*sizes)[index] = QFileInfo(fragmentPath(track, index)).size();
    }
    m_bytesDone = bytesOnDisk();
    sampleSpeed();
    emit segmentDone(index);
    emit progressChanged(m_bytesDone, m_bytesTotal);
    startNext();
}

qint64 DownloadJob::bytesOnDisk() const
{
    qint64 total = 0;
    for (int i = 0; i < m_videoRanges.size(); ++i) {
        const QFileInfo info(fragmentPath(QStringLiteral("video"), i));
        if (info.exists())
            total += info.size();
    }
    for (int i = 0; i < m_audioRanges.size(); ++i) {
        const QFileInfo info(fragmentPath(QStringLiteral("audio"), i));
        if (info.exists())
            total += info.size();
    }
    return total;
}

double DownloadJob::progress() const
{
    int done = 0;
    for (int i = 0; i < m_videoDone.size(); ++i)
        if (m_videoDone.at(i))
            ++done;
    for (int i = 0; i < m_audioDone.size(); ++i)
        if (m_audioDone.at(i))
            ++done;
    const int total = m_videoDone.size() + m_audioDone.size();
    if (total <= 0)
        return 0.0;
    return qBound(0.0, double(done) / double(total), 1.0);
}

void DownloadJob::sampleSpeed()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastSampleMs == 0) {
        m_lastSampleMs = now;
        m_lastSampleBytes = m_bytesDone;
        return;
    }
    const qint64 elapsed = now - m_lastSampleMs;
    if (elapsed < 500)
        return;
    const qint64 delta = m_bytesDone - m_lastSampleBytes;
    if (delta > 0) {
        const qint64 instant = delta * 1000 / elapsed;
        m_bytesPerSecond = m_bytesPerSecond > 0
            ? (m_bytesPerSecond + instant) / 2
            : instant;
    }
    m_lastSampleMs = now;
    m_lastSampleBytes = m_bytesDone;
}

// Writes the resource: the init segment first (it is the header of the
// file) and then the fragments IN ORDER (the order is what makes it a
// playable stream).
bool DownloadJob::assembleTrack(const QString &path, const QString &track,
                                const QVector<Range> &ranges,
                                const QString &initPath)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QFile init(initPath);
    if (init.exists() && init.open(QIODevice::ReadOnly)) {
        while (!init.atEnd())
            file.write(init.read(256 * 1024));
        init.close();
    }
    for (int i = 0; i < ranges.size(); ++i) {
        QFile fragment(fragmentPath(track, i));
        if (!fragment.exists())
            continue;
        if (!fragment.open(QIODevice::ReadOnly)) {
            file.close();
            return false;
        }
        while (!fragment.atEnd())
            file.write(fragment.read(256 * 1024));
        fragment.close();
    }
    file.close();
    return true;
}

void DownloadJob::maybeFinishSegments()
{
    if (!m_running || m_inFlight > 0 || m_arrival.pending() > 0)
        return;
    // The finalize is already running (or done): the bytes are complete and
    // the assembly must not be started twice.
    if (m_muxing)
        return;

    for (int i = 0; i < m_videoDone.size(); ++i)
        if (!m_videoDone.at(i))
            return;
    for (int i = 0; i < m_audioDone.size(); ++i)
        if (!m_audioDone.at(i))
            return;

    // The download is done: the total is the size of what is on the disk,
    // which is the truth.
    m_bytesTotal = m_bytesDone;
    emit progressChanged(m_bytesDone, m_bytesTotal);

    startFinalize();
}

// The assembly (the fragments into one resource) and the mux (ffmpeg) run in
// their OWN thread, and that is not a detail: both of them are blocking work
// over ~1 GB and they ran in the thread of the interface. Measured on the
// machine that reported it, when a download ended the app froze 20.8 s
// (zero position ticks, zero repaints) and the player stayed frozen after the
// last byte had arrived, which is exactly what the report said.
//
// The mux is `ffmpeg -c copy` and it waits for it with waitForFinished, so it
// cannot be made asynchronous in place: it has to leave this thread.
void DownloadJob::startFinalize()
{
    m_muxing = true;
    setStatus(QStringLiteral("muxing"));

    FinalizeInput in;
    in.tempDir = m_tempDir;
    in.videoPath = m_videoPath;
    in.audioPath = m_audioPath;
    in.videoInitPath = m_videoInitPath;
    in.audioInitPath = m_audioInitPath;
    in.outputPath = m_outputPath;
    in.videoRanges = m_videoRanges;
    in.audioRanges = m_audioRanges;
    in.hasAudio = m_plan.audio.valid;
    in.audioOnly = m_kind == dl::DownloadKind::Audio;
    in.keepPartials = qEnvironmentVariableIsSet("NEVERTUBE_KEEP_PARTIALS");

    // The thread is detached from the job's lifetime of the members: it only
    // reads the copy `in` and, when it ends, it posts the result back HERE
    // (the state, the signals and the repository are the thread of the
    // interface's).
    QThread *thread = QThread::create([this, in]() {
        FinalizeResult r;
        r.outputPath = in.outputPath;

        if (!in.audioOnly
            && !assembleTrackFiles(in.tempDir, in.videoPath,
                                   QStringLiteral("video"), in.videoRanges,
                                   in.videoInitPath)) {
            r.assembled = false;
            r.error = QStringLiteral("no se pudo escribir el video");
            r.outputPath.clear();
        } else if (!in.audioOnly && in.hasAudio
                   && !assembleTrackFiles(in.tempDir, in.audioPath,
                                          QStringLiteral("audio"), in.audioRanges,
                                          in.audioInitPath)) {
            r.assembled = false;
            r.error = QStringLiteral("no se pudo escribir el audio");
            r.outputPath.clear();
        } else {
            const QString ffmpeg = findFfmpeg();
            if (ffmpeg.isEmpty()) {
                // The fragments are already on the disk: the mux can be
                // retried later when an ffmpeg shows up. The download is NOT
                // lost.
                r.error = nt::tr("error.dlFfmpegMissing");
                r.outputPath.clear();
            } else {
                MuxResult m;
                if (in.audioOnly)
                    m = convertToMp3(in.audioPath, in.outputPath, ffmpeg);
                else
                    m = muxToMp4(in.videoPath, in.audioPath, in.outputPath, ffmpeg);
                r.ok = m.ok;
                r.error = m.error;
                // The name of the file can change (the audio falls back to an
                // M4A when the ffmpeg has no mp3 encoder).
                if (m.ok && !m.outputPath.isEmpty())
                    r.outputPath = m.outputPath;
            }
        }

        // The cleanup of the partials is file work over ~1 GB too: it stays in
        // this thread.
        if (r.ok && !in.keepPartials) {
            QFile::remove(in.videoPath);
            QFile::remove(in.audioPath);
            QDir(in.tempDir).removeRecursively();
        }

        QMetaObject::invokeMethod(this, [this, r]() {
            onFinalizeDone(r.assembled, r.ok, r.error, r.outputPath);
        }, Qt::QueuedConnection);
    });
    thread->setObjectName(QStringLiteral("download-finalize"));
    m_finalThread = thread;
    thread->start();
}

// Back in the thread of the interface: the state, the signals and the
// repository are only touched here.
void DownloadJob::onFinalizeDone(bool assembled, bool ok, const QString &error,
                                 const QString &outputPath)
{
    if (m_finalThread) {
        // The thread posted this call as its last statement: the wait is
        // immediate and the delete cannot race with it.
        m_finalThread->wait();
        delete m_finalThread;
        m_finalThread = nullptr;
    }

    if (!assembled) {
        setStatus(QStringLiteral("error"), error);
        m_running = false;
        m_muxing = false;
        emit failed(error);
        return;
    }
    if (!ok) {
        // The fragments are NOT deleted when the mux fails: the bytes that
        // were downloaded are still there and the mux can be retried.
        setStatus(QStringLiteral("error"), error);
        m_running = false;
        m_muxing = false;
        emit failed(error);
        return;
    }
    if (!outputPath.isEmpty() && outputPath != m_outputPath)
        m_outputPath = outputPath;

    m_running = false;
    m_muxing = false;
    setStatus(QStringLiteral("done"));
    emit finished(m_outputPath);
}

} // namespace dl
