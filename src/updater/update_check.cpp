#include "update_check.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>

#include "../i18n.h"

namespace nt {

namespace {

// GitHub asks every client to identify itself, and refusing to do so gets a
// 403 from its edge (the API documents the User-Agent as REQUIRED).
QByteArray userAgent()
{
    return QByteArrayLiteral("NeverTube/") + appVersion().toUtf8();
}

// The installer of a release is downloaded from the URL the API itself gave
// for that asset, and only from GitHub: if a response ever pointed somewhere
// else, that is not our installer and the download does not start.
bool isGithubAssetUrl(const QUrl &url)
{
    const QString host = url.host().toLower();
    return host == QLatin1String("github.com")
        || host == QLatin1String("objects.githubusercontent.com")
        || host.endsWith(QLatin1String(".githubusercontent.com"));
}

// One version, split the way SemVer orders them.
struct ParsedVersion
{
    QList<long long> core;
    QStringList pre;
    bool hasPre = false;
};

ParsedVersion parseVersion(const QString &text)
{
    QString s = text.trimmed();
    if (s.startsWith(QLatin1Char('v')) || s.startsWith(QLatin1Char('V')))
        s.remove(0, 1);
    ParsedVersion out;
    const int dash = s.indexOf(QLatin1Char('-'));
    const QString coreText = dash < 0 ? s : s.left(dash);
    if (dash >= 0) {
        out.hasPre = true;
        out.pre = s.mid(dash + 1).split(QLatin1Char('.'), Qt::SkipEmptyParts);
    }
    const QStringList parts =
        coreText.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        bool ok = false;
        const long long n = p.toLongLong(&ok);
        if (!ok)
            break; // a core that stops being numeric stops being compared
        out.core << n;
    }
    while (out.core.size() < 3)
        out.core << 0; // "1.0" is "1.0.0"
    return out;
}

QString tempInstallerPath(const QString &assetName)
{
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir dir(base + QStringLiteral("/nevertube-update"));
    dir.mkpath(QStringLiteral("."));
    return dir.filePath(assetName);
}

} // namespace

QString appVersion()
{
#ifdef NEVERTUBE_VERSION
    return QStringLiteral(NEVERTUBE_VERSION);
#else
    return QStringLiteral("0.0.0");
#endif
}

