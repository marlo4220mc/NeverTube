#include "download_job.h"

#include "download_transport.h"
#include "muxer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace dl {

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
    m_videoId = videoId;
    m_dir = m_dir.isEmpty() ? QDir::tempPath() : m_dir;
    QDir().mkpath(m_dir);

    // Each segment goes into its own file inside a temporary folder: the
    // bytes are on the disk as they arrive (so an interrupted download keeps
    // them and can be resumed) and nothing is reserved in advance.
    m_tempDir = m_dir + QLatin1Char('/') + m_videoId + QStringLiteral(".parts");
    QDir().mkpath(m_tempDir);

    m_videoPath = m_dir + QLatin1Char('/') + m_videoId + QStringLiteral(".video.ts");
    m_audioPath = m_dir + QLatin1Char('/') + m_videoId + QStringLiteral(".audio.ts");
    m_outputPath = m_dir + QLatin1Char('/') + m_videoId + QStringLiteral(".mp4");

    m_videoData = QVector<QByteArray>(plan.video.segments.size());
    m_audioData = QVector<QByteArray>(plan.audio.segments.size());
    m_videoDone = QVector<bool>(plan.video.segments.size(), false);
    m_audioDone = QVector<bool>(plan.audio.segments.size(), false);
    m_alreadyDone = alreadyDone;
    m_nextVideo = 0;
    m_nextAudio = 0;
    m_inFlight = 0;
    m_bytesDone = 0;
    m_bytesTotal = plan.estimatedBytes;
    m_muxing = false;
    m_running = true;

    setStatus(QStringLiteral("downloading"));
    buildTransportPool();
    fetchMediaPlaylists();
}

// The master says WHICH variant and WHICH audio rendition; the media
// playlist of each one says the SEGMENTS. That second playlist is a
// different URL (and the app never fetched it before: mpv follows the
// master by itself).
void DownloadJob::fetchMediaPlaylists()
{
    const QString videoUrl = m_plan.video.playlistUrl;
    const QString audioUrl = m_plan.audio.playlistUrl;

    // The video playlist comes first; the audio one is fetched with the same
    // transport afterwards (it is small).
    DownloadTransport *t = m_pool.first();
    t->get(QUrl(videoUrl), -1, -1, DownloadTransport::userAgent(),
           [this, audioUrl](const DownloadTransport::Response &r) {
        if (!r.isOk()) {
            const QString msg = r.error.isEmpty()
                ? QStringLiteral("no se pudo bajar la playlist de video")
                : r.error;
            setStatus(QStringLiteral("error"), msg);
            m_running = false;
            emit failed(msg);
            return;
        }
        m_plan.video.segments =
            dl::segmentsFromMediaPlaylist(r.body, &m_videoInit);
        if (m_plan.video.segments.isEmpty()) {
            const QString msg = QStringLiteral("la playlist de video no tiene segmentos");
            setStatus(QStringLiteral("error"), msg);
            m_running = false;
            emit failed(msg);
            return;
        }
        m_plan.videoDuration = dl::durationOfMediaPlaylist(r.body);
        m_videoData = QVector<QByteArray>(m_plan.video.segments.size());
        m_videoDone = QVector<bool>(m_plan.video.segments.size(), false);

        if (audioUrl.isEmpty()) {
            fetchInitSegment();
            return;
        }
        DownloadTransport *a = m_pool.first();
        a->get(QUrl(audioUrl), -1, -1, DownloadTransport::userAgent(),
               [this](const DownloadTransport::Response &ar) {
            if (ar.isOk())
                m_plan.audio.segments =
                    dl::segmentsFromMediaPlaylist(ar.body, &m_audioInit);
            m_audioData = QVector<QByteArray>(m_plan.audio.segments.size());
            m_audioDone = QVector<bool>(m_plan.audio.segments.size(), false);
            // The total: the BANDWIDTH of the master times the REAL duration
            // of the media playlist (the sum of the EXTINF). With the real
            // duration the estimate is close (measured: 1.7 %).
            if (m_plan.videoDuration > 0.0)
                m_plan.estimatedBytes =
                    dl::estimateBytes(m_plan.video.bandwidth, m_plan.videoDuration);
            m_bytesTotal = m_plan.estimatedBytes;
            fetchInitSegment();
        });
    });
}

