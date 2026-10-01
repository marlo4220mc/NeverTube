#include "confirm_dialog.h"

#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>

namespace {

// Sin marco: el borde y el radio los pone el QFrame#popupDialog de dentro. Sin
// WA_TranslucentBackground un QDialog sin regla de QSS pinta el fondo de su
// paleta y las esquinas del marco se ven cuadradas.
class ConfirmDialog : public QDialog
{
public:
    static void ask(QWidget *parent, const QString &title, const QString &text,
                    const QString &acceptText, std::function<void()> onAccept)
    {
        ConfirmDialog dlg(parent, title, text, acceptText, std::move(onAccept));
        dlg.exec();
    }

private:
    ConfirmDialog(QWidget *parent, const QString &title, const QString &text,
                  const QString &acceptText, std::function<void()> onAccept)
        : QDialog(parent)
        , m_onAccept(std::move(onAccept))
    {
        setWindowTitle(title);
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setModal(true);
        // El ancho de los otros dos popups de la app (340 en Biblioteca, 380 en
        // Ajustes); aquí el texto de confirmación es una frase larga.
        setFixedWidth(400);

        // Márgenes 0 en el diálogo: el marco lo rellena entero, y dentro van los
        // de verdad (18/16/18/14, los de los popups existentes).
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);

        auto *frame = new QFrame(this);
        frame->setObjectName(QStringLiteral("popupDialog"));
        outer->addWidget(frame);

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
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        row->addWidget(cancel);
        auto *ok = new QPushButton(acceptText, frame);
        ok->setObjectName(QStringLiteral("primary"));
        ok->setCursor(Qt::PointingHandCursor);
        connect(ok, &QPushButton::clicked, this, [this]() {
            // Se guarda la acción y se cierra primero: si el callback borra la
            // fila que está detrás, el diálogo ya no está en medio.
            const auto cb = std::move(m_onAccept);
            accept();
            if (cb)
                cb();
        });
        row->addWidget(ok);
        v->addLayout(row);

        cancel->setFocus(Qt::OtherFocusReason);
    }

    std::function<void()> m_onAccept;
};

} // namespace

namespace nt {

void confirm(QWidget *parent, const QString &title, const QString &text,
             const QString &acceptText, std::function<void()> onAccept)
{
    ConfirmDialog::ask(parent, title, text, acceptText, std::move(onAccept));
}

} // namespace nt