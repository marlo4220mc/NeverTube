#include "download_manager.h"

#include "download_folder.h"

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
#include <QThread>

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
        connect(m_yt, &Yt::adaptiveFormatsFailed, this,
                [this](const QString &vid, const QString &reason) {
            if (vid != m_currentId)
                return;
            // There is NO job yet when the formats fail: the job is created
            // AFTER the formats arrive. Without this the row would stay in
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

void DownloadManager::setPlaybackActive(bool active)
{
    if (m_playbackActive == active)
        return;
    m_playbackActive = active;
    NT_DEBUG("E2E dlPlaybackActive=%d job=%d\n", active ? 1 : 0,
             m_job ? 1 : 0);
    // Priority is applied per-worker when the transport thread starts; keep it
    // in sync for any job created later.
    if (m_job) {
        m_job->setPlaybackActive(m_playbackActive);
    }
}

QString DownloadManager::defaultDirectory()
{
    // The folder of downloads of the system, which comes already translated
    // ("Descargas", "Downloads"). The app keeps its own subfolder so the
    // files are not mixed with the ones of the user.
    //
    // It is ONE function (downloads::folderOf), so the button of "Abrir
    // carpeta" opens exactly the folder where the files are written.
    return downloads::folderOf(QString());
}

QVector<DownloadInfo> DownloadManager::downloads() const
{
    return m_repo ? m_repo->all() : QVector<DownloadInfo>();
}

bool DownloadManager::state(const QString &videoId, DownloadInfo &out) const
{
    return m_repo && m_repo->get(videoId, out);
}

// True when the path is INSIDE the folder of the downloads (the one of the
// downloader). The app only deletes its own files: a row could name a file
// anywhere (a stale row, a hand-edited database) and the delete must not
// reach outside its own folder.
bool DownloadManager::isInsideDownloadFolder(const QString &path) const
{
    if (path.isEmpty() || m_dir.isEmpty())
        return false;
    const QString root = QDir(m_dir).absolutePath();
    const QString target = QDir(QFileInfo(path).absolutePath()).absolutePath();
    // The same folder, or a subfolder of it.
    return target == root
           || target.startsWith(root + QLatin1Char('/'));
}

bool DownloadManager::remove(const QString &videoId)
{
    // The download IN PROGRESS is CANCELLED: the X of a row has to stop
    // the job, not only delete the row. Without this the job kept
    // downloading, and when it finished it wrote the row again: the
    // download came back to the list and the downloader stayed busy.
    if (m_job && m_currentId == videoId) {
        // The job is stopped and its callbacks are IGNORED: they arrive
        // later (the event loop) and they wrote the row again, so the
        // download came back to the list.
        m_cancelled = videoId;
        m_job->stop();
        m_job->deleteLater();
        m_job = nullptr;
        m_currentId.clear();
    }
    DownloadInfo info;
    if (m_repo)
        m_repo->get(videoId, info);
    // The files of the download go with it: the mp4, the partials and the
    // temporary ones.
    //
    // ONLY the ones INSIDE the folder of the downloads: the path comes from
    // the repository and a row could name a file anywhere (a stale row, a
    // hand-edited database). The app never deletes a file that is not its
    // own.
    const QStringList paths = {info.finalPath, info.filePath,
                               m_dir + QLatin1Char('/') + videoId
                                   + QStringLiteral(".video.ts"),
                               m_dir + QLatin1Char('/') + videoId
                                   + QStringLiteral(".audio.ts")};
    for (const QString &p : paths) {
        if (p.isEmpty() || !QFileInfo::exists(p))
            continue;
        if (!isInsideDownloadFolder(p))
            continue;
        QFile::remove(p);
    }
    // The FOLDER of the fragments: it is where the bytes of the download
    // live, and it is the one that was left behind (hundreds of MB) when
    // the download was deleted.
    {
        const QString partsDir =
            m_dir + QLatin1Char('/') + videoId + QStringLiteral(".parts");
        if (QFileInfo::exists(partsDir))
            QDir(partsDir).removeRecursively();
    }
    const bool ok = m_repo ? m_repo->remove(videoId) : false;
    emit changed();
    return ok;
}

bool DownloadManager::retry(const QString &videoId)
{
    DownloadInfo info;
    if (!m_repo || !m_repo->get(videoId, info)) {
        return false;
    }
    // The row stays (with its index of fragments): the download resumes.
    //
    // The RESOLUTION of the row is kept ("720p" -> 720): the person paused
    // a 720p and it is not a 4K what they get when they continue.
    int height = 0;
    if (info.quality.endsWith(QLatin1Char('p'))) {
        bool ok = false;
        const int h = info.quality.left(info.quality.size() - 1).toInt(&ok);
        if (ok && h > 0)
            height = h;
    }
    start(videoId, info.title, info.channel, 0, 0, height, m_forcedCodec,
          m_forcedAudioOnly);
    return true;
}

bool DownloadManager::removeFromList(const QString &videoId)
{
    // The job is stopped (a download in course cannot go on without its
    // row) but the FILE is left on the disk.
    if (m_job && m_currentId == videoId) {
        m_cancelled = videoId;
        m_job->stop();
        m_job->deleteLater();
        m_job = nullptr;
        m_currentId.clear();
    }
    const bool ok = m_repo ? m_repo->remove(videoId) : false;
    emit changed();
    return ok;
}

void DownloadManager::pause(const QString &videoId)
{
    // The job is stopped when it is the one that is running. A download that
    // is paused from a previous session (there is no job) only needs the
    // row to change its state.
    const bool isTheJob = m_job && m_currentId == videoId;
    // The data of the job is read BEFORE releasing it: the job is deleted
    // right after (it is not a parent that survives), so reading it after
    // is a use-after-free (it crashed the app).
    QVector<int> parts;
    qint64 bytesDone = 0;
    if (isTheJob) {
        parts = m_job->doneSegments();
        bytesDone = m_job->bytesDone();
        m_job->stop();
        // The job is RELEASED: the downloader is free again. It used to stay
        // alive (stopped), and then the start() of the resume found a job in
        // place and refused it ("a download is already running"): the
        // download did not continue.
        m_job->deleteLater();
        m_job = nullptr;
        m_currentId.clear();
    }
    // The row goes to "paused" with the fragments that are already on the
    // disk: continuing resumes from there.
    if (m_repo) {
        DownloadInfo info;
        if (m_repo->get(videoId, info)) {
            info.status = QStringLiteral("paused");
            if (isTheJob) {
                info.parts = parts;
                info.bytesDone = bytesDone;
            }
            info.updatedAt = QDateTime::currentMSecsSinceEpoch();
            m_repo->upsert(info);
        }
    }
    emit changed();
}

void DownloadManager::resume(const QString &videoId)
{
    // Continuing IS the retry: the row is kept (with its fragments) and the
    // download starts again from where it was.
    retry(videoId);
}

void DownloadManager::start(const QString &videoId, const QString &title,
                            const QString &channel, int maxHeight,
                            int durationSeconds, int forcedHeight,
                            const QString &forcedCodec, bool forcedAudioOnly)
{
    if (m_job) {
        // One download at a time (decision D15).
        emit failed(videoId, nt::tr("error.dlBusy"));
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
    m_forcedCodec = forcedCodec;
    m_forcedAudioOnly = forcedAudioOnly;

    if (m_repo) {
        // The row that was already there is KEPT (with its index of
        // fragments): the upsert is a REPLACE, so a fresh DownloadInfo would
        // erase the fragments that are already on the disk and the resume
        // would be lost.
        DownloadInfo info;
        if (!m_repo->get(videoId, info))
            info.createdAt = QDateTime::currentMSecsSinceEpoch();
        info.videoId = videoId;
        info.title = title;
        info.channel = channel;
        info.status = QStringLiteral("pending");
        info.updatedAt = QDateTime::currentMSecsSinceEpoch();
        m_repo->upsert(info);
    }
    emit changed();

    m_yt->requestAdaptiveFormats(videoId);
}


// The adaptive formats arrived: the plan is built from them.
//
// BOTH tracks (video and audio) come from here, and both are fetched by byte
// range, because YouTube throttles a request by its size: the same audio
// asked for whole runs at 0.011 MB/s and a 2 MB range of it runs at
// 14 MB/s (measured). The master HLS is NOT used: its audio is AAC in LATM
// and the FFmpeg of this system cannot mux that.
void DownloadManager::onAdaptiveFormats(const QString &videoId,
                                       const QJsonArray &formats)
{
    NT_DEBUG("E2E dl formatos id=%s n=%d current=%s\n",
             videoId.toUtf8().constData(), int(formats.size()),
             m_currentId.toUtf8().constData());
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
    choice.codec = m_forcedCodec;
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

    if (choice.height > 0 && !choice.codec.isEmpty())
        plan = planWithVariant(plan, choice.height, choice.codec);
    else if (choice.height > 0)
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
    // The number of connections of the setting of Ajustes.
    m_job->setConcurrency(m_concurrency);
    m_job->setPlaybackActive(m_playbackActive);
    m_lastUiUpdateMs = 0;
    m_lastSaveMs = 0;
    connect(m_job, &DownloadJob::progressChanged, this,
            [this, videoId](qint64 done, qint64 total) {
        // The save is throttled inside (kSaveEveryMs): this signal arrives
        // with every chunk of ~64 KB.
        saveProgress();
        emit progressChanged(videoId, done, total);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastUiUpdateMs >= 250) {
            m_lastUiUpdateMs = now;
            emit changed();
        }
    });
    // The indexes of the fragments that are already on disk: they are what
    // the resume reads, so this one is saved at once.
    connect(m_job, &DownloadJob::segmentDone, this,
            [this, videoId](int) { saveProgress(true); });
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
    // The signed URL ran out of its budget (401/403): the formats are
    // asked again (with new URLs) and the download continues from the
    // fragments that are already on the disk. It is NOT an error of the
    // video: the URLs of the player response last a while and a long
    // download outlives them.
    connect(m_job, &DownloadJob::urlExpired, this,
            [this, videoId]() {
                NT_DEBUG("E2E dl url expirada id=%s -> pedir formatos\n",
                         videoId.toUtf8().constData());
                if (m_job) {
                    m_job->deleteLater();
                    m_job = nullptr;
                }
                // The row goes back to "downloading" (it was not a
                // failure) and the formats are asked again.
                if (m_repo) {
                    DownloadInfo info;
                    if (m_repo->get(videoId, info)) {
                        info.status = QStringLiteral("downloading");
                        info.updatedAt =
                            QDateTime::currentMSecsSinceEpoch();
                        m_repo->upsert(info);
                    }
                }
                m_currentId = videoId;
                // With forceRefresh: the cached URLs are the dead ones.
                m_yt->requestAdaptiveFormats(videoId, true);
                emit changed();
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

void DownloadManager::saveProgress(bool force)
{
    if (!m_repo || !m_job || m_currentId.isEmpty())
        return;
    // The throttle: the job reports progress on every ~64 KB chunk, and each
    // save is a SELECT plus an INSERT with its commit. See kSaveEveryMs.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!force && m_lastSaveMs != 0 && nowMs - m_lastSaveMs < kSaveEveryMs)
        return;
    m_lastSaveMs = nowMs;
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
    if (!m_cancelled.isEmpty()) {
        // The download was cancelled with the X: the row is gone and it
        // does not come back.
        m_cancelled.clear();
        if (m_job) {
            m_job->deleteLater();
            m_job = nullptr;
        }
        m_currentId.clear();
        emit changed();
        return;
    }
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
    if (!m_cancelled.isEmpty()) {
        // The download was cancelled with the X: nothing is written (the row
        // is gone and the files with it).
        m_cancelled.clear();
        if (m_job) {
            m_job->deleteLater();
            m_job = nullptr;
        }
        m_currentId.clear();
        emit changed();
        return;
    }
    if (m_job) {
        m_job->deleteLater();
        m_job = nullptr;
    }
    m_currentId.clear();
    emit changed();
    emit failed(id, reason);
}

} // namespace dl