// The init segment ("moov") of an fMP4 variant.
//
// It goes ONCE at the front of the file. Without it the file has no header
// and nothing can read it: ffmpeg answered "trun track id unknown, no tfhd
// was found" (measured, and the reason the first version of this job
// produced a file that could not be muxed).
void DownloadJob::fetchInitSegment()
{
    if (!m_videoInit.present) {
        // A variant in MPEG-TS has no init: the segments are complete on
        // their own.
        startNext();
        return;
    }

    DownloadTransport *t = m_pool.first();
    t->get(QUrl(m_videoInit.url), -1, -1, DownloadTransport::userAgent(),
           [this](const DownloadTransport::Response &r) {
        if (r.isOk())
            m_videoInitData = r.body;
        if (!m_audioInit.present) {
            startNext();
            return;
        }
        DownloadTransport *a = m_pool.first();
        a->get(QUrl(m_audioInit.url), -1, -1, DownloadTransport::userAgent(),
               [this](const DownloadTransport::Response &ar) {
            if (ar.isOk())
                m_audioInitData = ar.body;
            startNext();
        });
    });
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
    for (int i = 0; i < n; ++i) {
        auto *t = new DownloadTransport(this);
        m_pool.append(t);
    }
}

QString DownloadJob::segmentPath(const QString &track, int index) const
{
    return m_tempDir + QLatin1Char('/') + track + QLatin1Char('_')
           + QString::number(index) + QStringLiteral(".part");
}

QVector<int> DownloadJob::doneSegments() const
{
    QVector<int> out;
    for (int i = 0; i < m_videoDone.size(); ++i)
        if (m_videoDone.at(i))
            out.append(i);
    for (int i = 0; i < m_audioDone.size(); ++i)
        if (m_audioDone.at(i))
            out.append(-(i + 2));  // the audio ones are negative
    return out;
}

void DownloadJob::startNext()
{
    if (!m_running)
        return;

    for (DownloadTransport *t : m_pool) {
        if (!t || t->isBusy())
            continue;

        bool isVideo = false;
        int index = -1;
        QString url;
        if (m_nextVideo < m_plan.video.segments.size()) {
            isVideo = true;
            index = m_nextVideo;
            url = m_plan.video.segments.at(index).url;
        } else if (m_nextAudio < m_plan.audio.segments.size()) {
            isVideo = false;
            index = m_nextAudio;
            url = m_plan.audio.segments.at(index).url;
        } else {
            break;  // nothing left to fetch
        }

        if (isVideo)
            ++m_nextVideo;
        else
            ++m_nextAudio;

        // A segment that is already on disk from a previous attempt is not
        // fetched again: that is what makes the download resumable.
        const QString track = isVideo ? QStringLiteral("video") : QStringLiteral("audio");
        const int mark = isVideo ? index : -(index + 2);
        if (m_alreadyDone.contains(mark)) {
            QVector<bool> *flags = isVideo ? &m_videoDone : &m_audioDone;
            if (index < flags->size()) {
                (*flags)[index] = true;
                const qint64 size =
                    QFileInfo(segmentPath(track, index)).size();
                m_bytesDone += size;
            }
            emit progressChanged(m_bytesDone, m_bytesTotal);
            continue;
        }

        ++m_inFlight;
        t->get(QUrl(url), -1, -1, DownloadTransport::userAgent(),
               [this, track, index](const DownloadTransport::Response &r) {
                   --m_inFlight;
                   if (!r.isOk()) {
                       if (m_running) {
                           const QString msg =
                               r.error.isEmpty()
                                   ? QStringLiteral("fallo al bajar un segmento")
                                   : r.error;
                           setStatus(QStringLiteral("error"), msg);
                           m_running = false;
                           emit failed(msg);
                       }
                       return;
                   }
                   onSegment(track, index, r.body);
               });
    }

    if (m_inFlight == 0)
        maybeFinishSegments();
}

