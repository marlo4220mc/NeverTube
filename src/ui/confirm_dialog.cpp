#include <QEventLoop>
#include <QFrame>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QPointer>
#include <QShowEvent>
#include <QVBoxLayout>
#include <functional>

#include "confirm_dialog.h"

namespace {

class ConfirmOverlay : public QWidget
{
public:
    static void ask(QWidget *parent, const QString &title, const QString &text,
                    const QString &acceptText, std::function<void()> onAccept)
    {
        ConfirmOverlay ov(parent, title, text, acceptText, std::move(onAccept));
        ov.run();
    }

private:
    ConfirmOverlay(QWidget *page, const QString &title, const QString &text,
                   const QString &acceptText, std::function<void()> onAccept)
        : QWidget(page->window())
        , m_page(page)
        , m_onAccept(std::move(onAccept))
    {
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addStretch(1);

        auto *mid = new QHBoxLayout;
        mid->setContentsMargins(0, 0, 0, 0);
        mid->addStretch(1);

        auto *frame = new QFrame(this);
        frame->setObjectName(QStringLiteral("popupDialog"));
        frame->setFixedWidth(400);
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

        auto *body = new QLabel(text, frame);
        body->setObjectName(QStringLiteral("popupText"));
        body->setWordWrap(true);
        v->addWidget(body);

        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        row->addStretch(1);
        auto *cancel = new QPushButton(QStringLiteral("Cancelar"), frame);
        cancel->setObjectName(QStringLiteral("ghost"));
        cancel->setCursor(Qt::PointingHandCursor);
        connect(cancel, &QPushButton::clicked, this,
                [this]() { finish(false); });
        row->addWidget(cancel);
        auto *ok = new QPushButton(acceptText, frame);
        ok->setObjectName(QStringLiteral("primary"));
        ok->setCursor(Qt::PointingHandCursor);
        connect(ok, &QPushButton::clicked, this, [this]() { finish(true); });
        row->addWidget(ok);
        v->addLayout(row);
        m_cancel = cancel;

        connect(page, &QWidget::hide, this, [this]() { finish(false); });
        if (m_page)
            connect(m_page.data(), &QObject::destroyed, this,
                    [this]() { finish(false); });

        if (parentWidget())
            parentWidget()->installEventFilter(this);
    }

    void run()
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

        const auto cb = std::move(m_cbPending);
        if (m_accepted && cb)
            cb();
    }

    void finish(bool accepted)
    {
        if (m_loop && m_loop->isRunning()) {
            m_accepted = accepted;
            if (accepted)
                m_cbPending = std::move(m_onAccept);
            m_loop->quit();
            return;
        }
        const auto cb = std::move(m_onAccept);
        if (accepted && cb)
            cb();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            finish(false);
            return;
        }
        QWidget::keyPressEvent(event);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget()
            && (event->type() == QEvent::Resize
                || event->type() == QEvent::Move))
            setGeometry(parentWidget()->rect());
        return QWidget::eventFilter(watched, event);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0, 0, 0, 168));
    }

private:
    QEventLoop *m_loop = nullptr;
    QPushButton *m_cancel = nullptr;
    QPointer<QWidget> m_page;
    std::function<void()> m_onAccept;
    std::function<void()> m_cbPending;
    bool m_accepted = false;
};

