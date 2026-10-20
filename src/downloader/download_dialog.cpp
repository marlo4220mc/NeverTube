#include "download_dialog.h"

#include "segment_plan.h"

#include "../i18n.h"

#include <QComboBox>
#include <QEventLoop>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QShowEvent>
#include <QVBoxLayout>

namespace dl {

namespace {

// The bytes in a form a person can read.
QString humanBytes(qint64 bytes)
{
    if (bytes <= 0)
        return QString();
    if (bytes < 1024 * 1024)
        return QString::number(bytes / 1024) + QStringLiteral(" KB");
    if (bytes < 1024LL * 1024 * 1024)
        return QString::number(double(bytes) / (1024 * 1024), 'f', 1)
               + QStringLiteral(" MB");
    return QString::number(double(bytes) / (1024.0 * 1024.0 * 1024.0), 'f', 2)
           + QStringLiteral(" GB");
}

class DownloadOverlay : public QWidget
{
public:
    static bool ask(QWidget *parent, const QVector<VideoOption> &options,
                    const QString &title, const QString &durationLabel,
                    DownloadChoice *choice)
    {
        DownloadOverlay ov(parent, options, title, durationLabel, choice);
        return ov.run();
    }

private:
    DownloadOverlay(QWidget *page, const QVector<VideoOption> &options,
                    const QString &title, const QString &durationLabel,
                    DownloadChoice *choice)
        : QWidget(page->window())
        , m_page(page)
        , m_choice(choice)
    {
        setObjectName(QStringLiteral("downloadOverlay"));
        // The overlay takes the pointer: without it the click would go to
        // the page below.
        setAttribute(Qt::WA_TransparentForMouseEvents, false);

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addStretch(1);

        auto *mid = new QHBoxLayout;
        mid->setContentsMargins(0, 0, 0, 0);
        mid->addStretch(1);

        auto *frame = new QFrame(this);
        frame->setObjectName(QStringLiteral("popupDialog"));
        frame->setFixedWidth(420);
        mid->addWidget(frame);
        mid->addStretch(1);
        lay->addLayout(mid);
        lay->addStretch(1);

        auto *v = new QVBoxLayout(frame);
        v->setContentsMargins(18, 16, 18, 14);
        v->setSpacing(10);

        auto *titleLabel = new QLabel(title, frame);
        titleLabel->setObjectName(QStringLiteral("popupTitle"));
        titleLabel->setWordWrap(true);
        v->addWidget(titleLabel);

        if (!durationLabel.isEmpty()) {
            auto *dur = new QLabel(durationLabel, frame);
            dur->setObjectName(QStringLiteral("popupText"));
            dur->setWordWrap(true);
            v->addWidget(dur);
        }

        // --- what to download ---
        auto *videoRadio = new QRadioButton(nt::tr("downloads.kindVideo"), frame);
        videoRadio->setObjectName(QStringLiteral("downloadKindVideo"));
        v->addWidget(videoRadio);

        m_audioRadio = new QRadioButton(nt::tr("downloads.kindAudio"), frame);
        m_audioRadio->setObjectName(QStringLiteral("downloadKindAudio"));
        v->addWidget(m_audioRadio);

        // --- the resolution ---
        m_height = new QComboBox(frame);
        m_height->setObjectName(QStringLiteral("downloadHeight"));
        for (const VideoOption &o : options) {
            QString label = QString::number(o.height) + QLatin1Char('p');
            if (o.contentLength > 0)
                label += QStringLiteral(" · ") + humanBytes(o.contentLength);
            m_height->addItem(label, o.height);
        }
        if (m_height->count() == 0) {
            m_height->addItem(nt::tr("downloads.bestQuality"), 0);
        }
        v->addWidget(m_height);

        // The mp3 needs ffmpeg (it is a conversion). The label says so when
        // it is not there: the option is not hidden, it is explained.
        m_audioNote = new QLabel(frame);
        m_audioNote->setObjectName(QStringLiteral("popupText"));
        m_audioNote->setWordWrap(true);
        v->addWidget(m_audioNote);

        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        row->addStretch(1);
        auto *cancel = new QPushButton(nt::tr("common.cancel"), frame);
        cancel->setObjectName(QStringLiteral("ghost"));
        cancel->setCursor(Qt::PointingHandCursor);
        connect(cancel, &QPushButton::clicked, this, [this]() { finish(false); });
        row->addWidget(cancel);
        auto *ok = new QPushButton(nt::tr("common.ok"), frame);
        ok->setObjectName(QStringLiteral("primary"));
        ok->setCursor(Qt::PointingHandCursor);
        connect(ok, &QPushButton::clicked, this, [this]() { finish(true); });
        row->addWidget(ok);
        v->addLayout(row);
        m_cancel = cancel;

        videoRadio->setChecked(true);
        updateAudioNote();

        connect(videoRadio, &QRadioButton::toggled, this,
                [this](bool on) {
                    if (on) {
                        m_height->setEnabled(true);
                        updateAudioNote();
                    }
                });
        connect(m_audioRadio, &QRadioButton::toggled, this,
                [this](bool on) {
                    if (on) {
                        m_height->setEnabled(false);
                        updateAudioNote();
                    }
                });

        connect(page, &QWidget::hide, this, [this]() { finish(false); });
        if (m_page)
            connect(m_page.data(), &QObject::destroyed, this,
                    [this]() { finish(false); });
    }

    void updateAudioNote()
    {
        if (!m_audioNote)
            return;
        if (m_audioRadio->isChecked()) {
            m_audioNote->setText(nt::tr("downloads.audioNeedsFfmpeg"));
            m_audioNote->setVisible(true);
        } else {
            m_audioNote->setVisible(false);
        }
    }

    bool run()
    {
        setGeometry(parentWidget()->rect());
        show();
        raise();
        m_cancel->setFocus(Qt::OtherFocusReason);

        m_loop = new QEventLoop(this);
        m_loop->exec();
        delete m_loop;
        m_loop = nullptr;

        hide();
        deleteLater();
        return m_accepted;
    }

    void finish(bool accepted)
    {
        if (m_loop && m_loop->isRunning()) {
            m_accepted = accepted;
            if (accepted && m_choice) {
                m_choice->height = m_height->currentData().toInt();
                m_choice->audioOnly = m_audioRadio->isChecked();
            }
            m_loop->quit();
        }
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            finish(false);
            return;
        }
        QWidget::keyPressEvent(event);
    }

    QPointer<QWidget> m_page;
    QEventLoop *m_loop = nullptr;
    bool m_accepted = false;
    DownloadChoice *m_choice = nullptr;
    QComboBox *m_height = nullptr;
    QRadioButton *m_audioRadio = nullptr;
    QLabel *m_audioNote = nullptr;
    QPushButton *m_cancel = nullptr;
};

} // namespace

bool askDownloadOptions(QWidget *parent, const QVector<VideoOption> &options,
                        const QString &title, const QString &durationLabel,
                        DownloadChoice *choice)
{
    return DownloadOverlay::ask(parent, options, title, durationLabel, choice);
}

} // namespace dl
