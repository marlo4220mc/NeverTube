#include "download_job.h"

#include "../i18n.h"

#include "download_transport.h"
#include "muxer.h"

#include "../debug.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

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
}

DownloadJob::DownloadJob(QObject *parent)
    : QObject(parent)
    , m_status(QStringLiteral("pending"))
{
}

DownloadJob::~DownloadJob()
{
    stop();
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
    m_extraPending = 0;
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
    if (m_pool.isEmpty() && !m_running)
        return;
    for (DownloadTransport *t : m_pool) {
        if (t)
            t->abort();
    }
    qDeleteAll(m_pool);
    m_pool.clear();
    m_running = false;
    m_inFlight = 0;
    if (m_status == QStringLiteral("downloading")
        || m_status == QStringLiteral("muxing"))
        setStatus(QStringLiteral("paused"));
}

void DownloadJob::buildTransportPool()
{
    qDeleteAll(m_pool);
    m_pool.clear();
    const int n = DownloadTransport::defaultConcurrency();
    for (int i = 0; i < n; ++i)
        m_pool.append(new DownloadTransport(this));
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
        if (t && !t->isBusy())
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
        if (track == QLatin1String("audio"))
            startNext();
        return;
    }

    // The init segment first (it is the header of the file).
    ++m_extraPending;
    DownloadTransport *ti = takeFreeTransport();
    if (!ti) {
        --m_extraPending;
        if (track == QLatin1String("audio"))
            startNext();
        return;
    }
    ti->get(QUrl(t.url), t.initStart, t.initEnd, DownloadTransport::userAgent(),
            [this, track](const DownloadTransport::Response &r) {
        if (r.isOk()) {
            const QString path = (track == QLatin1String("video"))
                ? m_videoInitPath : m_audioInitPath;
            writeFile(path, r.body);
        }
        --m_extraPending;
    });

    // Then the index: a window that covers it (the indexRange of the format
    // does not).
    const qint64 indexStart = t.indexStart > 0 ? t.indexStart : t.initEnd + 1;
    const qint64 indexEnd = indexStart + kIndexWindowBytes - 1;
    ++m_extraPending;
    DownloadTransport *ta = takeFreeTransport();
    if (!ta) {
        --m_extraPending;
        if (track == QLatin1String("audio"))
            startNext();
        return;
    }
    ta->get(QUrl(t.url), indexStart, indexEnd, DownloadTransport::userAgent(),
             [this, track, indexStart, t](const DownloadTransport::Response &r) {
        if (r.isOk()) {
            double total = 0.0;
            QVector<Range> ranges =
                rangesFromSidx(r.body, indexStart, &total);
            if (ranges.isEmpty()) {
                // The track has NO index (a WebM/Opus one: its clusters are
                // not indexed). It is cut in pieces of 2 MB instead, which is
                // what makes it downloadable: YouTube throttles a request by
                // its size.
                ranges = rangesWithoutIndex(t.contentLength, kChunkBytes);
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
        }
        --m_extraPending;
        if (m_extraPending == 0)
            startNext();
    });
}

QString DownloadJob::fragmentPath(const QString &track, int index) const
{
    return m_tempDir + QLatin1Char('/') + track + QLatin1Char('_')
           + QString::number(index) + QStringLiteral(".part");
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
    if (!m_running || m_extraPending > 0)
        return;

    for (DownloadTransport *t : m_pool) {
        if (!t || t->isBusy())
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
        t->get(QUrl(url), range.start, range.end,
               DownloadTransport::userAgent(),
               [this, track, index, filePtr](const DownloadTransport::Response &r) {
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
               });
    }

    if (m_inFlight == 0 && m_extraPending == 0)
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
    if (!m_running || m_inFlight > 0 || m_extraPending > 0)
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

    if (!assembleTrack(m_videoPath, QStringLiteral("video"), m_videoRanges,
                       m_videoInitPath)) {
        const QString msg = QStringLiteral("no se pudo escribir el video");
        setStatus(QStringLiteral("error"), msg);
        m_running = false;
        emit failed(msg);
        return;
    }
    if (m_plan.audio.valid
        && !assembleTrack(m_audioPath, QStringLiteral("audio"), m_audioRanges,
                          m_audioInitPath)) {
        const QString msg = QStringLiteral("no se pudo escribir el audio");
        setStatus(QStringLiteral("error"), msg);
        m_running = false;
        emit failed(msg);
        return;
    }

    startMux();
}

void DownloadJob::startMux()
{
    m_muxing = true;
    setStatus(QStringLiteral("muxing"));

    const QString ffmpeg = findFfmpeg();
    if (ffmpeg.isEmpty()) {
        // The fragments are already on the disk: the mux can be retried
        // later when an ffmpeg shows up. The download is NOT lost.
        const QString msg = QStringLiteral("falta ffmpeg para unir las pistas");
        setStatus(QStringLiteral("error"), msg);
        m_running = false;
        emit failed(msg);
        return;
    }

    MuxResult result;
    if (m_kind == dl::DownloadKind::Audio) {
        result = convertToMp3(m_audioPath, m_outputPath, ffmpeg);
    } else {
        result = muxToMp4(m_videoPath, m_audioPath, m_outputPath, ffmpeg);
    }
    if (!result.ok) {
        // The fragments are NOT deleted when the mux fails: the bytes that
        // were downloaded are still there and the mux can be retried.
        setStatus(QStringLiteral("error"), result.error);
        m_running = false;
        emit failed(result.error);
        return;
    }

    // The mux worked: the fragments are not needed any more. They are the
    // same size as the file, so leaving them would double what a download
    // takes on the disk.
    if (!qEnvironmentVariableIsSet("NEVERTUBE_KEEP_PARTIALS")) {
        QFile::remove(m_videoPath);
        QFile::remove(m_audioPath);
        QDir(m_tempDir).removeRecursively();
    }

    m_running = false;
    m_muxing = false;
    setStatus(QStringLiteral("done"));
    emit finished(m_outputPath);
}

} // namespace dl
