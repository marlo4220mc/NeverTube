#include "download_job.h"

#include "../i18n.h"

#include "download_transport.h"
#include "download_folder.h"
#include "muxer.h"

#include "../debug.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QUrl>

#if defined(Q_OS_WIN)
#include <windows.h>
#elif defined(Q_OS_UNIX)
#include <sys/resource.h>
#endif

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

namespace {

// The ``nice`` value of a download worker while video is playing. 10 is the
// usual "below normal" in POSIX (Windows has its own constant).
constexpr int kDownloadNice = 10;

// The priority the thread had BEFORE the downloader touched it, so the
// restore can bring it back to THAT one. Restoring a fixed "normal" was
// wrong in both directions: on Windows a thread started above normal ended up
// in normal, and on Unix a worker that came from nice 5 was asked to go back
// to 0, which fails silently without privileges (so it stayed at 10 for ever).
// The state is per THREAD (thread_local): the pool thread and the finalize
// thread each keep their own.
#if defined(Q_OS_WIN)
thread_local int tOriginalWinPriority = THREAD_PRIORITY_NORMAL;
thread_local bool tOriginalWinSaved = false;
#elif defined(Q_OS_UNIX)
thread_local int tOriginalNice = 0;
thread_local bool tOriginalNiceSaved = false;
#endif

void applyThreadPriority(bool belowNormal)
{
#if defined(Q_OS_WIN)
    HANDLE h = GetCurrentThread();
    if (!belowNormal) {
        const int target = tOriginalWinSaved ? tOriginalWinPriority
                                             : THREAD_PRIORITY_NORMAL;
        const BOOL ok = SetThreadPriority(h, target);
        tOriginalWinSaved = false;
        // The probe of the scheduling: with NEVERTUBE_DEBUG=1 the log says
        // which priority the thread of the downloads gets, so "the FPS did
        // not come back" can be told apart from "the priority was never
        // applied".
        NT_DEBUG("E2E dlThreadPriority below=0 ok=%d restored=%d\n",
                 ok ? 1 : 0, target);
        return;
    }
    // The first time it is lowered the original is remembered; a second
    // "playback started" must not overwrite it with BELOW_NORMAL.
    if (!tOriginalWinSaved) {
        const int current = GetThreadPriority(h);
        if (current != THREAD_PRIORITY_ERROR_RETURN) {
            tOriginalWinPriority = current;
            tOriginalWinSaved = true;
        }
    }
    const BOOL ok = SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
    NT_DEBUG("E2E dlThreadPriority below=1 ok=%d original=%d\n", ok ? 1 : 0,
             tOriginalWinPriority);
#elif defined(Q_OS_UNIX)
    errno = 0;
    const int nic = getpriority(PRIO_PROCESS, 0);
    if (nic == -1 && errno != 0)
        return;
    if (!belowNormal) {
        // setpriority() cannot RAISE the priority without privileges, so the
        // restore fails silently when the original was lower than the
        // current one and the process is not root: it is the OS, not a bug.
        const int target = tOriginalNiceSaved ? tOriginalNice : 0;
        setpriority(PRIO_PROCESS, 0, target);
        tOriginalNiceSaved = false;
        NT_DEBUG("E2E dlThreadPriority below=0 nice=%d restored=%d\n",
                 getpriority(PRIO_PROCESS, 0), target);
        return;
    }
    if (!tOriginalNiceSaved) {
        tOriginalNice = nic;
        tOriginalNiceSaved = true;
    }
    if (nic < kDownloadNice)
        setpriority(PRIO_PROCESS, 0, kDownloadNice);
    NT_DEBUG("E2E dlThreadPriority below=1 nice=%d original=%d\n",
             getpriority(PRIO_PROCESS, 0), tOriginalNice);
#else
    Q_UNUSED(belowNormal);
#endif
}

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

void DownloadJob::setPlaybackActive(bool active)
{
    if (m_playbackActive.exchange(active) == active)
        return;
    // The priority has to change in the THREAD THAT OWNS IT, and it has to
    // change WHILE it runs: a download started with no video playing keeps
    // its workers in NORMAL until the video starts, and that is exactly the
    // case where the frames drop (measured on the Lenovo: the FPS fell from
    // the FIRST second of the download, before any fragment was muxed). The
    // functor is posted to the thread of the transports and runs there.
    if (!m_home || !m_thread || !m_thread->isRunning())
        return;
    // The value goes BY VALUE: the functor does not touch the job (it could
    // outlive it on a shutdown that goes wrong) and only needs the flag.
    const bool belowNormal = m_playbackActive.load();
    QMetaObject::invokeMethod(
        m_home, [belowNormal]() { applyThreadPriority(belowNormal); },
        Qt::QueuedConnection);
}

void DownloadJob::setStatus(const QString &status, const QString &error)
{
    m_status = status;
    m_error = error;
    emit statusChanged(m_status, m_error);
}

void DownloadJob::start(const Plan &plan, const QString &key,
                        const QString &title,
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
    m_videoId = key;
    m_title = title;
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
    //
    // The NAME is the title of the video (the id says nothing to a person),
    // and `uniquePathIn` appends " (2)" when that name is already taken: the
    // video and the mp3 of the SAME video have the same title and neither of
    // them overwrites the other.
    const QString audioExt = plan.audio.format.value(QStringLiteral("mimeType"))
                                .toString().contains(QLatin1String("webm"))
                                 ? QStringLiteral(".audio.webm")
                                 : QStringLiteral(".audio.m4a");
    m_videoPath = m_dir + QLatin1Char('/') + m_videoId + QStringLiteral(".video.mp4");
    m_audioPath = m_dir + QLatin1Char('/') + m_videoId + audioExt;
    const QString ext = (m_kind == dl::DownloadKind::Audio)
                            ? QStringLiteral(".mp3")
                            : QStringLiteral(".mkv");
    m_outputPath = downloads::uniquePathIn(
        m_dir, downloads::safeFileName(m_title, m_videoId), ext);
    m_videoInitPath = m_tempDir + QStringLiteral("/video.init");
    m_audioInitPath = m_tempDir + QStringLiteral("/audio.init");

    m_alreadyDone = alreadyDone;
    m_nextVideo = 0;
    m_nextAudio = 0;
    m_inFlight = 0;
    m_fragmentBytes.reset();
    m_fragmentByteSlots = 0;
    m_lastProgressPostMs.store(0);
    m_arrival.reset();
    m_arrivalQueue.clear();
    m_bytesDone.store(0);
    m_speed.clear();
    m_bytesPerSecond = 0;
    m_bytesTotal = (m_kind == dl::DownloadKind::Audio)
                       ? plan.audio.contentLength
                       : plan.estimatedBytes;
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
    // of the job (the counters, the queue) is only touched by the thread of
    // the interface. The BYTES are not: each transport writes its fragment in
    // its own thread (see DownloadTransport::get()), because writing them
    // here is what dropped the player to 2-5 fps with a full core of the
    // interface busy in QFile/QFileInfo.
    m_thread = new QThread();
    m_thread->setObjectName(QStringLiteral("download-transport"));
    const bool belowNormal = m_playbackActive.load();
    connect(m_thread, &QThread::started, this, [belowNormal]() {
        applyThreadPriority(belowNormal);
    }, Qt::DirectConnection);
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
                         qint64 expectedBytes, const QString &sinkPath)
{
    if (!t)
        return;
    m_busy.insert(t);
    const QString ua = DownloadTransport::userAgent();
    QMetaObject::invokeMethod(t, [t, url, rangeStart, rangeEnd, ua, done,
                                  progress, expectedBytes, sinkPath]() {
        t->get(url, rangeStart, rangeEnd, ua, done, progress, expectedBytes,
               sinkPath);
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
                // The answer arrives in the thread of the transports: the
                // state of the job is posted to ITS thread (see the header).
                postToJob([this, task, t, r]() {
                    if (!m_running)
                        return;
                    m_busy.remove(t);
                    if (!r.isOk()) {
                        // It is NOT ignored: an init that never arrives leaves
                        // the file without its header.
                        onArrivalFailed(task, r);
                        return;
                    }
                    const QString path =
                        (task.track == QLatin1String("video"))
                            ? m_videoInitPath : m_audioInitPath;
                    writeFile(path, r.body);
                    // The counter is touched in ONE place (the gate), so the
                    // order of the answers does not matter.
                    m_arrival.done();
                    if (m_arrival.takeFinished())
                        startNext();
                    else
                        pumpArrival();
                });
            }, nullptr, tr.initEnd - tr.initStart + 1);
        } else {
            const Track &tr = (task.track == QLatin1String("video"))
                ? m_plan.video : m_plan.audio;
            const qint64 indexStart = task.indexStart;
            const qint64 indexEnd = indexStart + kIndexWindowBytes - 1;
            launch(t, QUrl(tr.url), indexStart, indexEnd,
                   [this, task, indexStart, tr, t]
                   (const DownloadTransport::Response &r) {
                postToJob([this, task, indexStart, tr, t, r]() {
                    if (!m_running)
                        return;
                    m_busy.remove(t);
                    if (!r.isOk()) {
                        // Without the index there are NO ranges: the download
                        // used to continue anyway and report "done" an empty
                        // file.
                        onArrivalFailed(task, r);
                        return;
                    }
                    const QString track = task.track;
                    double total = 0.0;
                    QVector<Range> ranges =
                        rangesFromSidx(r.body, indexStart, &total);
                    if (ranges.isEmpty()) {
                        // The track has NO index (a WebM/Opus one: its
                        // clusters are not indexed). It is cut in pieces of
                        // 2 MB instead, which is what makes it downloadable:
                        // YouTube throttles a request by its size.
                        ranges = rangesWithoutIndex(tr.contentLength,
                                                    kChunkBytes);
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
                });
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
    return dl::fragmentMark(track == QLatin1String("video"), index);
}

void DownloadJob::startNext()
{
    if (!m_running || m_arrival.pending() > 0)
        return;

    // One slot of bytes per fragment (the video first and then the audio). It
    // is done HERE, in the thread of the job and before anything is launched,
    // because the thread of the transports reads it while a fragment arrives.
    //
    // (`slots` is a macro of Qt —it expands to nothing unless QT_NO_KEYWORDS—
    // so the variable is called slotCount.)
    const int slotCount = m_videoRanges.size() + m_audioRanges.size();
    if (m_fragmentByteSlots != slotCount) {
        std::atomic<qint64> *memory =
            new std::atomic<qint64>[size_t(slotCount > 0 ? slotCount : 1)];
        for (int i = 0; i < slotCount; ++i)
            memory[i].store(0);
        m_fragmentBytes.reset(memory);
        m_fragmentByteSlots = slotCount;
    }

    for (DownloadTransport *t : m_pool) {
        // The busy flag is the job's (m_busy) and not the transport's: the
        // request is POSTED to the thread of the transport, so at this moment
        // the transport does not know yet that it is working. Asking it here
        // would hand out the same transport twice, and the second get() aborts
        // the first (a fragment that never answers and a counter that never
        // comes down).
        if (!t || m_busy.contains(t))
            continue;

        // The next fragment that is NOT on the disk yet. The walk (and the
        // SKIP of the ones that are already there, which is what makes the
        // resume work) is a pure function with its own selftest:
        // dl::pickNextFragment. Doing the skip per transport is what left a
        // resumed download asking for NOTHING (nothing in flight, nothing
        // calling the allocator again).
        const FragmentPick pick = pickNextFragment(
            m_kind == dl::DownloadKind::Audio, m_videoRanges.size(),
            m_audioRanges.size(), m_nextVideo, m_nextAudio, m_alreadyDone);

        // The fragments that the walk went over are on the disk: their flag
        // and their bytes are accounted here, with ONE stat for each of them
        // (not a stat of every fragment of the download: this runs once per
        // fragment, so the whole resume measures n and not n^2).
        const int skippedVideo =
            pick.videoAdvance - (pick.found && pick.video ? 1 : 0);
        const int skippedAudio =
            pick.audioAdvance - (pick.found && !pick.video ? 1 : 0);
        if (skippedVideo > 0 || skippedAudio > 0) {
            auto accountSkipped = [this](bool video, int from, int count) {
                QVector<bool> *flags = video ? &m_videoDone : &m_audioDone;
                QVector<qint64> *sizes = video ? &m_videoSizes : &m_audioSizes;
                const QString track = video ? QStringLiteral("video")
                                            : QStringLiteral("audio");
                for (int i = from; i < from + count && i < flags->size(); ++i) {
                    (*flags)[i] = true;
                    qint64 size = 0;
                    if (i < sizes->size()) {
                        size = QFileInfo(fragmentPath(track, i)).size();
                        (*sizes)[i] = size;
                    }
                    m_bytesDone.fetch_add(size);
                }
            };
            accountSkipped(true, m_nextVideo, skippedVideo);
            accountSkipped(false, m_nextAudio, skippedAudio);
            emit progressChanged(m_bytesDone.load(), m_bytesTotal);
        }
        m_nextVideo += pick.videoAdvance;
        m_nextAudio += pick.audioAdvance;
        if (!pick.found)
            break;

        const bool isVideo = pick.video;
        const int index = pick.index;
        const Range range = isVideo ? m_videoRanges.at(index)
                                    : m_audioRanges.at(index);

        const QString track = isVideo ? QStringLiteral("video")
                                      : QStringLiteral("audio");
        ++m_inFlight;
        // The fragment is written by the TRANSPORT, in the thread of the
        // transports: opening, writing and closing the file here (the thread
        // of the interface) was what dropped the player to 2-5 fps. The
        // thread of the interface only receives how many bytes arrived.
        const QString path = fragmentPath(track, index);
        const int mark = markOf(track, index);
        const QString url = isVideo ? m_plan.video.url : m_plan.audio.url;
        launch(t, QUrl(url), range.start, range.end,
               [this, track, index, mark, t]
               (const DownloadTransport::Response &r) {
                   // In the thread of the transports: the body is posted to the
                   // thread of the job, so the state machine only ever runs in
                   // one thread (see the header). The answers of a transport
                   // are delivered in order, so the progress of this fragment
                   // has already been accounted when the body runs.
                   postToJob([this, track, index, mark, t, r]() {
                       onFragmentResponse(track, index, mark, t, r);
                   });
               },
               [this, track, index](qint64 bytes) {
                   // In the thread of the transports: ONLY atomics here, and
                   // the progress of the interface is posted at most every
                   // kProgressPostMs (this is called on every ~64 KB chunk).
                   const int slot = byteSlot(track, index);
                   if (slot >= 0)
                       m_fragmentBytes[slot].fetch_add(bytes);
                   m_bytesDone.fetch_add(bytes);
                   postProgress();
               }, range.size(), path);
    }

    if (m_inFlight == 0 && m_arrival.pending() == 0)
        maybeFinishSegments();
}

void DownloadJob::onFragmentDone(const QString &track, int index)
{
    if (!m_running)
        return;
    QVector<bool> *flags = (track == QLatin1String("video"))
        ? &m_videoDone : &m_audioDone;
    QVector<qint64> *sizes = (track == QLatin1String("video"))
        ? &m_videoSizes : &m_audioSizes;
    const int mark = markOf(track, index);
    // The size on the disk is the truth. It is corrected ONCE per fragment
    // (the transport counts the body as it arrives): measuring EVERY
    // fragment on every answer was O(n^2) QFileInfo/GetFileAttributesExW
    // calls in the thread of the interface (n = the ~460 fragments of a
    // video), and that is what kept the player from painting.
    const qint64 real = QFileInfo(fragmentPath(track, index)).size();
    // The bytes that the transport reported for this fragment are replaced by
    // the ones that are really on the disk.
    m_bytesDone.fetch_add(real - takeFragmentBytes(track, index));
    if (m_bytesDone.load() < 0)
        m_bytesDone.store(0);
    if (index >= 0 && index < flags->size())
        (*flags)[index] = true;
    if (index >= 0 && index < sizes->size())
        (*sizes)[index] = real;
    sampleSpeed();
    emit segmentDone(index);
    emit progressChanged(m_bytesDone.load(), m_bytesTotal);
    startNext();
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
    // The speed over the last seconds (SpeedWindow): the average since the
    // start kept a high number for minutes after the CDN had already squeezed
    // the connection, and after a resume it counted the bytes of the previous
    // session, which were not moved in this one.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_speed.add(now, m_bytesDone.load());
    m_bytesPerSecond = m_speed.bytesPerSecond(now);
}

// --- the bridge between the thread of the transports and the thread of the job ---

void DownloadJob::postToJob(std::function<void()> fn)
{
    // The context is `this`: a call that was posted while the job was alive is
    // DROPPED by Qt if the job is gone, instead of touching a dead object.
    QMetaObject::invokeMethod(this, std::move(fn), Qt::QueuedConnection);
}

int DownloadJob::byteSlot(const QString &track, int index) const
{
    if (index < 0)
        return -1;
    const bool video = track == QLatin1String("video");
    // The video occupies the first slots and the audio the ones after it: the
    // "mark" cannot be used as an index (the video 2 and the audio 0 are 2 and
    // -2, and both are small numbers).
    const int base = video ? 0 : m_videoRanges.size();
    const int slot = base + index;
    if (slot >= m_fragmentByteSlots)
        return -1;
    return slot;
}

qint64 DownloadJob::takeFragmentBytes(const QString &track, int index)
{
    const int slot = byteSlot(track, index);
    if (slot < 0 || !m_fragmentBytes)
        return 0;
    return m_fragmentBytes[slot].exchange(0);
}

void DownloadJob::postProgress()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 last = m_lastProgressPostMs.load();
    if (last != 0 && now - last < kProgressPostMs)
        return;
    // Only ONE post per window: the transports run in one thread, but the
    // check is atomic so a second call cannot slip in between.
    if (!m_lastProgressPostMs.compare_exchange_strong(last, now))
        return;
    postToJob([this]() {
        if (!m_running)
            return;
        // The speed is measured where its state lives.
        sampleSpeed();
        emit progressChanged(m_bytesDone.load(), m_bytesTotal);
    });
}

void DownloadJob::onFragmentResponse(const QString &track, int index, int mark,
                                     DownloadTransport *t,
                                     const DownloadTransport::Response &r)
{
    m_busy.remove(t);
    --m_inFlight;
    if (!m_running)
        return;
    if (!r.isOk()) {
        // The bytes of the attempt that failed are not kept in the counter:
        // the fragment is asked again from zero (the transport removed the
        // partial file).
        m_bytesDone.fetch_sub(takeFragmentBytes(track, index));
        if (m_bytesDone.load() < 0)
            m_bytesDone.store(0);
        emit progressChanged(m_bytesDone.load(), m_bytesTotal);
        // The fragment is asked AGAIN: with a saturated IP some requests do
        // not answer at all (a timeout), and the whole download used to fail
        // for ONE of them. After a few tries it really fails.
        const int tries = ++m_fragmentTries[mark];
        if (tries < kFragmentRetries) {
            if (track == QLatin1String("video"))
                m_nextVideo = index;
            else
                m_nextAudio = index;
            // The retry is SAID: for the person the download looked stuck
            // while the fragments were being asked again without a word.
            emit fragmentRetrying(tries, kFragmentRetries);
            startNext();
            return;
        }
        // The signed URL ran out of its budget (the server answers 401/403):
        // the download has to ask for the formats again. It is NOT an error of
        // the video: it is an expired URL.
        if (r.status == 401 || r.status == 403) {
            setStatus(QStringLiteral("expired"));
            m_running = false;
            emit urlExpired();
            return;
        }
        const QString msg = r.error.isEmpty() ? nt::tr("error.dlPartFetch")
                                              : r.error;
        setStatus(QStringLiteral("error"), msg);
        m_running = false;
        emit failed(msg);
        return;
    }
    // The fragment went through: if it was being retried, the note of the row
    // stops saying it (attempt 0).
    if (m_fragmentTries.remove(mark) > 0)
        emit fragmentRetrying(0, kFragmentRetries);
    onFragmentDone(track, index);
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
    m_bytesTotal = m_bytesDone.load();
    emit progressChanged(m_bytesDone.load(), m_bytesTotal);

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
        applyThreadPriority(m_playbackActive.load());
        FinalizeResult r;
        r.outputPath = in.outputPath;

        // 1. Ensamblar video (si no es solo audio)
        if (!in.audioOnly) {
            if (!assembleTrackFiles(in.tempDir, in.videoPath,
                                   QStringLiteral("video"), in.videoRanges,
                                   in.videoInitPath)) {
                r.assembled = false;
                r.error = QStringLiteral("no se pudo escribir el video");
                r.outputPath.clear();
            }
        }

        // 2. Ensamblar audio (si tiene audio)
        if (r.assembled && in.hasAudio) {
            if (!assembleTrackFiles(in.tempDir, in.audioPath,
                                    QStringLiteral("audio"), in.audioRanges,
                                    in.audioInitPath)) {
                r.assembled = false;
                r.error = QStringLiteral("no se pudo escribir el audio");
                r.outputPath.clear();
            }
        }

        // 3. Mux/Convertir (solo si todo lo anterior salió bien)
        if (r.assembled) {
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
    // The mux said it worked: the FILE has to be there and have bytes. A row
    // that says "done" pointing at a 0-byte file is worse than a failure (the
    // person finds out when pressing play), so it is checked here and the
    // download fails with its own message.
    if (!dl::outputIsUsable(outputPath)) {
        const QString reason = nt::tr("error.dlMuxResult");
        setStatus(QStringLiteral("error"), reason);
        m_running = false;
        m_muxing = false;
        emit failed(reason);
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
