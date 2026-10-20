#include "download_manager.h"

#include "download_job.h"
#include "muxer.h"
#include "segment_plan.h"
#include "../yt.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace dl {

DownloadManager::DownloadManager(Yt *yt, DownloadRepo *repo, QObject *parent)
    : QObject(parent)
    , m_yt(yt)
    , m_repo(repo)
{
    m_dir = defaultDirectory();
    if (m_yt) {
        connect(m_yt, &Yt::masterBodyReady, this,
                &DownloadManager::onMasterReady);
        connect(m_yt, &Yt::masterBodyFailed, this,
                [this](const QString &vid, const QString &reason) {
            if (vid != m_currentId || !m_job)
                return;
            if (m_repo)
                m_repo->setStatus(vid, QStringLiteral("error"), reason);
            delete m_job;
            m_job = nullptr;
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
                            const QString &channel, int maxHeight)
{
    if (m_job) {
        // One download at a time (decision D15).
        emit failed(videoId, QStringLiteral("ya hay una descarga en curso"));
        return;
    }
    DownloadInfo existing;
    if (m_repo && m_repo->get(videoId, existing)) {
        // Already known: the state is shown, it is not queued again.
        emit changed();
        return;
    }

    m_currentId = videoId;
    m_title = title;
    m_channel = channel;
    m_maxHeight = maxHeight;

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

    m_yt->requestMaster(videoId);
}

void DownloadManager::onMasterReady(const QString &videoId,
                                    const QByteArray &masterBody)
{
    if (videoId != m_currentId)
        return;

    // The height wins over the codec for a DOWNLOAD (the opposite of the
    // player): the user asked for the file, so the best picture wins. The
    // codec only breaks the tie between two variants of the same height.
    auto rank = [](int height, const QString &codecs) {
        int r = 0;
        if (codecs.contains(QLatin1String("vp09")))
            r = 2;
        else if (codecs.contains(QLatin1String("avc1")))
            r = 1;
        return r * 100000 + height;
    };
    const Plan plan = planFromMaster(masterBody, m_maxHeight, QString(), rank);
    if (!plan.valid) {
        if (m_repo)
            m_repo->setStatus(videoId, QStringLiteral("error"), plan.error);
        emit changed();
        emit failed(videoId, plan.error);
        m_currentId.clear();
        return;
    }

    m_job = new DownloadJob(this);
    m_job->setDirectory(m_dir);
    connect(m_job, &DownloadJob::progressChanged, this,
            [this, videoId](qint64 done, qint64 total) {
        saveProgress();
        emit progressChanged(videoId, done, total);
    });
    connect(m_job, &DownloadJob::finished, this,
            [this, videoId](const QString &path) {
        if (m_repo) {
            DownloadInfo info;
            m_repo->get(videoId, info);
            info.finalPath = path;
            info.status = QStringLiteral("done");
            info.progress = 1.0;
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

    if (m_repo) {
        DownloadInfo info;
        m_repo->get(videoId, info);
        // The row always has the id, even if the read came back empty (the
        // row is created by start()).
        info.videoId = videoId;
        info.quality = QString::number(plan.video.height) + QLatin1Char('p');
        info.bytesTotal = plan.estimatedBytes;
        info.status = QStringLiteral("downloading");
        info.updatedAt = QDateTime::currentMSecsSinceEpoch();
        m_repo->upsert(info);
    }

    m_job->start(plan, videoId);
    emit changed();
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
    info.progress = info.bytesTotal > 0
        ? qBound(0.0, double(info.bytesDone) / double(info.bytesTotal), 1.0)
        : 0.0;
    info.status = m_job->status();
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
