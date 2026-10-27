#include "update_dialog.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QTextDocument>
#include <QVBoxLayout>

#include "../i18n.h"
#include "../ui/confirm_dialog.h"

namespace nt {

namespace {

// "12.4 MB" for the progress line; the exact bytes are not interesting while
// a download advances.
QString humanSize(qint64 bytes)
{
    if (bytes <= 0)
        return QStringLiteral("?");
    const double mb = double(bytes) / (1024.0 * 1024.0);
    if (mb < 1.0)
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
}

} // namespace

UpdateDialog::UpdateDialog(const UpdateRelease &release,
                           const QString &installedVersion, QWidget *parent)
    : QDialog(parent), m_release(release), m_installed(installedVersion)
{
    m_hasUpdate = release.found
        && !release.prerelease && !release.draft
        && isNewerVersion(release.version, m_installed);
    m_canInstall = m_hasUpdate && !installerFlavorForBuild().isEmpty();
    buildUI();
    refreshButtons();
}

void UpdateDialog::buildUI()
{
    setObjectName(QStringLiteral("updateDialog"));
    setWindowTitle(nt::tr("update.title"));
    resize(640, 460);
    setSizeGripEnabled(true);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(18, 16, 18, 16);
    lay->setSpacing(10);

    auto *title = new QLabel(nt::tr("update.title"), this);
    title->setObjectName(QStringLiteral("popupTitle"));
    lay->addWidget(title);

    auto *versions = new QGridLayout;
    versions->setHorizontalSpacing(12);
    versions->setVerticalSpacing(4);
    auto *installedLbl = new QLabel(nt::tr("update.installed"), this);
    auto *installedVal = new QLabel(
        m_installed.isEmpty() ? QStringLiteral("-") : m_installed, this);
    installedVal->setObjectName(QStringLiteral("updateValue"));
    installedVal->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *availableLbl = new QLabel(nt::tr("update.available"), this);
    auto *availableVal = new QLabel(
        m_release.found ? m_release.version : QStringLiteral("-"), this);
    availableVal->setObjectName(QStringLiteral("updateValue"));
    availableVal->setTextInteractionFlags(Qt::TextSelectableByMouse);
    versions->addWidget(installedLbl, 0, 0);
    versions->addWidget(installedVal, 0, 1);
    versions->addWidget(availableLbl, 1, 0);
    versions->addWidget(availableVal, 1, 1);
    versions->setColumnStretch(1, 1);
    lay->addLayout(versions);

    auto *notesTitle = new QLabel(nt::tr("update.notesTitle"), this);
    notesTitle->setObjectName(QStringLiteral("sectionSub"));
    lay->addWidget(notesTitle);

    // The body of the release is Markdown (what the GitHub page shows): it is
    // converted to HTML so the dialog shows formatted notes instead of the
    // raw text with its asterisks. A QLabel with rich text is used on purpose:
    // it inherits the app's text color from the QSS, which a QTextBrowser
    // would not (its document paints black over our dark theme).
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("updateNotesScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_notes = new QLabel(scroll);
    m_notes->setObjectName(QStringLiteral("updateNotes"));
    m_notes->setWordWrap(true);
    m_notes->setTextFormat(Qt::RichText);
    m_notes->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_notes->setTextInteractionFlags(Qt::TextSelectableByMouse
                                     | Qt::LinksAccessibleByMouse);
    m_notes->setOpenExternalLinks(true);
    {
        QTextDocument doc;
        doc.setMarkdown(m_release.notes);
        const QString html = doc.toHtml();
        // Without notes there is nothing to read: the dialog says there are
        // none instead of showing an empty box.
        m_notes->setText(m_release.notes.trimmed().isEmpty()
                             ? nt::tr("update.noNotes")
                             : html);
    }
    scroll->setWidget(m_notes);
    lay->addWidget(scroll, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("updateStatus"));
    m_status->setWordWrap(true);
    lay->addWidget(m_status);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    m_progress->hide();
    lay->addWidget(m_progress);

    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    row->addStretch(1);
    m_secondary = new QPushButton(this);
    m_secondary->setObjectName(QStringLiteral("ghost"));
    m_secondary->setCursor(Qt::PointingHandCursor);
    connect(m_secondary, &QPushButton::clicked, this, [this]() {
        if (m_downloader && m_downloader->isRunning()) {
            m_downloader->cancel();
            return;
        }
        if (m_hasUpdate && m_release.found)
            emit postponed(m_release.version);
        reject();
    });
    row->addWidget(m_secondary);
    m_primary = new QPushButton(this);
    m_primary->setObjectName(QStringLiteral("primary"));
    m_primary->setCursor(Qt::PointingHandCursor);
    connect(m_primary, &QPushButton::clicked, this, [this]() {
        if (!m_verifiedPath.isEmpty()) {
            confirmInstall(m_verifiedPath);
            return;
        }
        startDownload();
    });
    row->addWidget(m_primary);
    lay->addLayout(row);
}

void UpdateDialog::refreshButtons()
{
    const bool downloading = m_downloader && m_downloader->isRunning();

    if (!m_release.found) {
        m_status->setText(nt::tr("update.noRelease"));
        m_primary->hide();
        m_secondary->setText(nt::tr("common.close"));
        return;
    }
    if (!m_hasUpdate) {
        m_status->setText(nt::tr("update.upToDate"));
        m_primary->hide();
        m_secondary->setText(nt::tr("common.close"));
        return;
    }
    if (!m_verifiedPath.isEmpty()) {
        m_primary->show();
        m_primary->setEnabled(true);
        m_primary->setText(nt::tr("update.install"));
        m_secondary->setText(nt::tr("update.later"));
        return;
    }
    if (downloading) {
        m_primary->hide();
        m_secondary->setText(nt::tr("update.cancelDownload"));
        return;
    }
    if (!m_canInstall) {
        // Linux: the packages and the Flatpak update through the package
        // manager, so there is no installer to run from here. The dialog still
        // shows the available version and its notes.
        m_status->setText(nt::tr("update.linuxNote"));
        m_primary->hide();
        m_secondary->setText(nt::tr("common.close"));
        return;
    }
    m_primary->show();
    m_primary->setEnabled(true);
    m_primary->setText(nt::tr("update.now"));
    m_secondary->setText(nt::tr("update.later"));
}

void UpdateDialog::startDownload()
{
    if (m_downloader && m_downloader->isRunning())
        return;
    if (!m_downloader) {
        m_downloader = new UpdateDownloader(this);
        connect(m_downloader, &UpdateDownloader::progress, this,
                &UpdateDialog::onProgress);
        connect(m_downloader, &UpdateDownloader::ready, this,
                &UpdateDialog::onReady);
        connect(m_downloader, &UpdateDownloader::failed, this,
                &UpdateDialog::onFailed);
        connect(m_downloader, &UpdateDownloader::cancelled, this,
                &UpdateDialog::onCancelled);
    }
    m_verifiedPath.clear();
    m_progress->setValue(0);
    m_progress->show();
    m_status->setText(nt::tr("update.downloading")
                          .arg(QStringLiteral("?"), QStringLiteral("?")));
    refreshButtons();
    m_downloader->start(m_release);
}

void UpdateDialog::onProgress(qint64 received, qint64 total)
{
    if (total > 0)
        m_progress->setValue(int(received * 100 / total));
    m_status->setText(nt::tr("update.downloading")
                          .arg(humanSize(received), humanSize(total)));
}

void UpdateDialog::onReady(const QString &path)
{
    m_verifiedPath = path;
    m_progress->setValue(100);
    m_progress->hide();
    // What is said here is what was checked: the file matches the release's
    // SHA256SUMS.txt (it arrived whole and is the file that manifest
    // describes). It does NOT say who published it, and the second line says
    // so in the same breath: the installers are unsigned, and a hash does not
    // authenticate the publisher.
    m_status->setText(nt::tr("update.ready") + QLatin1Char('\n')
                      + nt::tr("update.hashNote"));
    refreshButtons();
}

void UpdateDialog::onFailed(const QString &message)
{
    m_progress->hide();
    m_status->setText(message);
    refreshButtons();
}

void UpdateDialog::onCancelled()
{
    m_progress->hide();
    m_status->setText(nt::tr("update.cancelled"));
    refreshButtons();
}

void UpdateDialog::confirmInstall(const QString &path)
{
    nt::confirm(
        this, nt::tr("update.title"), nt::tr("update.installNote"),
        nt::tr("update.install"),
        [this, path]() {
            emit installRequested(path);
            accept();
        });
}

} // namespace nt
