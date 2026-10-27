#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

// The built-in updater, in three pieces so that every rule can be tested
// OFFLINE (--selftest-update):
//
//   1. the version of the binary and the ORDER of two versions
//      (appVersion / compareVersions): pure functions;
//   2. the GitHub query and the selection of the installer that belongs to
//      THIS build (UpdateChecker / installerAssetName): no UI;
//   3. the download with the verification against SHA256SUMS.txt
//      (UpdateDownloader): sums first, installer second.
//
// What the updater does NOT do: it does not claim the download is authentic.
// The installers are unsigned (there is no Authenticode certificate yet), so
// SHA-256 proves the file arrived whole and matches the manifest of the
// release, not who built it. That is what the dialog says, and
// verifyDownloadedFile() is the single seam where a signature check is added
// when there is a certificate to check against.
namespace nt {

// The version of this binary. A release build is configured with
// -DNEVERTUBE_VERSION=<tag> (so "1.0.0-alpha.1" reaches the About box and the
// updater with its suffix); without it, the PROJECT_VERSION of CMakeLists.txt.
QString appVersion();

// Order of two versions: -1 if a is older, 0 if equal, 1 if newer. Accepts the
// forms the project uses: "1.0.0", "1.0.0-alpha.1", a leading "v" and a
// missing patch ("1.0"). The comparison is SemVer's: the numeric core first,
// and then a version WITH a pre-release suffix is OLDER than the same core
// without one ("1.0.0-rc.1" < "1.0.0"), which is what keeps a pre-release from
// being offered as an update of the final one and the other way round.
int compareVersions(const QString &a, const QString &b);
bool isNewerVersion(const QString &candidate, const QString &installed);

// "1.0.0" out of a tag ("v1.0.0", "v1.0.0-alpha.1"); the tag itself when it
// has no leading v.
QString versionFromTag(const QString &tag);

// The installer of THIS build: "x64", "win10-x86" or "win7-x86". Empty when
// the platform has no installer (Linux: the packages update through the
// package manager). The three Windows builds are told apart at COMPILE time,
// which is the only honest source: the Win7 x86 build is the one without mpv
// (NEVERTUBE_NO_MPV, its only engine is the FfmpegBackend) and the 32-bit
// build with mpv is the Windows 10 x86 one.
QString installerFlavorForBuild();
// The asset name of a flavor, spelled so a person scanning the release page
// can pick their file without knowing what "x86" means:
// "NeverTube-<version>-Windows-10-11-x64-Setup.exe", "...-Windows-10-11-x86-
// Setup.exe" or "...-Windows-7-SP1-x86-Setup.exe". Kept as a function of the
// flavor (not of the build) so the three names are testable from any platform.
// A flavor that is not one of the three has NO name: a typo must not produce
// an asset that does not exist.
QString installerAssetNameFor(const QString &flavor, const QString &version);
// The spelling the releases carried BEFORE this convention
// ("nevertube-<version>-setup-<flavor>.exe"). The selection still accepts it,
// so a release published under the old names keeps installing; the checksum is
// always looked up under the name that was actually selected.
QString legacyInstallerAssetNameFor(const QString &flavor,
                                    const QString &version);
// Same as installerAssetNameFor for the build this binary is: empty when there
// is no installer.
QString installerAssetName(const QString &version);

struct UpdateAsset
{
    QString name;
    QUrl url;
    qint64 size = 0;
};

struct UpdateRelease
{
    // false when GitHub answered 404: the project has no published release
    // yet, which is not an error and must not look like one.
    bool found = false;
    QString tag;
    QString version;
    QString notes;
    QString publishedAt;
    bool prerelease = false;
    bool draft = false;
    QList<UpdateAsset> assets;

    // The installer of a GIVEN flavor, by EXACT name: the current convention
    // first and the legacy spelling as a fallback. Empty when the release
    // carries neither (or the flavor is unknown): the selection never guesses,
    // and the checksum is looked up under the name that was selected.
    UpdateAsset installerAssetFor(const QString &flavor) const;
    // The installer of this build: installerAssetFor() with the flavor of the
    // binary (empty on Linux, where the packages update themselves).
    UpdateAsset installerAsset() const;
    UpdateAsset sumsAsset() const;
};

// Async query of releases/latest. It never blocks the interface and never
// polls: the caller decides when to ask (the automatic check of MainWindow is
// throttled with the "update_last_check" setting). "releases/latest" is
// GitHub's own "newest non-prerelease, non-draft release", which is what makes
// a pre-release impossible to offer as a stable update; the extra checks in
// parseRelease() are the belt and braces.
class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    explicit UpdateChecker(QObject *parent = nullptr);

    void check();
    void cancel();
    bool isRunning() const { return m_reply != nullptr; }

    // The URL of the API: fixed, and the single place where it lives.
    static QUrl apiUrl();

signals:
    // A complete answer (found = false means "no release published yet").
    void finished(const nt::UpdateRelease &release);
    // Network, TLS, HTTP or JSON error, already worded for the user.
    void failed(const QString &message);

private:
    void onReplyFinished();

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;
};

// Parses a GitHub release JSON. Separated from the query so the shape of the
// answer is testable without network.
UpdateRelease parseReleaseJson(const QByteArray &json, QString *error);

// SHA256SUMS.txt: the hash of `fileName`, looked up by EXACT name. Returns
// false (with the reason in *error) when the entry is missing or the manifest
// names the file more than once: an ambiguous manifest is not a hit, it is a
// reason to stop.
bool checksumFor(const QByteArray &sumsText, const QString &fileName,
                 QString *sha256, QString *error);

// SHA-256 of a file (lowercase hex); empty with *error set when it cannot be
// read.
QString sha256OfFile(const QString &path, QString *error);

// The ONE decision that a downloaded file is fit to run, so that a future
// signature check has a single place to go: today it is the SHA-256 against the
// manifest of the release, and when the installers are signed (they are not:
// the project has no certificate) the signature is verified HERE, next to this
// one, and nothing else in the download path changes.
bool verifyDownloadedFile(const QString &path, const QString &expectedSha256,
                          QString *error);

// The download of the installer, with the verification against the release's
// SHA256SUMS.txt. The sums travel FIRST (they are a few hundred bytes): a
// release without an entry for our installer stops before downloading 100 MB.
class UpdateDownloader : public QObject
{
    Q_OBJECT

public:
    explicit UpdateDownloader(QObject *parent = nullptr);
    ~UpdateDownloader() override;

    // `release` comes from the checker; the installer and the manifest are
    // picked by name from its assets.
    void start(const UpdateRelease &release);
    void cancel();
    bool isRunning() const { return m_reply != nullptr; }

    QString filePath() const { return m_filePath; }

signals:
    void progress(qint64 received, qint64 total);
    // The file is on the disk AND its SHA-256 matches the manifest.
    void ready(const QString &installerPath);
    void failed(const QString &message);
    void cancelled();

private:
    enum Stage { Idle, Sums, Installer };

    void startSums();
    void startInstaller(const QString &expectedSha);
    void finishReply();
    void cleanupPartialFile();

    QNetworkAccessManager *m_nam = nullptr;
    QNetworkReply *m_reply = nullptr;
    Stage m_stage = Idle;
    UpdateRelease m_release;
    QString m_expectedSha;
    QString m_filePath; // the installer being downloaded
};

} // namespace nt
