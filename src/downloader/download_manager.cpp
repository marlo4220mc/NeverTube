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
            // The answer carries the VIDEO ID, not the key: the mp3 of a video
            // ("<id>-audio") is the same video.
            if (vid != m_currentVideoId)
                return;
            // There is NO job yet when the formats fail: the job is created
            // AFTER the formats arrive. Without this the row would stay in
            // "pending" forever (measured: a video that YouTube refuses
            // left the list as "pending" and nothing happened).
            const QString key = m_currentId;
            if (m_repo)
                m_repo->setStatus(key, QStringLiteral("error"), reason);
            if (m_job) {
                m_job->deleteLater();
                m_job = nullptr;
            }
            m_currentId.clear();
            m_currentVideoId.clear();
            emit changed();
            emit failed(key, reason);
            // The queue goes on: a queued download must not wait for a
            // failure of the previous one.
            processQueue();
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
    // Kill switch, to tell "the player+download slows down" apart from "the
    // priority change is to blame" on a machine: with this set the downloader
    // behaves exactly as before this feature.
    if (qEnvironmentVariableIsSet("NEVERTUBE_NO_DL_LOWPRIO"))
        active = false;
    if (m_playbackActive == active)
        return;
    m_playbackActive = active;
    NT_DEBUG("E2E dlPlaybackActive=%d job=%d\n", active ? 1 : 0,
             m_job ? 1 : 0);
    // Priority is applied per-worker when the transport thread starts; keep it
    // in sync for any job created later.
    //
    // Only for a job that is RUNNING: a job that is still waiting for its
    // formats has no worker to lower, and its flag is passed at creation
    // (onAdaptiveFormats). Posting to a thread that does not exist yet only
    // adds noise to the log.
    if (m_job && m_job->isRunning()) {
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

bool DownloadManager::state(const QString &key, DownloadInfo &out) const
{
    return m_repo && m_repo->get(key, out);
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

bool DownloadManager::cancelInFlight(const QString &key)
{
    if (m_currentId != key)
        return false;
    if (m_job) {
        m_job->stop();
        m_job->deleteLater();
        m_job = nullptr;
    }
    m_currentId.clear();
    m_currentVideoId.clear();
    return true;
}

bool DownloadManager::remove(const QString &key)
{
    // The download IN PROGRESS is CANCELLED: the X of a row has to stop
    // the job, not only delete the row. Without this the job kept
    // downloading, and when it finished it wrote the row again: the
    // download came back to the list and the downloader stayed busy.
    //
    // It also covers the download that is only WAITING for its formats
    // (there is no job yet): its late answer was creating the job for a row
    // that had already been deleted.
    const bool wasInFlight = cancelInFlight(key);
    // A download that only WAITED in the queue goes with the row: without
    // this it started when the current one ended and came back to the list.
    dropQueued(key);
    DownloadInfo info;
    if (m_repo)
        m_repo->get(key, info);
    // The files of the download go with it: the mp4, the partials and the
    // temporary ones.
    //
    // ONLY the ones INSIDE the folder of the downloads: the path comes from
    // the repository and a row could name a file anywhere (a stale row, a
    // hand-edited database). The app never deletes a file that is not its
    // own.
    const QStringList paths = {info.finalPath, info.filePath,
                               m_dir + QLatin1Char('/') + key
                                   + QStringLiteral(".video.mp4"),
                               m_dir + QLatin1Char('/') + key
                                   + QStringLiteral(".audio.m4a"),
                               m_dir + QLatin1Char('/') + key
                                   + QStringLiteral(".audio.webm"),
                               // The names of the downloader before it wrote
                               // .mp4 tracks (a stale row can still name them).
                               m_dir + QLatin1Char('/') + key
                                   + QStringLiteral(".video.ts"),
                               m_dir + QLatin1Char('/') + key
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
    // the download was deleted. Same rule as the files: only inside the
    // folder of the downloads (the key comes from the repository).
    {
        const QString partsDir =
            m_dir + QLatin1Char('/') + key + QStringLiteral(".parts");
        if (QFileInfo::exists(partsDir) && isInsideDownloadFolder(partsDir))
            QDir(partsDir).removeRecursively();
    }
    const bool ok = m_repo ? m_repo->remove(key) : false;
    emit changed();
    // The downloader is free again: the next one of the queue goes on.
    if (wasInFlight)
        processQueue();
    return ok;
}

// See the header: the mp3 rows of the first version of the downloader shared
// the row (and the ".parts" folder) of the video, so their key is the plain
// video id. They are moved to the row of the mp3 ONCE, with their folder.
void DownloadManager::migrateLegacyAudioRow(DownloadInfo &info)
{
    if (!m_repo || info.quality != QLatin1String("mp3"))
        return;
    const QString key = info.identity();
    if (key != info.videoId) {
        // It already has a row of its own (the key carries the suffix).
        return;
    }
    const QString audioKey = downloadKey(info.videoId, true);
    DownloadInfo existing;
    if (m_repo->get(audioKey, existing)) {
        // The row of the mp3 already exists: the old one is a leftover (the
        // migration ran and the old row survived, or the mp3 was downloaded
        // again after the update). The ROW goes, the FILE stays: it is the
        // one of the row that already exists (the name is the title, so the
        // two can point at the same file).
        removeFromList(key);
        info.key = audioKey;
        return;
    }
    // The bytes that are already on the disk go with the row: the folder of
    // the fragments is named after the key.
    const QString oldParts = m_dir + QLatin1Char('/') + key
                             + QStringLiteral(".parts");
    const QString newParts = m_dir + QLatin1Char('/') + audioKey
                             + QStringLiteral(".parts");
    bool moved = true;
    if (QFileInfo::exists(oldParts))
        moved = QDir().rename(oldParts, newParts);
    // The index of the fragments is only valid with its bytes: if the folder
    // could not be moved, the row starts from zero instead of trusting an
    // index of files that are not there (the output would be corrupt).
    if (!moved)
        info.parts.clear();
    info.key = audioKey;
    info.updatedAt = QDateTime::currentMSecsSinceEpoch();
    m_repo->upsert(info);
    m_repo->remove(key);
}

bool DownloadManager::retry(const QString &key)
{
    DownloadInfo info;
    if (!m_repo || !m_repo->get(key, info)) {
        return false;
    }
    // A row of the first version (the mp3 shared the row of the video) is
    // moved to the row of the mp3 before it is resumed: see the header.
    migrateLegacyAudioRow(info);
    // The row stays (with its index of fragments): the download resumes.
    //
    // `key` is the IDENTITY of the download, not the video: the mp3 of a
    // video is "<videoId>-audio". The video id that YouTube knows is the one
    // in `info.videoId`, so THAT is what is asked for again (asking for
    // "abc-audio" answered nothing and the mp3 never resumed).
    // The IDENTITY of the row says whether it is the mp3: the key carries the
    // suffix. `quality` is the belt and braces of the migration above (a row
    // that could not be migrated is still an mp3 and must not be resumed as a
    // VIDEO: a full video download under the name of an mp3).
    const bool audioOnly = info.identity() != info.videoId
                           || info.quality == QLatin1String("mp3");
    // The RESOLUTION of the row is kept ("720p" -> 720): the person paused
    // a 720p and it is not a 4K what they get when they continue.
    int height = 0;
    if (info.quality.endsWith(QLatin1Char('p'))) {
        bool ok = false;
        const int h = info.quality.left(info.quality.size() - 1).toInt(&ok);
        if (ok && h > 0)
            height = h;
    }
    // The codec is NOT kept: the row stores the resolution, not the codec, and
    // reusing the last one clicked (of another video) made no sense. Empty =
    // the default of that resolution (the smallest), the same rule of the
    // first download.
    start(info.videoId, info.title, info.channel, 0, 0, height, QString(),
          audioOnly);
    return true;
}

bool DownloadManager::removeFromList(const QString &key)
{
    // The job is stopped (a download in course cannot go on without its
    // row) but the FILE is left on the disk.
    const bool wasInFlight = cancelInFlight(key);
    dropQueued(key);
    const bool ok = m_repo ? m_repo->remove(key) : false;
    emit changed();
    if (wasInFlight)
        processQueue();
    return ok;
}

void DownloadManager::pause(const QString &key)
{
    // The job is stopped when it is the one that is running. A download that
    // is paused from a previous session (there is no job) only needs the
    // row to change its state.
    const bool isTheJob = m_job && m_currentId == key;
    // The data of the job is read BEFORE releasing it: the job is deleted
    // right after (it is not a parent that survives), so reading it after
    // is a use-after-free (it crashed the app).
    QVector<int> parts;
    qint64 bytesDone = 0;
    if (isTheJob) {
        parts = m_job->doneSegments();
        bytesDone = m_job->bytesDone();
    }
    // The job is RELEASED (and the download that was only waiting for its
    // formats is forgotten, so the answer does not start it): the downloader
    // is free again. It used to stay alive (stopped), and then the start()
    // of the resume found a job in place and refused it ("a download is
    // already running"): the download did not continue.
    const bool wasInFlight = cancelInFlight(key);
    // The queued download of that row waits no more: it is paused.
    dropQueued(key);
    // The row goes to "paused" with the fragments that are already on the
    // disk: continuing resumes from there.
    if (m_repo) {
        DownloadInfo info;
        if (m_repo->get(key, info)) {
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
    // The downloader is free: the next one of the queue goes on.
    if (wasInFlight)
        processQueue();
}

void DownloadManager::resume(const QString &key)
{
    // Continuing IS the retry: the row is kept (with its fragments) and the
    // download starts again from where it was.
    retry(key);
}

void DownloadManager::pauseAll()
{
    if (!m_repo)
        return;
    // The snapshot is a copy (`all()` returns a QVector): pause() writes rows
    // while this loop reads them.
    const QVector<DownloadInfo> all = m_repo->all();
    for (const DownloadInfo &info : all) {
        const QString key = info.identity();
        if (info.status == QLatin1String("done")
            || info.status == QLatin1String("error")
            || info.status == QLatin1String("paused")) {
            continue;
        }
        pause(key);
    }
}

void DownloadManager::resumeAll()
{
    if (!m_repo)
        return;
    // Oldest first, like resumePending(): the first one starts and the rest
    // queue behind it.
    const QVector<DownloadInfo> all = m_repo->all();
    for (int i = all.size() - 1; i >= 0; --i) {
        const DownloadInfo &info = all.at(i);
        if (info.status == QLatin1String("paused"))
            retry(info.identity());
    }
}

int DownloadManager::sweepOrphanParts()
{
    if (!m_repo || m_dir.isEmpty())
        return 0;
    QDir dir(m_dir);
    if (!dir.exists())
        return 0;
    int removed = 0;
    const QStringList folders =
        dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : folders) {
        const QString suffix = QStringLiteral(".parts");
        if (!name.endsWith(suffix))
            continue;
        const QString key = name.left(name.size() - suffix.size());
        // The download of that key still exists: its bytes are the resume.
        DownloadInfo info;
        if (m_repo->get(key, info))
            continue;
        // Belt and braces: the folder of the download in course is never
        // touched, even if its row could not be read.
        if (!m_currentId.isEmpty() && key == m_currentId)
            continue;
        const QString path = dir.filePath(name);
        if (!isInsideDownloadFolder(path))
            continue;
        if (QDir(path).removeRecursively())
            ++removed;
        NT_DEBUG("E2E dl barrido huerfano=%s ok=%d\n",
                 name.toUtf8().constData(), removed > 0 ? 1 : 0);
    }
    return removed;
}

void DownloadManager::start(const QString &videoId, const QString &title,
                            const QString &channel, int maxHeight,
                            int durationSeconds, int forcedHeight,
                            const QString &forcedCodec, bool forcedAudioOnly)
{
    // The IDENTITY of the download: video and its MP3 are two different
    // downloads (two files, two rows), so the audio one does not replace
    // the video one in the list.
    const QString key = downloadKey(videoId, forcedAudioOnly);

    // Already in the list AND finished: the state is shown, it is not queued
    // again (the file is on the disk).
    DownloadInfo existing;
    if (m_repo && m_repo->get(key, existing)) {
        // A row of the first version: the mp3 shared the row of the video, so
        // the VIDEO of that id found the row of the mp3 ("done") and refused
        // to download, silently. The mp3 is moved to its own row first, and
        // then the row of the video is read again (it may not exist).
        if (!forcedAudioOnly && existing.quality == QLatin1String("mp3")
            && existing.identity() == existing.videoId) {
            migrateLegacyAudioRow(existing);
            if (!m_repo->get(key, existing))
                existing = DownloadInfo();
        }
        if (existing.status == QLatin1String("done")) {
            emit changed();
            return;
        }
    }

    if (m_job || !m_currentId.isEmpty()) {
        // One download at a time: the request waits in the queue. The
        // downloader is busy while the job runs AND while it is waiting for
        // the formats of the previous one (there is no job yet, but its
        // identity is taken: starting another one would lose that answer).
        // It is not queued twice (the same video pressed twice) and the row
        // is written NOW, so the person sees it as "pending" while the
        // other one runs. The one that is RUNNING is not queued again either.
        bool alreadyQueued = (m_currentId == key);
        for (const DownloadRequest &r : m_queue) {
            if (downloadKey(r.videoId, r.forcedAudioOnly) == key) {
                alreadyQueued = true;
                break;
            }
        }
        if (!alreadyQueued)
            m_queue.enqueue({videoId, title, channel, maxHeight,
                             durationSeconds, forcedHeight, forcedCodec,
                             forcedAudioOnly});
        writePendingRow(key, videoId, title, channel);
        emit changed();
        return;
    }

    m_currentId = key;
    m_currentVideoId = videoId;
    m_title = title;
    m_channel = channel;
    m_maxHeight = maxHeight;
    m_durationSeconds = durationSeconds;
    // When the height (or the audio only) comes from the command line
    // there is nobody to ask: the dialog is skipped.
    m_forcedHeight = forcedHeight;
    m_forcedCodec = forcedCodec;
    m_forcedAudioOnly = forcedAudioOnly;

    writePendingRow(key, videoId, title, channel);
    emit changed();

    // A manager without Yt is the one the tests build to exercise the QUEUE
    // (which is pure state) without network: there is nobody to ask.
    if (m_yt)
        m_yt->requestAdaptiveFormats(videoId);
}

// The row of a queued (or just started) download: the fragments that are
// already on the disk are KEPT (the upsert is a REPLACE, so a fresh
// DownloadInfo would erase them and the resume would be lost).
void DownloadManager::writePendingRow(const QString &key,
                                      const QString &videoId,
                                      const QString &title,
                                      const QString &channel)
{
    if (!m_repo)
        return;
    DownloadInfo info;
    if (!m_repo->get(key, info))
        info.createdAt = QDateTime::currentMSecsSinceEpoch();
    info.key = key;
    info.videoId = videoId;
    if (!title.isEmpty())
        info.title = title;
    if (!channel.isEmpty())
        info.channel = channel;
    // A download that is already running is not sent back to "pending": the
    // pending row is only for the ones that wait.
    if (info.status != QLatin1String("downloading"))
        info.status = QStringLiteral("pending");
    info.updatedAt = QDateTime::currentMSecsSinceEpoch();
    m_repo->upsert(info);
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
    // The answer comes with the VIDEO id; the row of the download is `key`
    // (the video, or "<videoId>-audio" for its mp3).
    if (videoId != m_currentVideoId)
        return;
    const QString key = m_currentId;

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
            m_repo->setStatus(key, QStringLiteral("error"), plan.error);
        emit changed();
        emit failed(key, plan.error);
        m_currentId.clear();
        m_currentVideoId.clear();
        // The queue goes on with the next one.
        processQueue();
        return;
    }

    if (choice.height > 0 && !choice.codec.isEmpty())
        plan = planWithVariant(plan, choice.height, choice.codec);
    else if (choice.height > 0)
        plan = planWithHeight(plan, choice.height);
    plan.kind = choice.audioOnly ? dl::DownloadKind::Audio
                                 : dl::DownloadKind::Video;
    // The total of an mp3 is the audio track, NOT the video plus the audio
    // (the row showed "2 MB / 300 MB" and the bar never filled).
    if (plan.kind == dl::DownloadKind::Audio && plan.audio.contentLength > 0)
        plan.estimatedBytes = plan.audio.contentLength;

    if (m_repo) {
        DownloadInfo info;
        // The row is the KEY (the mp3 has its own): reading it by the video id
        // wrote the state and the path of the mp3 in the row of the VIDEO, and
        // the two downloads of the same video stopped coexisting.
        m_repo->get(key, info);
        info.key = key;
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

    // A job of the same row that is still around (a duplicated answer, a
    // forced refresh) is released before the new one: overwriting the
    // pointer would leak it and leave its threads running.
    if (m_job) {
        m_job->stop();
        m_job->deleteLater();
        m_job = nullptr;
    }
    m_job = new DownloadJob(this);
    m_job->setDirectory(m_dir);
    // The number of connections of the setting of Ajustes.
    m_job->setConcurrency(m_concurrency);
    m_job->setPlaybackActive(m_playbackActive);
    m_lastUiUpdateMs = 0;
    m_lastSaveMs = 0;
    // The number of THIS job: the callbacks compare it against m_jobSerial
    // and ignore themselves when the job is no longer the current one (it
    // was cancelled, paused or replaced and the event loop delivered its
    // signals late).
    const quint64 serial = ++m_jobSerial;
    connect(m_job, &DownloadJob::progressChanged, this,
            [this, key, serial](qint64 done, qint64 total) {
        if (serial != m_jobSerial || !m_job)
            return;
        // The save is throttled inside (kSaveEveryMs): this signal arrives
        // with every chunk of ~64 KB.
        saveProgress();
        emit progressChanged(key, done, total);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastUiUpdateMs >= 250) {
            m_lastUiUpdateMs = now;
            emit changed();
        }
    });
    // The indexes of the fragments that are already on disk: they are what
    // the resume reads, so this one is saved at once.
    connect(m_job, &DownloadJob::segmentDone, this,
            [this, serial](int) {
        if (serial != m_jobSerial || !m_job)
            return;
        saveProgress(true);
    });
    // A fragment that failed and is being asked AGAIN. It is written in the
    // row as a transient note (the column `error`, which the page shows while
    // the state is "downloading"): for the person the download looked stuck
    // while the fragments were being retried without a word. `attempt == 0`
    // clears it (the fragment went through).
    connect(m_job, &DownloadJob::fragmentRetrying, this,
            [this, key, serial](int attempt, int max) {
        if (serial != m_jobSerial || !m_job || !m_repo)
            return;
        const QString note = attempt > 0
            ? nt::tr("downloads.retrying").arg(attempt).arg(max)
            : QString();
        m_repo->setStatus(key, QStringLiteral("downloading"), note);
        emit changed();
    });
    connect(m_job, &DownloadJob::finished, this,
            [this, key, videoId, serial](const QString &path) {
        // A job that is no longer the current one (its row was deleted, or
        // the download was paused or replaced) does not write anything: the
        // row is gone or belongs to another download now.
        if (serial != m_jobSerial || !m_job)
            return;
        if (m_repo) {
            DownloadInfo info;
            m_repo->get(key, info);
            info.key = key;
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
            [this, key, videoId, serial]() {
                if (serial != m_jobSerial || !m_job)
                    return;
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
                    if (m_repo->get(key, info)) {
                        info.status = QStringLiteral("downloading");
                        info.updatedAt =
                            QDateTime::currentMSecsSinceEpoch();
                        m_repo->upsert(info);
                    }
                }
                // The key is KEPT (it is the identity of the download, not the
                // video): only the id of the video is asked for again.
                m_currentId = key;
                m_currentVideoId = videoId;
                // With forceRefresh: the cached URLs are the dead ones.
                m_yt->requestAdaptiveFormats(videoId, true);
                emit changed();
            });
    connect(m_job, &DownloadJob::failed, this,
            [this, key, serial](const QString &reason) {
        // The job is no longer the current one: nothing is written (the row
        // was deleted or the download was paused/replaced).
        if (serial != m_jobSerial || !m_job)
            return;
        if (m_repo)
            m_repo->setStatus(key, QStringLiteral("error"), reason);
        onJobFailed(reason);
    });

    // The segments that are already on disk from a previous attempt.
    QVector<int> already;
    if (m_repo) {
        DownloadInfo info;
        if (m_repo->get(key, info))
            already = info.parts;
    }
    m_job->start(plan, key, m_title, already);
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
    if (m_job) {
        m_job->deleteLater();
        m_job = nullptr;
    }
    m_currentId.clear();
    m_currentVideoId.clear();
    emit changed();
    emit finished(id, path);
    processQueue();
}

void DownloadManager::onJobFailed(const QString &reason)
{
    const QString id = m_currentId;
    if (m_job) {
        m_job->deleteLater();
        m_job = nullptr;
    }
    m_currentId.clear();
    m_currentVideoId.clear();
    emit changed();
    emit failed(id, reason);
    processQueue();
}

// Processes the next download in the queue.
void DownloadManager::processQueue()
{
    // One at a time: a start() while a job runs would only queue the request
    // again (and it would be taken out of the queue in the wrong order). The
    // identity is also checked: a download that is WAITING for its formats
    // has no job yet but it is still the one in flight.
    //
    // The loop is NOT cosmetic: start() can refuse a request (a row that is
    // already "done"), and with a single attempt that request left the queue
    // and NOTHING was started: the whole queue stayed still until the next
    // event. Each turn takes one request out, so the loop always ends.
    while (!m_job && m_currentId.isEmpty() && !m_queue.isEmpty()) {
        const DownloadRequest req = m_queue.dequeue();
        start(req.videoId, req.title, req.channel, req.maxHeight,
              req.durationSeconds, req.forcedHeight, req.forcedCodec,
              req.forcedAudioOnly);
    }
}

// The downloads that a previous session left half way. See the header: the
// queue is in memory, so a row that waited for the other one to end was
// "pending" and stayed that way for ever.
void DownloadManager::resumePending()
{
    if (!m_repo)
        return;
    // `all()` comes newest first; the downloads are resumed in the order they
    // were created (the queue is a FIFO).
    const QVector<DownloadInfo> all = m_repo->all();
    for (int i = all.size() - 1; i >= 0; --i) {
        DownloadInfo info = all.at(i);
        const bool waiting = info.status == QLatin1String("pending");
        const bool interrupted = info.status == QLatin1String("downloading")
                                 || info.status == QLatin1String("muxing");
        if (!waiting && !interrupted)
            continue;
        // A row of the first version (the mp3 shared the row of the video) is
        // moved to the row of the mp3, with its folder, before anything else:
        // the migrated row is the one that starts.
        migrateLegacyAudioRow(info);
        // An interrupted row is written back as "pending" BEFORE starting:
        // the job that was running is gone, and a row that says "downloading"
        // with nothing running is a lie (and it also made writePendingRow()
        // keep the state of a download that does not exist).
        if (interrupted) {
            info.status = QStringLiteral("pending");
            info.bytesPerSecond = 0;
            info.updatedAt = QDateTime::currentMSecsSinceEpoch();
            m_repo->upsert(info);
        }
        // The same rule as the retry: the row knows if it is the mp3 (the
        // migration above leaves it in its own row) and at what resolution it
        // was going. `quality` stays as the belt and braces of a row that
        // could not be migrated.
        const bool audioOnly = info.identity() != info.videoId
                               || info.quality == QLatin1String("mp3");
        int height = 0;
        if (!audioOnly && info.quality.endsWith(QLatin1Char('p'))) {
            bool ok = false;
            const int h = info.quality.left(info.quality.size() - 1).toInt(&ok);
            if (ok && h > 0)
                height = h;
        }
        start(info.videoId, info.title, info.channel, 0, 0, height, QString(),
              audioOnly);
    }
}

// The request of that row is not going to run: it leaves the queue.
void DownloadManager::dropQueued(const QString &key)
{
    for (int i = 0; i < m_queue.size(); ++i) {
        const DownloadRequest &r = m_queue.at(i);
        if (downloadKey(r.videoId, r.forcedAudioOnly) == key) {
            m_queue.removeAt(i);
            return;
        }
    }
}

} // namespace dl