class InputOverlay : public QWidget
{
public:
    static bool ask(QWidget *parent, const QString &title, const QString &prompt,
                    const QString &initialText, const QString &okText,
                    QString *outText)
    {
        InputOverlay ov(parent, title, prompt, initialText, okText);
        ov.run();
        if (ov.m_accepted && outText)
            *outText = ov.m_result.trimmed();
        return ov.m_accepted;
    }

private:
    InputOverlay(QWidget *page, const QString &title, const QString &prompt,
                 const QString &initialText, const QString &okText)
        : QWidget(page->window())
        , m_page(page)
    {
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addStretch(1);

        auto *mid = new QHBoxLayout;
        mid->setContentsMargins(0, 0, 0, 0);
        mid->addStretch(1);

        auto *frame = new QFrame(this);
        frame->setObjectName(QStringLiteral("popupDialog"));
        frame->setFixedWidth(340);
        mid->addWidget(frame);
        mid->addStretch(1);
        lay->addLayout(mid);
        lay->addStretch(1);

        auto *v = new QVBoxLayout(frame);
        v->setContentsMargins(18, 16, 18, 14);
        v->setSpacing(10);

        if (!title.isEmpty()) {
            auto *titleLabel = new QLabel(title, frame);
            titleLabel->setObjectName(QStringLiteral("popupTitle"));
            titleLabel->setWordWrap(true);
            v->addWidget(titleLabel);
        }
        if (!prompt.isEmpty()) {
            auto *p = new QLabel(prompt, frame);
            p->setObjectName(QStringLiteral("popupText"));
            p->setWordWrap(true);
            v->addWidget(p);
        }

        m_edit = new QLineEdit(frame);
        m_edit->setText(initialText);
        m_edit->setClearButtonEnabled(true);
        m_edit->installEventFilter(this);
        v->addWidget(m_edit);

        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        row->addStretch(1);
        auto *cancel = new QPushButton(QStringLiteral("Cancelar"), frame);
        cancel->setObjectName(QStringLiteral("ghost"));
        cancel->setCursor(Qt::PointingHandCursor);
        connect(cancel, &QPushButton::clicked, this, [this]() { finish(false); });
        row->addWidget(cancel);
        auto *ok = new QPushButton(okText.isEmpty() ? QStringLiteral("Aceptar") : okText, frame);
        ok->setObjectName(QStringLiteral("primary"));
        ok->setCursor(Qt::PointingHandCursor);
        connect(ok, &QPushButton::clicked, this, [this]() { finish(true); });
        row->addWidget(ok);
        v->addLayout(row);
        m_cancel = cancel;

        connect(page, &QWidget::hide, this, [this]() { finish(false); });
        if (m_page)
            connect(m_page.data(), &QObject::destroyed, this, [this]() { finish(false); });

        if (parentWidget())
            parentWidget()->installEventFilter(this);

        connect(m_edit, &QLineEdit::returnPressed, this, [this]() { finish(true); });
    }

    void run()
    {
        setGeometry(parentWidget()->rect());
        show();
        raise();
        m_edit->selectAll();
        m_edit->setFocus(Qt::OtherFocusReason);
        m_loop = new QEventLoop(this);
        m_loop->exec();
        delete m_loop;
        m_loop = nullptr;
        hide();
        deleteLater();
        if (m_accepted)
            m_result = m_edit->text();
    }

    void finish(bool accepted)
    {
        if (m_loop && m_loop->isRunning()) {
            m_accepted = accepted;
            m_loop->quit();
            return;
        }
        m_accepted = accepted;
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            finish(false);
            return;
        }
        QWidget::keyPressEvent(event);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget()
            && (event->type() == QEvent::Resize
                || event->type() == QEvent::Move))
            setGeometry(parentWidget()->rect());
        return QWidget::eventFilter(watched, event);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0, 0, 0, 168));
    }

private:
    QEventLoop *m_loop = nullptr;
    QPushButton *m_cancel = nullptr;
    QLineEdit *m_edit = nullptr;
    QPointer<QWidget> m_page;
    QString m_result;
    bool m_accepted = false;
};

} // namespace

namespace nt {

void confirm(QWidget *parent, const QString &title, const QString &text,
             const QString &acceptText, std::function<void()> onAccept)
{
    if (!parent) {
        if (onAccept)
            onAccept();
        return;
    }
    ConfirmOverlay::ask(parent, title, text, acceptText, std::move(onAccept));
}

bool inputText(QWidget *parent, const QString &title, const QString &prompt,
               const QString &initialText, const QString &okText,
               QString *outText)
{
    if (!parent)
        return false;
    return InputOverlay::ask(parent, title, prompt, initialText, okText, outText);
}

} // namespace nt
