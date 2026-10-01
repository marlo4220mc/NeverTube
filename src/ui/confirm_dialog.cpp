#include "confirm_dialog.h"

#include <QEventLoop>
#include <QFrame>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QPointer>
#include <QShowEvent>
#include <QVBoxLayout>

namespace {

// Confirmación SIN ventana propia: es un QWidget hijo, no un QDialog. Un QDialog
// es un toplevel, o sea un xdg_toplevel nuevo para el compositor: aparece en la
// barra, se le da el foco y con él se va el puntero del usuario. Eso está
// prohibido en esta app (regla del usuario: las ventanas flotantes secuestran el
// puntero), así que esto vive DENTRO de la ventana, igual que los otros dos
// popovers (el de "Nueva playlist" y el de "Gestionar suscripciones"), que son
// QFrame(this) + raise() + show().
//
// El bloqueo (lo que hacía setModal(true)) lo pone el propio overlay: cubre la
// página entera, así que el ratón no llega a nada de debajo, y el bucle de
// eventos local (exec()) impide volver a la app hasta que se decida.
class ConfirmOverlay : public QWidget
{
public:
    static void ask(QWidget *parent, const QString &title, const QString &text,
                    const QString &acceptText, std::function<void()> onAccept)
    {
        ConfirmOverlay ov(parent, title, text, acceptText,
                          std::move(onAccept));
        ov.run();
    }

private:
    ConfirmOverlay(QWidget *page, const QString &title, const QString &text,
                   const QString &acceptText, std::function<void()> onAccept)
        : QWidget(page->window())
        , m_page(page)
        , m_onAccept(std::move(onAccept))
    {
        // El overlay es hijo de la VENTANA, no de la página: tiene que tapar
        // también la barra de navegación inferior, que si no queda viva detrás
        // del velo y el usuario puede cambiar de pestaña con la confirmación
        // abierta. Pero se sigue al widget que la pidió para cerrarse si esa
        // página se oculta (cambio de pestaña), que es el caso que el overlay ya
        // no puede resolver solo por ser hermano.
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->addStretch(1);

        auto *mid = new QHBoxLayout;
        mid->setContentsMargins(0, 0, 0, 0);
        mid->addStretch(1);

        auto *frame = new QFrame(this);
        frame->setObjectName(QStringLiteral("popupDialog"));
        // El ancho de los otros dos popups de la app (340 en Biblioteca, 420 en
        // Ajustes); aquí el texto de confirmación es una frase larga.
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

        // Si la página que la pidió se oculta (cambio de pestaña con la barra de
// navegación) o se destruye, se cierra el overlay: si no, se quedaría
// mostrado sobre otra página al volver.
        connect(page, &QWidget::hide, this, [this]() { finish(false); });
        if (m_page)
            connect(m_page.data(), &QObject::destroyed, this,
                    [this]() { finish(false); });

        // El overlay no está en ningún layout de la ventana, así que no la sigue
        // al redimensionar: sin esto el velo se quedaría en el tamaño viejo y
        // tapando la página equivocada.
        parentWidget()->installEventFilter(this);
    }

    void run()
    {
        setGeometry(parentWidget()->rect());
        show();
        raise();
        // Se enfoca CANCELAR y no hay botón por defecto, así que ni Enter ni un
        // Enter perdido borran nada: Enter sobre el botón enfocado es Cancelar.
        m_cancel->setFocus(Qt::OtherFocusReason);

        m_loop = new QEventLoop(this);
        m_loop->exec();
        delete m_loop;
        m_loop = nullptr;

        // El overlay se retira ANTES del callback: si la acción borra la fila que
        // estaba detrás, el overlay ya no está en medio.
        hide();
        deleteLater();

        // Y se guarda solo si se aceptó (finish mueve el callback a m_cbPending).
        const auto cb = std::move(m_cbPending);
        if (m_accepted && cb)
            cb();
    }

    // Solo marca el resultado y sale del bucle: el callback se ejecuta en run(),
    // con el overlay ya oculto. Si el bucle no está corriendo (o sea, ya se
    // decidió), se ejecuta aquí directamente.
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

    // Escape cierra, como en un QDialog. Se captura en keyPressEvent y no en un
    // eventFilter porque el foco está en el botón Cancelar: la tecla no la
    // consume y le llega aquí por la cadena de propagación de Qt.
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
        // La ventana cambia de tamaño (o se mueve): el velo se reacomoda. El
        // overlay no está en ningún layout, así que no la seguiría solo.
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

} // namespace

namespace nt {

void confirm(QWidget *parent, const QString &title, const QString &text,
             const QString &acceptText, std::function<void()> onAccept)
{
    if (!parent) {
        // Sin ventana que tapar no hay nada que confirmar: se ejecuta igual, para
        // no dejar la acción a medias por un padre nulo.
        if (onAccept)
            onAccept();
        return;
    }
    ConfirmOverlay::ask(parent, title, text, acceptText, std::move(onAccept));
}

} // namespace nt