int compareVersions(const QString &a, const QString &b)
{
    const ParsedVersion pa = parseVersion(a);
    const ParsedVersion pb = parseVersion(b);
    const int n = qMax(pa.core.size(), pb.core.size());
    for (int i = 0; i < n; ++i) {
        const long long x = i < pa.core.size() ? pa.core.at(i) : 0;
        const long long y = i < pb.core.size() ? pb.core.at(i) : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    // A pre-release is OLDER than its own core: "1.0.0-rc.1" < "1.0.0". This
    // is what makes the updater refuse to "update" a final version to a
    // pre-release of the same core, and an alpha to the final one.
    if (pa.hasPre != pb.hasPre)
        return pa.hasPre ? -1 : 1;
    if (!pa.hasPre)
        return 0;
    const int m = qMax(pa.pre.size(), pb.pre.size());
    for (int i = 0; i < m; ++i) {
        if (i >= pa.pre.size())
            return -1; // same prefix and shorter: the shorter one is older
        if (i >= pb.pre.size())
            return 1;
        const QString &x = pa.pre.at(i);
        const QString &y = pb.pre.at(i);
        bool okX = false;
        bool okY = false;
        const long long nx = x.toLongLong(&okX);
        const long long ny = y.toLongLong(&okY);
        if (okX && okY) {
            if (nx != ny)
                return nx < ny ? -1 : 1;
            continue;
        }
        // Numeric identifiers sort before alphanumeric ones ("1.0.0-2" <
        // "1.0.0-alpha"), as SemVer says.
        if (okX != okY)
            return okX ? -1 : 1;
        const int c = QString::compare(x, y, Qt::CaseInsensitive);
        if (c != 0)
            return c < 0 ? -1 : 1;
    }
    return 0;
}

bool isNewerVersion(const QString &candidate, const QString &installed)
{
    return compareVersions(candidate, installed) > 0;
}

QString versionFromTag(const QString &tag)
{
    QString s = tag.trimmed();
    if (s.startsWith(QLatin1Char('v')) || s.startsWith(QLatin1Char('V')))
        s.remove(0, 1);
    return s;
}

QString installerFlavorForBuild()
{
    // The Win7 x86 build is the one whose only engine is the FfmpegBackend
    // (NEVERTUBE_NO_MPV, set by CMakeLists.txt): its installer is the only one
    // that refuses Windows 10/11, so mixing them up would hand a Win7 machine
    // a build it cannot run (and the other way round).
#if defined(NEVERTUBE_NO_MPV)
    return QStringLiteral("win7-x86");
#elif defined(Q_OS_WIN) && !defined(_WIN64)
    return QStringLiteral("win10-x86");
#elif defined(Q_OS_WIN)
    return QStringLiteral("x64");
#else
    // Linux: there is no installer. The packages and the Flatpak update
    // through the package manager.
    return QString();
#endif
}

QString installerAssetNameFor(const QString &flavor, const QString &version)
{
    if (flavor.isEmpty() || version.isEmpty())
        return QString();
    return QStringLiteral("nevertube-%1-setup-%2.exe").arg(version, flavor);
}

QString installerAssetName(const QString &version)
{
    return installerAssetNameFor(installerFlavorForBuild(), version);
}

UpdateAsset UpdateRelease::installerAsset() const
{
    const QString wanted = installerAssetName(version);
    if (wanted.isEmpty())
        return UpdateAsset{};
    for (const UpdateAsset &a : assets)
        if (a.name == wanted)
            return a;
    return UpdateAsset{};
}

UpdateAsset UpdateRelease::sumsAsset() const
{
    for (const UpdateAsset &a : assets)
        if (a.name == QLatin1String("SHA256SUMS.txt"))
            return a;
    return UpdateAsset{};
}

QUrl UpdateChecker::apiUrl()
{
    return QUrl(QStringLiteral(
        "https://api.github.com/repos/marlo4220mc/NeverTube/releases/latest"));
}

UpdateChecker::UpdateChecker(QObject *parent) : QObject(parent) {}

void UpdateChecker::check()
{
    if (m_reply)
        return; // one query at a time
    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);

    QNetworkRequest req(apiUrl());
    req.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    req.setRawHeader(QByteArrayLiteral("Accept"),
                     QByteArrayLiteral("application/vnd.github+json"));
    // GitHub answers a redirect for some endpoints and a stalled TLS
    // handshake must not leave the query hanging forever.
    m_nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(20000);

    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::finished, this,
            &UpdateChecker::onReplyFinished);
}

void UpdateChecker::cancel()
{
    if (!m_reply)
        return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr; // onReplyFinished() will see it is not current any more
    reply->abort();
    reply->deleteLater();
}

void UpdateChecker::onReplyFinished()
{
    if (!m_reply)
        return; // cancelled
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();

    const QVariant statusAttr =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int status = statusAttr.isValid() ? statusAttr.toInt() : 0;

    // No HTTP status at all: DNS, TLS or connection failure.
    if (status == 0) {
        emit failed(nt::tr("update.err.network").arg(reply->errorString()));
        return;
    }
    // 404 on releases/latest is GitHub saying "this repository has no
    // published release": a state of the world, not an error.
    if (status == 404) {
        UpdateRelease none;
        none.found = false;
        emit finished(none);
        return;
    }
    if (status == 403 || status == 429) {
        const QByteArray remaining =
            reply->rawHeader(QByteArrayLiteral("x-ratelimit-remaining"));
        if (remaining == QByteArrayLiteral("0") || status == 429) {
            emit failed(nt::tr("update.err.rateLimit"));
            return;
        }
    }
    if (status != 200) {
        emit failed(nt::tr("update.err.http").arg(status));
        return;
    }
    QString error;
    const UpdateRelease release = parseReleaseJson(reply->readAll(), &error);
    if (!error.isEmpty()) {
        emit failed(error);
        return;
    }
    emit finished(release);
}

