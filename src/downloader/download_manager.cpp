#include "download_manager.h"

#include "download_dialog.h"
#include "../debug.h"

#include "download_job.h"
#include "muxer.h"
#include "segment_plan.h"
#include "../i18n.h"
#include "../ui/format.h"
#include "../yt.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace dl {

DownloadManager::DownloadManager(Yt *yt, DownloadRepo *repo, QWidget *window,
                                 QObject *parent)
    : QObject(parent)
    , m_yt(yt)
    , m_window(window)
    , m_repo(repo)
{
    m_dir = defaultDirectory();
    if (m_yt) {
        connect(m_yt, &Yt::adaptiveFormatsReady, this,
                &DownloadManager::onAdaptiveFormats);
        connect(m_yt, &Yt::masterBodyFailed, this,
                [this](const QString &vid, const QString &reason) {
            if (vid != m_currentId)
                return;
            // There is NO job yet when the master fails: the job is created
            // AFTER the master arrives. Without this the row would stay in
            // "pending" forever (measured: a video that YouTube refuses
            // left the list as "pending" and nothing happened).
            if (m_repo)
                m_repo->setStatus(vid, QStringLiteral("error"), reason);
            if (m_job) {
                m_job->deleteLater();
                m_job = nullptr;
            }
            m_currentId.clear();
            emit changed();
            emit failed(vid, reason);
        });
    }
}

DownloadManager::~DownloadManager()
{
    if (m_job) {
        m_job->stop();
        delete m_job;
    }
}

QString DownloadManager::defaultDirectory()
{
    // The folder of downloads of the system, which comes already translated
    // ("Descargas", "Downloads"). The app keeps its own subfolder so the
    // files are not mixed with the ones of the user.
    QString base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (base.isEmpty())
        base = QDir::tempPath();
    return base + QLatin1String("/NeverTube");
}

bool DownloadManager::hasFfmpeg()
{
    return !findFfmpeg().isEmpty();
}

QVector<DownloadInfo> DownloadManager::downloads() const
{
    return m_repo ? m_repo->all() : QVector<DownloadInfo>();
}

bool DownloadManager::state(const QString &videoId, DownloadInfo &out) const
{
    return m_repo && m_repo->get(videoId, out);
}

bool DownloadManager::remove(const QString &videoId)
{
    DownloadInfo info;
    if (m_repo)
        m_repo->get(videoId, info);
    // The files of the download go with it: the mp4, the partials and the
    // temporary ones.
    const QStringList paths = {info.finalPath, info.filePath,
                               m_dir + QLatin1Char('/') + videoId
                                   + QStringLiteral(".video.ts"),
                               m_dir + QLatin1Char('/') + videoId
                                   + QStringLiteral(".audio.ts")};
    for (const QString &p : paths) {
        if (!p.isEmpty() && QFileInfo::exists(p))
            QFile::remove(p);
    }
    const bool ok = m_repo ? m_repo->remove(videoId) : false;
    emit changed();
    return ok;
}

void DownloadManager::start(const QString &videoId, const QString &title,
                            const QString &channel, int maxHeight,
                            int durationSeconds, int forcedHeight,
                            bool forcedAudioOnly)
{
    if (m_job) {
        // One download at a time (decision D15).
        emit failed(videoId, QStringLiteral("ya hay una descarga en curso"));
        return;
    }
    DownloadInfo existing;
    if (m_repo && m_repo->get(videoId, existing)) {
        // Already known: the state is shown, it is not queued again. BUT a
        // download that was interrupted (or that failed) is RESUMED: the
        // segments that are already on disk are not fetched again.
        if (existing.status == QLatin1String("done")) {
            emit changed();
            return;
        }
    }

    m_currentId = videoId;
    m_title = title;
    m_channel = channel;
    m_maxHeight = maxHeight;
    m_durationSeconds = durationSeconds;
    // When the height (or the audio only) comes from the command line
    // there is nobody to ask: the dialog is skipped.
    m_forcedHeight = forcedHeight;
    m_forcedAudioOnly = forcedAudioOnly;

    if (m_repo) {
        DownloadInfo info;
        info.videoId = videoId;
        info.title = title;
        info.channel = channel;
        info.status = QStringLiteral("pending");
        info.createdAt = QDateTime::currentMSecsSinceEpoch();
        info.updatedAt = info.createdAt;
        m_repo->upsert(info);
    }
    emit changed();

    m_yt->requestAdaptiveFormats(videoId);
}


