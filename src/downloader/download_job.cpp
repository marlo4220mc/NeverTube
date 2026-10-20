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

void DownloadJob::start(const Plan &plan, const QString &videoId)
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

    m_videoPath = m_dir + QLatin1Char('/') + m_videoId + QStringLiteral(".video.ts");
    m_audioPath = m_dir + QLatin1Char('/') + m_videoId + QStringLiteral(".audio.ts");
    m_outputPath = m_dir + QLatin1Char('/') + m_videoId + QStringLiteral(".mp4");

    m_videoData = QVector<QByteArray>(plan.video.segments.size());
    m_audioData = QVector<QByteArray>(plan.audio.segments.size());
    m_videoDone = QVector<bool>(plan.video.segments.size(), false);
    m_audioDone = QVector<bool>(plan.audio.segments.size(), false);
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

// The init segment ("moov") of an fMP4 variant.
//
// It goes ONCE at the front of the file. Without it the file has no header
// and nothing can read it: ffmpeg answers "trun track id unknown, no tfhd
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
        m_plan.videoDuration = dl::durationOfMediaPlaylist(r.body);
        if (m_plan.video.segments.isEmpty()) {
            const QString msg = QStringLiteral("la playlist de video no tiene segmentos");
            setStatus(QStringLiteral("error"), msg);
            m_running = false;
            emit failed(msg);
            return;
        }
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
            // Con las dos playlists ya se puede afinar el total: el
            // BANDWIDTH del master es una estimacion, y el ancho de banda
            // de la pista de audio viene de su propia media playlist.
            // The total: the BANDWIDTH of the master times the REAL
            // duration of the media playlist (the sum of the EXTINF). With
            // the real duration the estimate is close (measured: 1.7 %).
            if (m_plan.videoDuration > 0.0)
                m_plan.estimatedBytes =
                    dl::estimateBytes(m_plan.video.bandwidth, m_plan.videoDuration);
            m_bytesTotal = m_plan.estimatedBytes;
            fetchInitSegment();
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

QVector<int> DownloadJob::doneSegments() const
{
    QVector<int> out;
    for (int i = 0; i < m_videoDone.size(); ++i)
        if (m_videoDone.at(i))
            out.append(i);
    for (int i = 0; i < m_audioDone.size(); ++i)
        if (m_audioDone.at(i))
            out.append(-1 - i);  // the audio ones are negative
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
        ++m_inFlight;

        const QString track = isVideo ? QStringLiteral("video") : QStringLiteral("audio");
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
    QVector<QByteArray> *store = (track == QLatin1String("video"))
        ? &m_videoData : &m_audioData;
    QVector<bool> *flags = (track == QLatin1String("video"))
        ? &m_videoDone : &m_audioDone;
    if (index < 0 || index >= store->size())
        return;
    (*store)[index] = data;
    (*flags)[index] = true;
    m_bytesDone += data.size();
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
    const QStringList partials = {m_videoPath, m_audioPath};
    const QVector<QByteArray> *stores[2] = {&m_videoData, &m_audioData};
    for (int s = 0; s < 2; ++s) {
        if (partials.at(s).isEmpty())
            continue;
        QFile file(partials.at(s));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const QString msg = QStringLiteral("no se pudo escribir el parcial");
            setStatus(QStringLiteral("error"), msg);
            m_running = false;
            emit failed(msg);
            return;
        }
        // The init segment goes FIRST: it is the header of the file.
        if (s == 0)
            file.write(m_videoInitData);
        else
            file.write(m_audioInitData);
        for (const QByteArray &chunk : *stores[s])
            file.write(chunk);
        file.close();
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
        setStatus(QStringLiteral("error"), result.error);
        m_running = false;
        emit failed(result.error);
        return;
    }

    m_running = false;
    m_muxing = false;
    setStatus(QStringLiteral("done"));
    emit finished(m_outputPath);
}

} // namespace dl