UpdateRelease parseReleaseJson(const QByteArray &json, QString *error)
{
    UpdateRelease out;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = nt::tr("update.err.json").arg(parseError.errorString());
        return out;
    }
    const QJsonObject o = doc.object();
    out.found = true;
    out.tag = o.value(QStringLiteral("tag_name")).toString();
    out.version = versionFromTag(out.tag);
    out.notes = o.value(QStringLiteral("body")).toString();
    out.publishedAt = o.value(QStringLiteral("published_at")).toString();
    out.prerelease = o.value(QStringLiteral("prerelease")).toBool();
    out.draft = o.value(QStringLiteral("draft")).toBool();
    const QJsonArray assets = o.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue &v : assets) {
        const QJsonObject a = v.toObject();
        UpdateAsset asset;
        asset.name = a.value(QStringLiteral("name")).toString();
        asset.url = QUrl(a.value(QStringLiteral("browser_download_url"))
                             .toString());
        asset.size = qint64(
            a.value(QStringLiteral("size")).toDouble());
        if (!asset.name.isEmpty() && asset.url.isValid())
            out.assets.append(asset);
    }
    return out;
}

bool checksumFor(const QByteArray &sumsText, const QString &fileName,
                 QString *sha256, QString *error)
{
    if (fileName.isEmpty()) {
        if (error)
            *error = nt::tr("update.err.sumsEntryMissing");
        return false;
    }

    int hits = 0;
    QString found;
    const QStringList lines =
        QString::fromUtf8(sumsText).split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        const int space = line.indexOf(QLatin1Char(' '));
        if (space <= 0)
            continue;
        const QString hash = line.left(space).trimmed();
        if (hash.size() != 64)
            continue; // not a SHA-256: comments and notes are not entries
        QString name = line.mid(space + 1).trimmed();
        // `sha256sum` writes a binary-mode entry as "*name".
        if (name.startsWith(QLatin1Char('*')))
            name.remove(0, 1);
        if (name != fileName)
            continue; // EXACT name: "…-setup-x64.exe.asc" is another file
        ++hits;
        found = hash.toLower();
    }
    if (hits == 0) {
        if (error)
            *error = nt::tr("update.err.sumsEntryMissing");
        return false;
    }
    if (hits > 1) {
        if (error)
            *error = nt::tr("update.err.sumsAmbiguous");
        return false;
    }
    if (sha256)
        *sha256 = found;
    return true;
}

QString sha256OfFile(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return QString();
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f)) {
        if (error)
            *error = f.errorString();
        return QString();
    }
    return QString::fromLatin1(hash.result().toHex());
}

UpdateDownloader::UpdateDownloader(QObject *parent) : QObject(parent) {}

UpdateDownloader::~UpdateDownloader() = default;

void UpdateDownloader::start(const UpdateRelease &release)
{
    if (m_reply)
        return;
    m_release = release;
    m_expectedSha.clear();
    m_filePath.clear();

    if (installerFlavorForBuild().isEmpty()) {
        emit failed(nt::tr("update.err.noInstaller"));
        return;
    }
    const UpdateAsset installer = m_release.installerAsset();
    if (installer.name.isEmpty()) {
        emit failed(nt::tr("update.err.assetMissing"));
        return;
    }
    if (!isGithubAssetUrl(installer.url)) {
        emit failed(nt::tr("update.err.assetMissing"));
        return;
    }
    const UpdateAsset sums = m_release.sumsAsset();
    if (sums.name.isEmpty() || !isGithubAssetUrl(sums.url)) {
        // Without the manifest there is nothing to verify the download
        // against, and an unverified installer is not offered.
        emit failed(nt::tr("update.err.sumsMissing"));
        return;
    }

    if (!m_nam)
        m_nam = new QNetworkAccessManager(this);
    m_nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    startSums();
}

void UpdateDownloader::startSums()
{
    m_stage = Sums;
    QNetworkRequest req(m_release.sumsAsset().url);
    // (the manifest is small: a few hundred bytes of text)
    req.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    req.setTransferTimeout(30000);
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::finished, this,
            &UpdateDownloader::finishReply);
}