void DownloadJob::onSegment(const QString &track, int index,
                            const QByteArray &data)
{
    if (!m_running)
        return;
    writeSegment(track, index, data);
    emit segmentDone(index);
    emit progressChanged(m_bytesDone, m_bytesTotal);
    startNext();
}

void DownloadJob::writeSegment(const QString &track, int index,
                               const QByteArray &data)
{
    QVector<bool> *flags = (track == QLatin1String("video"))
        ? &m_videoDone : &m_audioDone;
    if (index < 0 || index >= flags->size())
        return;
    // The segment goes into its own file: the bytes are on the disk as they
    // arrive, so an interrupted download keeps them.
    QFile file(segmentPath(track, index));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(data);
        file.close();
    }
    (*flags)[index] = true;
    m_bytesDone += data.size();
}

// Writes the partial file: the init segment first (it is the header of the
// file) and then the segments IN ORDER (the order is what makes it a
// playable stream).
bool DownloadJob::assemblePartial(const QString &path, const QString &track,
                                  const QByteArray &initData)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    if (!initData.isEmpty())
        file.write(initData);
    const int count = (track == QLatin1String("video"))
        ? m_plan.video.segments.size() : m_plan.audio.segments.size();
    for (int i = 0; i < count; ++i) {
        QFile segment(segmentPath(track, i));
        if (!segment.exists())
            continue;
        if (!segment.open(QIODevice::ReadOnly)) {
            file.close();
            return false;
        }
        // Copied in blocks: a segment is ~1.3 MB and the whole file can be
        // 300 MB.
        while (!segment.atEnd())
            file.write(segment.read(256 * 1024));
        segment.close();
    }
    file.close();
    return true;
}

void DownloadJob::maybeFinishSegments()
{
    if (!m_running || m_inFlight > 0)
        return;

    // Are all the segments there?
    for (int i = 0; i < m_videoDone.size(); ++i) {
        if (!m_videoDone.at(i))
            return;
    }
    for (int i = 0; i < m_audioDone.size(); ++i) {
        if (!m_audioDone.at(i))
            return;
    }

    // Now the real total is known: the BANDWIDTH of the master is the PEAK
    // of the variant, not its average, so the estimate was about twice the
    // real size (measured: 380 MB estimated against 179 MB real). The
    // segments that are on disk are the truth.
    m_bytesTotal = m_bytesDone;
    emit progressChanged(m_bytesDone, m_bytesTotal);

    // Write the partials and mux them.
    if (!assemblePartial(m_videoPath, QStringLiteral("video"), m_videoInitData)
        || !assemblePartial(m_audioPath, QStringLiteral("audio"), m_audioInitData)) {
        const QString msg = QStringLiteral("no se pudo escribir el parcial");
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
        // The segments are already on disk: the mux can be retried later
        // when an ffmpeg shows up. The download is NOT lost.
        const QString msg = QStringLiteral("falta ffmpeg para unir las pistas");
        setStatus(QStringLiteral("error"), msg);
        m_running = false;
        emit failed(msg);
        return;
    }

    const MuxResult result = muxToMp4(m_videoPath, m_audioPath, m_outputPath, ffmpeg);
    if (!result.ok) {
        // The partials are NOT deleted when the mux fails: the bytes that
        // were downloaded are still there and the mux can be retried.
        setStatus(QStringLiteral("error"), result.error);
        m_running = false;
        emit failed(result.error);
        return;
    }

    // The mux worked: the partials and the segments are not needed any more.
    // They are twice the size of the file (measured: 171 MB of partials for
    // a 171 MB mp4), so leaving them would double what the download takes.
    QFile::remove(m_videoPath);
    QFile::remove(m_audioPath);
    QDir(m_tempDir).removeRecursively();

    m_running = false;
    m_muxing = false;
    setStatus(QStringLiteral("done"));
    emit finished(m_outputPath);
}

} // namespace dl