// The adaptive formats arrived: the plan can be built (the master is
// already there).
// The adaptive formats arrived: the plan is built from them.
//
// The VIDEO comes from the master HLS (it is already fragmented by YouTube
// into segments of ~1.3 MB, and those are not throttled) and the AUDIO from
// the adaptive formats (a clean mp4; the audio of the master is AAC in LATM
// and the FFmpeg of this system cannot mux that).
//
// Both are fetched by byte range, because YouTube throttles a request by its
// size: the same audio asked for whole runs at 0.011 MB/s and a 2 MB range
// of it runs at 14 MB/s (measured).
void DownloadManager::onAdaptiveFormats(const QString &videoId,
                                       const QJsonArray &formats)
{
    if (videoId != m_currentId)
        return;

    // The same ranking that the player uses, in miniature: VP9 beats h264.
    // The HEIGHT wins for a download (the opposite of the player): the user
    // asked for the file, so the best picture wins, and among the variants
    // of the same height the SMALLEST one is taken (the size is what
    // matters for a file).
    struct Rank
    {
        static int of(int height, const QString &mime)
        {
            int r = 0;
            if (mime.contains(QLatin1String("vp09"))
                || mime.contains(QLatin1String("vp9")))
                r = 2;
            else if (mime.contains(QLatin1String("avc1")))
                r = 1;
            return r * 100000 + height;
        }
    };

    // The choice (the resolution and whether only the audio is wanted) comes
    // from the menu of the Player, which knows the resolutions of this video
    // without asking YouTube for anything.
    DownloadChoice choice;
    choice.height = m_forcedHeight;
    choice.audioOnly = m_forcedAudioOnly;

    // Now the plan, with the resolution that was chosen.
    Plan plan = planFromFormats(formats, m_maxHeight, QString(), &Rank::of);
    if (!plan.valid) {
        if (m_repo)
            m_repo->setStatus(videoId, QStringLiteral("error"), plan.error);
        emit changed();
        emit failed(videoId, plan.error);
        m_currentId.clear();
        return;
    }

    if (choice.height > 0)
        plan = planWithHeight(plan, choice.height);
    plan.kind = choice.audioOnly ? dl::DownloadKind::Audio
                                 : dl::DownloadKind::Video;

    if (m_repo) {
        DownloadInfo info;
        m_repo->get(videoId, info);
        info.videoId = videoId;
        if (info.title.isNull())
            info.title = QString();
        int height = 0;
        for (const VideoOption &o : plan.options) {
            if (o.height == choice.height)
                height = o.height;
        }
        if (height == 0 && !plan.options.isEmpty())
            height = plan.options.first().height;
        info.quality = choice.audioOnly
            ? QStringLiteral("mp3")
            : QString::number(height) + QLatin1Char('p');
        info.status = QStringLiteral("downloading");
        info.bytesTotal = plan.estimatedBytes;
        info.updatedAt = QDateTime::currentMSecsSinceEpoch();
        m_repo->upsert(info);
    }

    m_job = new DownloadJob(this);
    m_job->setDirectory(m_dir);
    m_lastUiUpdateMs = 0;
    connect(m_job, &DownloadJob::progressChanged, this,
            [this, videoId](qint64 done, qint64 total) {
        saveProgress();
        emit progressChanged(videoId, done, total);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastUiUpdateMs >= 250) {
            m_lastUiUpdateMs = now;
            emit changed();
        }
    });
    // The indexes of the fragments that are already on disk: they are what
    // the resume reads.
    connect(m_job, &DownloadJob::segmentDone, this,
            [this, videoId](int) { saveProgress(); });
    connect(m_job, &DownloadJob::finished, this,
            [this, videoId](const QString &path) {
        if (m_repo) {
            DownloadInfo info;
            m_repo->get(videoId, info);
            info.videoId = videoId;
            info.finalPath = path;
            info.status = QStringLiteral("done");
            info.progress = 1.0;
            info.bytesDone = QFileInfo(path).size();
            info.updatedAt = QDateTime::currentMSecsSinceEpoch();
            m_repo->upsert(info);
        }
        onJobFinished(path);
    });
    connect(m_job, &DownloadJob::failed, this,
            [this, videoId](const QString &reason) {
        if (m_repo)
            m_repo->setStatus(videoId, QStringLiteral("error"), reason);
        onJobFailed(reason);
    });

    // The segments that are already on disk from a previous attempt.
    QVector<int> already;
    if (m_repo) {
        DownloadInfo info;
        if (m_repo->get(videoId, info))
            already = info.parts;
    }
    m_job->start(plan, videoId, already);
    emit changed();
}

void DownloadManager::buildPlan()
{
    onAdaptiveFormats(m_currentId, m_adaptiveFormats);
}

void DownloadManager::saveProgress()
{
    if (!m_repo || !m_job || m_currentId.isEmpty())
        return;
    DownloadInfo info;
    if (!m_repo->get(m_currentId, info))
        return;
    info.bytesDone = m_job->bytesDone();
    info.bytesTotal = m_job->bytesTotal();
    // The progress is by SEGMENTS: the total of a download is not known
    // until it is done (the BANDWIDTH of the master is the PEAK of the
    // variant) and a fraction of the bytes made the bar jump backwards as
    // the estimate was corrected.
    info.progress = m_job->progress();
    info.status = m_job->status();
    info.bytesPerSecond = m_job->bytesPerSecond();
    info.parts = m_job->doneSegments();
    info.updatedAt = QDateTime::currentMSecsSinceEpoch();
    m_repo->upsert(info);
}

void DownloadManager::onJobFinished(const QString &path)
{
    const QString id = m_currentId;
    if (m_job) {
        m_job->deleteLater();
        m_job = nullptr;
    }
    m_currentId.clear();
    emit changed();
    emit finished(id, path);
}

void DownloadManager::onJobFailed(const QString &reason)
{
    const QString id = m_currentId;
    if (m_job) {
        m_job->deleteLater();
        m_job = nullptr;
    }
    m_currentId.clear();
    emit changed();
    emit failed(id, reason);
}

} // namespace dl