void UpdateDownloader::startInstaller(const QString &expectedSha)
{
    m_stage = Installer;
    m_expectedSha = expectedSha;
    const UpdateAsset installer = m_release.installerAsset();
    m_filePath = tempInstallerPath(installer.name);

    // A leftover of a previous attempt is not trusted: it is overwritten from
    // scratch (never verified against the old file).
    QFile::remove(m_filePath);

    QNetworkRequest req(installer.url);
    req.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    // The installer is tens of MB: the timeout is per transfer, not a global
    // budget, and the progress arrives as the file does.
    req.setTransferTimeout(120000);
    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::readyRead, this, [this]() {
        if (!m_reply || m_stage != Installer)
            return;
        QFile f(m_filePath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
            const QString why = f.errorString();
            m_reply->abort();
            m_reply->deleteLater();
            m_reply = nullptr;
            m_stage = Idle;
            emit failed(nt::tr("update.err.write").arg(why));
            return;
        }
        f.write(m_reply->readAll());
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                if (m_reply && m_stage == Installer)
                    emit progress(received, total);
            });
    connect(m_reply, &QNetworkReply::finished, this,
            &UpdateDownloader::finishReply);
}

void UpdateDownloader::finishReply()
{
    if (!m_reply)
        return; // cancelled
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->deleteLater();

    if (m_stage == Sums) {
        const QVariant statusAttr =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int status = statusAttr.isValid() ? statusAttr.toInt() : 0;
        if (status != 200) {
            emit failed(status == 0
                            ? nt::tr("update.err.network")
                                  .arg(reply->errorString())
                            : nt::tr("update.err.http").arg(status));
            return;
        }
        QString expected;
        QString error;
        if (!checksumFor(reply->readAll(),
                         m_release.installerAsset().name, &expected, &error)) {
            emit failed(error);
            return;
        }
        startInstaller(expected);
        return;
    }

    // The installer itself.
    const QVariant statusAttr =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int status = statusAttr.isValid() ? statusAttr.toInt() : 0;
    // The tail of the body can arrive with `finished` and not with a
    // readyRead, so it is written here before the hash is computed. Without
    // this the file would be one chunk short and the comparison would fail
    // (safely, but saying the download was corrupt when it was not).
    if (reply->bytesAvailable() > 0) {
        QFile tail(m_filePath);
        if (tail.open(QIODevice::WriteOnly | QIODevice::Append))
            tail.write(reply->readAll());
    }
    if (status != 200 || reply->error() != QNetworkReply::NoError) {
        cleanupPartialFile();
        emit failed(status == 0
                        ? nt::tr("update.err.network").arg(reply->errorString())
                        : nt::tr("update.err.http").arg(status));
        return;
    }

    QString error;
    const QString actual = sha256OfFile(m_filePath, &error);
    if (actual.isEmpty()) {
        cleanupPartialFile();
        emit failed(nt::tr("update.err.write").arg(error));
        return;
    }
    // byte-exact against the manifest of the release. It proves the file
    // arrived whole and is the one the manifest describes; it does NOT prove
    // who published it (the installers are unsigned), and the dialog says so.
    if (actual.compare(m_expectedSha, Qt::CaseInsensitive) != 0) {
        cleanupPartialFile();
        emit failed(nt::tr("update.err.hashMismatch"));
        return;
    }
    m_stage = Idle;
    emit ready(m_filePath);
}

void UpdateDownloader::cancel()
{
    if (!m_reply) {
        // Cancelling something that is not running is a no-op, not an
        // error: the button may be pressed after the download already ended.
        return;
    }
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->abort();
    reply->deleteLater();
    cleanupPartialFile();
    m_stage = Idle;
    emit cancelled();
}

void UpdateDownloader::cleanupPartialFile()
{
    // Only OUR temp file: the path is compared against the directory the
    // class itself composes, never against anything that came from network
    // input.
    if (m_filePath.isEmpty())
        return;
    const QString ourDir = QFileInfo(tempInstallerPath(QStringLiteral("x")))
                               .absolutePath();
    if (QFileInfo(m_filePath).absolutePath() == ourDir)
        QFile::remove(m_filePath);
}

} // namespace nt
