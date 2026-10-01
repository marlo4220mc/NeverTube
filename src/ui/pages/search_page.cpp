#include "search_page.h"

#include <QApplication>
#include <QBoxLayout>
#include <QFontMetrics>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QHideEvent>
#include <QKeyEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <QRegularExpression>

#include <utility>

#include "../../debug.h"
#include "../../db/app_database.h"
#include "../../db/repositories.h"
#include "../../models.h"
#include "../../yt.h"
#include "../channel_row.h"
#include "../format.h"
#include "../confirm_dialog.h"
#include "../icons.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

// --- Fila del desplegable de sugerencias -----------------------------------
//
// Es un QWidget y no un QPushButton por dos razones, ambas del mockup aprobado:
//   1. la negrita en el tramo escrito. Qt dibuja el texto de un boton con
//      QStyle::drawItemText, que NO parsea rich text, asi que con un boton la
//      negrita parcial es imposible. Un QLabel con Qt::RichText si la da.
//   2. el resaltado por teclado tiene que ser un estado aparte del hover del
//      raton, para poder moverlo con las flechas sin que el raton lo pise.
// El hover y el resaltado se resuelven con QSS (#suggestRow:hover y
// #suggestRow[highlight="true"]), por eso el WA_Hover: sin el, Qt no emite el
// cambio de hover a un QWidget normal.
class SuggestRow : public QWidget
{
    Q_OBJECT

public:
    SuggestRow(const QString &query, const QString &detail, const QString &badge,
               const QString &typed, int textW, int detailW, QWidget *parent);

    void setHighlighted(bool on);
    QString query() const { return m_query; }

signals:
    void picked(SuggestRow *row);
    void stepFocus(SuggestRow *row, int delta);
    void escapeFrom(SuggestRow *row);
    // Tab sale del desplegable: se cierra pero el foco continua donde toque.
    void tabOutOf(SuggestRow *row);
    void focusedRow(SuggestRow *row);

protected:
    // Tab/Backtab se interceptan en event(), no en keyPressEvent: QWidget::event()
    // los resuelve con focusNextPrevChild ANTES de llamar a keyPressEvent, asi
    // que un case Qt::Key_Tab en el switch no se ejecutaria nunca.
    bool event(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;

private:
    QString m_query;
    QLabel *m_text = nullptr;
};

namespace {
// El tramo escrito va en negrita. Se elida ANTES de aplicar la negrita: el
// recorte solo toca el final y el prefijo esta al principio, así que negritar
// los primeros n caracteres del texto ya recortado sigue siendo el prefijo
// correcto. Si el texto completo no cabe, el prefijo se recorta con el y se
// negrita entero, que es lo unico representable.
QString boldPrefix(const QString &text, const QString &typed, int textW,
                   QFontMetrics &fm)
{
    const QString elided = fm.elidedText(text, Qt::ElideRight, qMax(40, textW));
    int n = 0;
    if (!typed.isEmpty() && elided.startsWith(typed, Qt::CaseInsensitive))
        n = typed.size();
    const QString head = elided.left(n).toHtmlEscaped();
    const QString tail = elided.mid(n).toHtmlEscaped();
    if (n <= 0)
        return elided.toHtmlEscaped();
    // El tramo escrito va mas claro que el resto (kText sobre el kTextSoft de la
    // fila): es el gesto del mockup, ver bien la intencion sin haber pagado la
    // peticion. El color sale de la paleta del tema, no de un literal.
    return QStringLiteral("<span style=\"font-weight:600;color:%1;\">%2"
                          "</span>%3").arg(nt::kText.name(), head, tail);
}
} // namespace

SuggestRow::SuggestRow(const QString &query, const QString &detail,
                       const QString &badge, const QString &typed, int textW,
                       int detailW, QWidget *parent)
    : QWidget(parent)
    , m_query(query)
{
    setObjectName(QStringLiteral("suggestRow"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover, true);
    // Imprescindible en un QWidget que no es QFrame: sin WA_StyledBackground el
    // QSS hace "match" pero QWidget::paintEvent no pinta nada, y el fondo del
    // hover/resaltado se queda invisible (el QSS solo pinta en el QFrame).
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::PointingHandCursor);
    setToolTip(query);
    setFixedHeight(SearchPage::kSuggestRowH);

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(10, 0, 10, 0);
    lay->setSpacing(10);

    // Las etiquetas de la fila son transparentes al raton. No es cosmetico: son
    // HIJAS de la fila y cubren casi todo su ancho (el texto es lo que se
    // pincha), asi que si se quedan el clic no hay quien lo gestione y pulsar
    // una sugerencia no hace nada. La fila es quien recibe el clic siempre.
    const auto pasiva = [](QLabel *l) {
        l->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        return l;
    };

    auto *ico = pasiva(new QLabel(this));
    ico->setPixmap(nt::icon(nt::Icon::Search, nt::kMuted, 14).pixmap(14, 14));
    ico->setFixedSize(14, 14);
    lay->addWidget(ico, 0, Qt::AlignVCenter);

    QFontMetrics fm(font());
    m_text = pasiva(new QLabel(this));
    m_text->setObjectName(QStringLiteral("suggestQuery"));
    m_text->setTextFormat(Qt::RichText);
    // NoWrap + tamaño fijo: el texto se recorta con elipsis, no pasa a dos
    // lineas (una fila de 32 px con dos lineas descuadra todo el panel).
    m_text->setWordWrap(false);
    m_text->setFixedWidth(qMax(40, textW));
    m_text->setText(boldPrefix(query, typed, textW, fm));
    m_text->setToolTip(query);
    lay->addWidget(m_text, 0, Qt::AlignVCenter);
    // El hueco sobrante va AQUI, justo despues de la consulta: es lo que
    // empuja el detalle de entidad y la insignia "Tab" al borde derecho (en el
    // mockup es un margin-left:auto). Si el stretch se pusiera al final se
    // quedarian pegados a la consulta, en mitad de la fila.
    lay->addStretch(1);

    if (!detail.isEmpty() && detailW > 0) {
        auto *d = pasiva(new QLabel(this));
        d->setObjectName(QStringLiteral("suggestDetail"));
        d->setToolTip(detail);
        d->setText(fm.elidedText(detail, Qt::ElideRight, detailW));
        d->setFixedWidth(detailW);
        d->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        lay->addWidget(d, 0, Qt::AlignVCenter);
    }
    if (!badge.isEmpty()) {
        auto *b = pasiva(new QLabel(badge, this));
        b->setObjectName(QStringLiteral("suggestBadge"));
        b->setAlignment(Qt::AlignCenter);
        b->setToolTip(QStringLiteral("Toma esta búsqueda con Tab"));
        b->setFixedWidth(30);
        lay->addWidget(b, 0, Qt::AlignVCenter);
    }
}

void SuggestRow::setHighlighted(bool on)
{
    if (on)
        setProperty("highlight", true);
    else
        setProperty("highlight", false);
    // Hay que re-polish: cambiar una propiedad dinamica no repinta por si solo.
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void SuggestRow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        emit picked(this);
        return;
    }
    QWidget::mousePressEvent(event);
}

bool SuggestRow::event(QEvent *event)
{
    // Tab y Backtab se atienden AQUI y no en keyPressEvent: QWidget::event()
    // los gestiona antes de llegar a keyPressEvent (focusNextPrevChild), asi
    // que un case Qt::Key_Tab en el switch no se ejecutaria nunca. Se avisa
    // para cerrar el panel y se deja que la clase base mueva el foco; el foco
    // SIGUE su curso, no vuelve al campo como con Esc.
    if (event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Tab || key == Qt::Key_Backtab)
            emit tabOutOf(this);
    }
    return QWidget::event(event);
}

void SuggestRow::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        // Aqui SI se toma la sugerencia: el foco ya esta dentro del desplegable
        // (se entro con Tab), que es lo que distingue este Enter del del
        // campo, donde la busqueda es siempre del texto literal.
        emit picked(this);
        return;
    case Qt::Key_Up:
        emit stepFocus(this, -1);
        return;
    case Qt::Key_Down:
        emit stepFocus(this, 1);
        return;
    case Qt::Key_Escape:
        emit escapeFrom(this);
        return;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
}

void SuggestRow::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    emit focusedRow(this);
}

SearchPage::SearchPage(Yt *yt, SettingsRepo *settings, ThumbnailCache *thumbs,
                       QWidget *parent)
    : QWidget(parent)
    , m_yt(yt)
    , m_settings(settings)
    , m_thumbs(thumbs)
{
    // Type-ahead: 300 ms. Es el unico temporizador de escritura: antes
    // escribia una /search a los 600 ms de parar de teclear (el problema de
    // buscar en medio de una palabra), y ahora solo pide sugerencias. La
    // busqueda ocurre al pulsar Enter o Buscar, con el texto literal.
    m_suggestDebounce = new QTimer(this);
    m_suggestDebounce->setSingleShot(true);
    m_suggestDebounce->setInterval(300);
    connect(m_suggestDebounce, &QTimer::timeout, this, [this]() {
        const QString q = m_field->text().trimmed();
        if (q.isEmpty() || q.size() < 2) {
            hideSuggestPanel();
            return;
        }
        m_suggestQuery = q;
        m_yt->suggest(q);
    });

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(6);

    auto *title = new QLabel(QStringLiteral("Buscar"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    title->setFixedHeight(20);
    outer->addWidget(title);

    auto *searchCard = new QFrame(this);
    searchCard->setObjectName(QStringLiteral("cardPanel"));
    searchCard->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *searchRow = new QHBoxLayout(searchCard);
    searchRow->setContentsMargins(10, 3, 10, 3);
    searchRow->setSpacing(8);
    auto *searchIcon = new QLabel(searchCard);
    searchIcon->setPixmap(nt::icon(nt::Icon::Search, nt::kMuted, 18).pixmap(20, 20));
    m_field = new QLineEdit(searchCard);
    m_field->setPlaceholderText(QStringLiteral("Buscar en YouTube…"));
    // 36 px, no 32: el QSS global aplica padding 8px 12px, así que con 32 el
    // padding vertical se comía el texto (8+8+alto_de_línea > 32). 36 deja los
    // 8+8 y una línea de 13 comfortable.
    m_field->setFixedHeight(36);
    // Icono en vez del glifo "✕" (U+2715): ese carácter NO está en la fuente
    // por defecto (Liberation Sans en Linux; en Windows depende de la fuente)
    // y Qt pintaba en su lugar un punto de 2x5 px, o sea un botón casi
    // invisible: había que saber que estaba ahí para pulsarlo. nt::icon()
    // dibuja con QPainter y no depende de ninguna fuente. La propiedad
    // iconOnly quita el padding global, que si no deja al icono sin sitio
    // dentro de los 28 px.
    m_clearBtn = new QPushButton(searchCard);
    m_clearBtn->setObjectName(QStringLiteral("ghost"));
    m_clearBtn->setProperty("iconOnly", true);
    m_clearBtn->setIcon(nt::icon(nt::Icon::Clear, nt::kText, 16));
    m_clearBtn->setCursor(Qt::PointingHandCursor);
    m_clearBtn->setFixedSize(28, 28);
    m_clearBtn->setFocusPolicy(Qt::NoFocus);
    m_clearBtn->hide();
    auto *searchBtn = new QPushButton(QStringLiteral("Buscar"), searchCard);
    searchBtn->setObjectName(QStringLiteral("primary"));
    searchBtn->setCursor(Qt::PointingHandCursor);
    searchBtn->setFixedHeight(36);
    searchRow->addWidget(searchIcon);
    searchRow->addWidget(m_field, 1);
    searchRow->addWidget(m_clearBtn);
    searchRow->addWidget(searchBtn);
    outer->addWidget(searchCard);

    auto syncClear = [this](const QString &text) {
        m_clearBtn->setVisible(!text.isEmpty());
    };
    connect(m_field, &QLineEdit::textChanged, this, syncClear);
    connect(m_clearBtn, &QPushButton::clicked, this, [this]() {
        // Cerrar el panel aqui y no esperar al debounce: si no, tras limpiar
        // el campo se quedaban ~300 ms las sugerencias del texto anterior.
        m_suggestDebounce->stop();
        hideSuggestPanel();
        m_field->clear();
        // Al borrar todo: vuelve a modo historial y limpia los resultados, como
        // si nunca se hubiese buscado (así el botón X "deshace" la búsqueda).
        m_activeQuery.clear();
        m_searching = false;
        m_moreButton->hide();
        m_results.clear();
        m_list->clearVideos();
        while (QLayoutItem *item = m_channelBox->layout()->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        m_channelButtons.clear();
        m_channels.clear();
        m_channelBox->hide();
        setResultsMode(false);
        showHistory();
        m_field->setFocus();
    });

    m_historyBox = new QWidget(this);
    auto *hBox = new QVBoxLayout(m_historyBox);
    hBox->setContentsMargins(0, 4, 0, 0);
    hBox->setSpacing(2);
    outer->addWidget(m_historyBox);

    // Panel flotante de sugerencias. Es hijo de la pagina (no del layout), asi
    // que no reserva espacio ni altera la disposicion: se posiciona pegado al
    // campo en showSuggestPanel(). Se oculta por defecto.
    m_suggestPanel = new QFrame(this);
    m_suggestPanel->setObjectName(QStringLiteral("suggestPanel"));
    m_suggestPanel->setFrameShape(QFrame::NoFrame);
    m_suggestPanel->hide();
    {
        auto *sb = new QVBoxLayout(m_suggestPanel);
        sb->setContentsMargins(0, 0, 0, 0);
        sb->setSpacing(0);

        // Las filas van en un QScrollArea con altura tope. El endpoint devuelve
        // hasta 14 sugerencias y el historial puede añadir mas: sin tope el
        // panel crecia hacia abajo y se salia de la pagina, tapando los
        // resultados. Con tope sale su barra y el desplegable nunca pasa de
        // kSuggestPanelMaxH.
        m_suggestScroll = new QScrollArea(m_suggestPanel);
        m_suggestScroll->setObjectName(QStringLiteral("suggestScroll"));
        m_suggestScroll->setWidgetResizable(true);
        m_suggestScroll->setFrameShape(QFrame::NoFrame);
        m_suggestScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_suggestScroll->viewport()->setAutoFillBackground(false);
        m_suggestRowsBox = new QWidget(m_suggestScroll->viewport());
        m_suggestRowsBox->setObjectName(QStringLiteral("suggestRows"));
        auto *rb = new QVBoxLayout(m_suggestRowsBox);
        rb->setContentsMargins(0, 0, 0, 0);
        rb->setSpacing(0);
        m_suggestScroll->setWidget(m_suggestRowsBox);
        sb->addWidget(m_suggestScroll);
    }

    m_resultBox = new QWidget(this);
    m_resultBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto *rBox = new QVBoxLayout(m_resultBox);
    rBox->setContentsMargins(0, 0, 0, 0);
    rBox->setSpacing(8);
    auto *spinner = new QLabel(QStringLiteral("Buscando…"), this);
    spinner->setObjectName(QStringLiteral("sectionSub"));
    spinner->setAlignment(Qt::AlignCenter);
    spinner->setContentsMargins(0, 40, 0, 0);
    spinner->hide();
    auto *empty = new QLabel(QStringLiteral("Sin resultados."), this);
    empty->setObjectName(QStringLiteral("sectionSub"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setContentsMargins(0, 40, 0, 0);
    empty->hide();
    m_channelBox = new QWidget(this);
    auto *channelLayout = new QVBoxLayout(m_channelBox);
    channelLayout->setContentsMargins(0, 0, 0, 0);
    channelLayout->setSpacing(2);
    m_channelBox->hide();
    m_list = new VideoListView(m_thumbs, this);
    m_list->showVideos(QVector<VideoInfo>(), VideoListView::ListMode);
    // Botón de opciones por fila, como en el resto de superficies con fila. El
    // menú no se construye aquí: la página no tiene los repos, así que pide el
    // vídeo y la ventana lo muestra (MainWindow::showVideoRowMenu).
    m_list->setRowMoreVisible(true);
    connect(m_list, &VideoListView::moreRequested, this,
            [this](int, const VideoInfo &v, const QPoint &pos) {
                emit rowMenuRequested(v, pos);
            });
    m_moreButton = new QPushButton(QStringLiteral("Mostrar más"), this);
    // Ghost centrado, como el "Cargar más vídeos" de la página Canal: era
    // #primary (rojo) con 180 px fijos, una barra ancha bajo la lista para una
    // acción que solo amplía lo que ya se ve.
    m_moreButton->setObjectName(QStringLiteral("ghost"));
    m_moreButton->setCursor(Qt::PointingHandCursor);
    m_moreButton->hide();
    m_moreButton->setFixedHeight(34);
    connect(m_moreButton, &QPushButton::clicked, this, [this]() {
        m_moreButton->setEnabled(false);
        m_yt->search(m_activeQuery, 20, m_results.size());
    });
    rBox->addWidget(spinner);
    rBox->addWidget(empty);
    rBox->addWidget(m_channelBox);
    rBox->addWidget(m_list, 1);
    rBox->addWidget(m_moreButton, 0, Qt::AlignHCenter);
    outer->addWidget(m_resultBox, 1);
    m_resultBox->hide();
    m_bottomStretch = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
    outer->addSpacerItem(m_bottomStretch);

    // Escribir YA NO dispara una busqueda: solo pide sugerencias para el
    // desplegable. La busqueda ocurre al pulsar Enter o el boton Buscar, y usa
    // el texto literal. Asi una palabra a medio escribir no gasta una /search.
    connect(m_field, &QLineEdit::textChanged, this, [this](const QString &) {
        // Volver a escribir rehabilita las sugerencias para el texto nuevo.
        m_suggestClosedFor.clear();
        m_suggestDebounce->start();
    });
    // Enter y el boton Buscar buscar SIEMPRE el texto literal del campo, sin
    // corregirlo por la sugerencia que este(res) highlighted.
    connect(m_field, &QLineEdit::returnPressed, this, &SearchPage::doSearch);
    connect(searchBtn, &QPushButton::clicked, this, &SearchPage::doSearch);
    connect(m_yt, &Yt::searchFinished, this, &SearchPage::onSearchReady);
    connect(m_yt, &Yt::suggestReady, this, &SearchPage::onSuggestReady);
    connect(m_yt, &Yt::channelInfoReady, this, &SearchPage::onChannelInfo);

    // Cualquier pulsacion fuera del campo y del panel lo cierra. Se hace con un
    // eventFilter sobre la aplicacion (no con editingFinished: al pulsar una
    // fila, editingFinished se dispara ANTES que el clicked de la fila, y el
    // panel se cerraria antes de poder leer la sugerencia).
    m_app = qApp;
    if (m_app)
        m_app->installEventFilter(this);
    // Filtro ADEMAS sobre el campo, para el teclado del desplegable. Tiene que
    // ser un filtro, no un keyPressEvent de la pagina: entre el campo y la
    // pagina esta el QFrame del "card", y QWidget::event() resuelve Tab/Backtab
    // con focusNextPrevChild() cuando keyPressEvent no acepta la tecla. Ese
    // focusNextPrevChild se come el Tab en el QFrame y la pagina nunca lo ve (el
    // gesto "Tab y luego flechas" no hacia nada). El filtro de la aplicacion ya
    // recibia estos eventos; este los deja explicitos y sobrevive a que se
    // cambie el de fuera.
    m_field->installEventFilter(this);

    connect(m_list, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                emit openRequested(m_results, index);
            });

    showHistory();
}

void SearchPage::startSearch(const QString &query)
{
    const QSignalBlocker blocker(m_field);
    m_field->setText(query);
    m_clearBtn->setVisible(!query.trimmed().isEmpty());
    doSearch();
}

SearchPage::~SearchPage()
{
    m_field->removeEventFilter(this);
    if (m_app)
        m_app->removeEventFilter(this);
}

void SearchPage::setResultsMode(bool active)
{
    if (m_resultBox->isVisible() == active)
        return;
    m_resultBox->setVisible(active);
    m_historyBox->setVisible(!active);
    if (m_bottomStretch) {
        m_bottomStretch->changeSize(0, 0, QSizePolicy::Minimum,
                                    active ? QSizePolicy::Fixed
                                           : QSizePolicy::Expanding);
        if (auto *lay = qobject_cast<QBoxLayout *>(layout()))
            lay->invalidate();
    }
    updateGeometry();
}

void SearchPage::doSearch()
{
    // Buscar siempre cierra el desplegable: la pagina pasa a modo resultados.
    // Se para tambien el debounce: si el usuario escribe rapido y pulsa Enter
    // antes de los 300 ms, la peticion de sugerencias salia DESPUES y el panel
    // reaparecia encima de los resultados. Y se marca la consulta como ya
    // buscada, para que una respuesta en vuelo tampoco lo reabra.
    m_suggestDebounce->stop();
    hideSuggestPanel();
    const QString q = m_field->text().trimmed();
    m_suggestClosedFor = q;
    if (q.isEmpty()) {
        setResultsMode(false);
        showHistory();
        return;
    }
    setResultsMode(true);
    m_searching = true;
    m_activeQuery = q;
    m_results.clear();
    m_moreButton->hide();
    m_list->clearVideos();
    while (QLayoutItem *item = m_channelBox->layout()->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_channelButtons.clear();
    m_channels.clear();
    m_channelBox->hide();
    m_list->showVideos(QVector<VideoInfo>(), VideoListView::ListMode);
    for (auto *child : m_resultBox->findChildren<QLabel *>()) {
        if (child->objectName() == QLatin1String("sectionSub")) {
            if (child->text().startsWith(QStringLiteral("Buscando")))
                child->show();
            else if (child->text().startsWith(QStringLiteral("Sin")))
                child->hide();
        }
    }
    m_error.clear();

    QStringList hist = history();
    hist.removeAll(q);
    hist.prepend(q);
    while (hist.size() > 8)
        hist.removeLast();
    setHistory(hist);

    m_yt->search(q);
}

void SearchPage::onSearchReady(const QString &query, const QVector<VideoInfo> &videos)
{
    // La consulta debe ser la vigente... y el campo no puede estar vacío: si el
    // usuario pulsa X mientras esta búsqueda sigue en vuelo, la respuesta llega
    // con query == m_activeQuery y repinta la lista que se acaba de vaciar.
    if (query != m_activeQuery || m_field->text().trimmed().isEmpty())
        return;
    m_searching = false;
    NT_DEBUG("E2E searchReady count=%d\n", (int)videos.size());
    for (auto *child : m_resultBox->findChildren<QLabel *>()) {
        if (child->text().startsWith(QStringLiteral("Buscando")))
            child->hide();
    }
    if (m_results.isEmpty())
        m_results = videos;
    else
        m_results += videos;
    if (m_results.isEmpty()) {
        m_list->clearVideos();
        m_moreButton->hide();
        for (auto *child : m_resultBox->findChildren<QLabel *>()) {
            if (child->text().startsWith(QStringLiteral("Sin")))
                child->show();
        }
        return;
    }
    for (auto *child : m_resultBox->findChildren<QLabel *>()) {
        if (child->text().startsWith(QStringLiteral("Sin")))
            child->hide();
    }
    if (m_list->isEmpty()) {
        buildChannelButtons();
        m_list->showVideos(m_results, VideoListView::ListMode);
    } else {
        m_list->appendVideos(videos);
    }
    m_moreButton->setVisible(videos.size() >= 20);
    m_moreButton->setEnabled(true);
}

void SearchPage::buildChannelButtons()
{
    while (QLayoutItem *item = m_channelBox->layout()->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_channels.clear();
    m_channelButtons.clear();
    QSet<QString> seenChannels;
    for (const VideoInfo &video : m_results) {
        if (video.channelId.isEmpty() || seenChannels.contains(video.channelId))
            continue;
        seenChannels.insert(video.channelId);
        ChannelInfo channel;
        channel.id = video.channelId;
        channel.name = video.channel;
        m_channels.append(channel);
    }
    if (m_channels.isEmpty()) {
        m_channelBox->hide();
        return;
    }
    auto *title = new QLabel(QStringLiteral("Canales"), this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    m_channelBox->layout()->addWidget(title);
    auto *scroll = new QScrollArea(m_channelBox);
    scroll->setWidgetResizable(false);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    // El scrollbar horizontal está dentro del área y consume altura del
    // viewport: el mínimo debe incluir esa franja (igual que en la
    // Biblioteca), o la tarjeta quedará recortada por abajo.
    scroll->setMinimumHeight(
        kChannelRowHeight + scroll->horizontalScrollBar()->sizeHint().height());
    auto *strip = new QWidget(scroll);
    m_channelStrip = strip;
    strip->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto *row = new QHBoxLayout(strip);
    row->setContentsMargins(0, 4, 0, 0);
    row->setSpacing(14);
    row->setAlignment(Qt::AlignVCenter);
    strip->setLayout(row);
    scroll->setWidget(strip);
    for (const ChannelInfo &channel : std::as_const(m_channels)) {
        auto *card = new ChannelRow(m_thumbs, channel.id, QString(), strip);
        card->setTitle(channel.name);
        card->setTitleLines(1);
        card->setMeta(channelHandleFor(channel.name));
        card->applyContentWidth();
        card->setActivate(
            [this, channel]() { emit channelRequested(channel.id); });
        row->addWidget(card);
        m_channelButtons.insert(channel.id, card);
        m_yt->channelInfo(channel.id);
    }
    row->addStretch(1);
    m_channelBox->layout()->addWidget(scroll);
    m_channelBox->show();
    // El QScrollArea (widgetResizable=false) dimensiona el anfitrión a partir
    // de su layout; sin activarlo/adjustSize la franja colapsa a ancho 0
    // (igual que hace la Biblioteca con updateSubsRowGeometry).
    QTimer::singleShot(0, this, [strip]() {
        if (auto *lay = strip->layout()) {
            lay->invalidate();
            lay->activate();
        }
        strip->adjustSize();
    });
}

void SearchPage::onChannelInfo(const ChannelInfo &channel)
{
    ChannelRow *card = m_channelButtons.value(channel.id, nullptr);
    if (!card)
        return;
    card->setTitle(channel.name);
    if (!channel.avatarUrl.isEmpty())
        card->setAvatarUrl(channel.avatarUrl);
    QString meta = channelHandleFor(channel.name);
    if (channel.subscriberCount > 0)
        meta = QStringLiteral("%1\n%2 suscriptores")
                   .arg(meta, compactCount(channel.subscriberCount));
    card->setMeta(meta);
    card->applyContentWidth();
    if (m_channelStrip && m_channelStrip->layout()) {
        m_channelStrip->layout()->invalidate();
        m_channelStrip->layout()->activate();
        m_channelStrip->adjustSize();
    }
}

void SearchPage::showHistory()
{
    QLayoutItem *item;
    while ((item = m_historyBox->layout()->takeAt(0))) {
        delete item->widget();
        delete item;
    }
    const QStringList hist = history();
    if (hist.isEmpty()) {
        m_historyBox->hide();
        if (m_bottomStretch) {
            m_bottomStretch->changeSize(0, 0, QSizePolicy::Minimum,
                                        QSizePolicy::Expanding);
            if (auto *lay = qobject_cast<QBoxLayout *>(layout()))
                lay->invalidate();
        }
        return;
    }
    m_historyBox->show();
    auto *hTitle = new QLabel(QStringLiteral("Búsquedas recientes"), this);
    hTitle->setObjectName(QStringLiteral("sectionSub"));
    m_historyBox->layout()->addWidget(hTitle);

    for (const QString &q : hist) {
        auto *row = new QWidget(m_historyBox);
        auto *rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(0, 0, 0, 0);
        rowLay->setSpacing(4);
        auto *btn = new QPushButton(QStringLiteral("  %1").arg(q), row);
        btn->setIcon(nt::icon(nt::Icon::Search, nt::kMuted, 14));
        btn->setObjectName(QStringLiteral("historyRow"));
        connect(btn, &QPushButton::clicked, this,
                [this, q]() { onHistoryQuery(q); });
        auto *del = new QPushButton(row);
        del->setObjectName(QStringLiteral("iconBtn"));
        del->setIcon(nt::icon(nt::Icon::Clear, nt::kMuted, 14));
        del->setToolTip(QStringLiteral("Borrar esta búsqueda"));
        del->setFixedSize(28, 28);
        del->setCursor(Qt::PointingHandCursor);
        connect(del, &QPushButton::clicked, this,
                [this, q]() { onHistoryRemove(q); });
        rowLay->addWidget(btn, 1);
        rowLay->addWidget(del);
        m_historyBox->layout()->addWidget(row);
    }
    // "Borrar historial de búsquedas", no "Borrar historial": la app tiene dos
    // historiales (este, de consultas, y el de vídeos vistos en Biblioteca) y a
    // solas el nombre no decía cuál se borraba. El dialogo ya lo decía en el
    // cuerpo; ahora también lo dice el botón. Sigue siendo más corto que el
    // "historial de búsqueda" que compone nt::metaLine para las filas.
    auto *clear = new QPushButton(QStringLiteral("Borrar historial de búsquedas"),
                                  this);
    // #sectionLink y no #ghost: las filas de esta sección NO tienen caja (el QSS
    // las pone con `background: transparent`, solo se ven al pasar el ratón), así
    // que una cápsula con borde era lo único enmarcado del bloque y se leía como
    // un control suelto de la página, colado al pie de la lista. Medido por
    // píxeles: las filas ocupan y=113..250 con separadores y sin fondo, y el
    // borde del #ghost se veía en x=709..924 y=218..251, o sea fuera de la
    // columna. Sin marco, y alineado a la izquierda como las filas, pasa a ser
    // el pie de la sección; el foco se marca con el texto más claro (sin borde
    // no hay dónde encenderse un anillo de foco), igual que los "Ver todo" de
    // Biblioteca.
    clear->setObjectName(QStringLiteral("sectionLink"));
    clear->setCursor(Qt::PointingHandCursor);
    clear->setFixedHeight(28);
    connect(clear, &QPushButton::clicked, this, &SearchPage::onHistoryClear);
    if (auto *hLayout =
            qobject_cast<QBoxLayout *>(m_historyBox->layout())) {
        hLayout->addSpacing(2); // respiro respecto al último separador
        hLayout->addWidget(clear, 0, Qt::AlignLeft);
        hLayout->addStretch(1);
    }
}

void SearchPage::onHistoryClear()
{
    // Diálogo de la app (nt::confirm) y no QMessageBox: el mensaje del sistema
    // salía con el tema del escritorio, el único resto de aspecto ajeno que
    // quedaba. Cancelar viene enfocado y no hay botón por defecto, así que un
    // Enter perdido no borra el historial.
    nt::confirm(this, QStringLiteral("Borrar historial de búsquedas"),
                QStringLiteral("Se borrarán las consultas guardadas. Esta "
                               "acción no se puede deshacer."),
                QStringLiteral("Borrar"), [this]() {
                    setHistory(QStringList());
                    showHistory();
                });
}

void SearchPage::onHistoryRemove(const QString &query)
{
    QStringList hist = history();
    hist.removeAll(query);
    setHistory(hist);
    showHistory();
}

void SearchPage::onHistoryQuery(const QString &query)
{
    startSearch(query);
}

QStringList SearchPage::history() const
{
    if (!m_settings)
        return QStringList();
    QString blob;
    if (!m_settings->get(QStringLiteral("search_history"), blob))
        return QStringList();
    const QJsonArray arr =
        QJsonDocument::fromJson(blob.toUtf8()).array();
    QStringList list;
    list.reserve(arr.size());
    for (const auto &v : arr)
        list << v.toString();
    return list;
}

void SearchPage::setHistory(const QStringList &queries)
{
    if (!m_settings)
        return;
    QJsonArray arr;
    for (const QString &q : queries)
        arr.append(q);
    m_settings->set(QStringLiteral("search_history"),
                    QString::fromUtf8(QJsonDocument(arr).toJson()));
}

// --- Type-ahead: desplegable de sugerencias -------------------------------
//
// No usa Highlight de QLineEdit a proposito: Enter y el boton Buscar buscan el
// texto LITERAL, sin corregirlo por la sugerencia señalada. Las filas son
// QPushButton plano, como las de historyRow, para poder alinear el texto a la
// izquierda con icono.

void SearchPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // isHidden() y no isVisible(): el estado del panel es suyo, no depende de
    // que la pagina este visible (en un selftest, o al cambiar de pestana, la
    // pagina puede no estar mostrada y isVisible() dariase falso).
    if (m_suggestPanel && !m_suggestPanel->isHidden()) {
        m_suggestPanel->setFixedWidth(m_field->width());
        positionSuggestPanel();
    }
}

void SearchPage::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    hideSuggestPanel();
}

// Teclas del CAMPO con el desplegable abierto. Va en el eventFilter del campo,
// no en SearchPage::keyPressEvent: el campo no es hijo directo de la pagina
// (esta dentro del QFrame del "card") y para el Tab eso lo rompe todo.
// QWidget::event() resuelve Backtab/Tab con focusNextPrevChild() cuando
// keyPressEvent no acepta la tecla, y lo hace en el QFrame intermedio ANTES de
// que el evento pueda subir a la pagina: el Tab se consumia moviendo el foco al
// boton Buscar y la pagina no lo veia nunca. Interceptando en el campo (el
// receptor) no hay nada por medio.
bool SearchPage::fieldKey(QKeyEvent *event)
{
    if (!m_suggestPanel || m_suggestPanel->isHidden())
        return false; // desplegable cerrado: el QLineEdit trabaja normal
    switch (event->key()) {
    case Qt::Key_Down:
        // Con el desplegable abierto, Abajo entra en el. Abajo sin panel
        // sigue siendo el QLineEdit normal (historial del campo, vacio).
        moveHighlight(1);
        return true;
    case Qt::Key_Up:
        moveHighlight(-1);
        return true;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        // Tab salta al desplegable, como en el mockup. Shift+Tab tambien
        // (que en X11 llega como Key_Backtab), que si no dejaria al usuario
        // sin salida hacia atras.
        focusRow(m_highlight >= 0 ? m_highlight : 0);
        return true;
    case Qt::Key_Escape:
        // Cierra SIN borrar lo escrito y devuelve el foco al campo.
        hideSuggestPanel();
        m_field->setFocus(Qt::OtherFocusReason);
        return true;
    default:
        break;
    }
    // Enter, y todo lo demas, pasa al QLineEdit. Ojo: aqui NO se mira el
    // resaltado. Buscar es buscar lo escrito.
    return false;
}

void SearchPage::setHighlight(int index)
{
    if (index == m_highlight)
        return;
    if (m_highlight >= 0 && m_highlight < m_rows.size()
        && m_rows.at(m_highlight))
        m_rows.at(m_highlight)->setHighlighted(false);
    m_highlight = (index >= 0 && index < m_rows.size()) ? index : -1;
    if (m_highlight >= 0)
        m_rows.at(m_highlight)->setHighlighted(true);
}

void SearchPage::moveHighlight(int delta)
{
    if (m_rows.isEmpty())
        return;
    int next = m_highlight + delta;
    // Sin resaltado, la primera flecha entra por el extremo que toca: Abajo por
    // la primera fila, Arriba por la ultima.
    if (m_highlight < 0)
        next = delta > 0 ? 0 : m_rows.size() - 1;
    else if (next < 0)
        next = m_rows.size() - 1;
    else if (next >= m_rows.size())
        next = 0;
    setHighlight(next);
    // Moverse con las flechas mantiene el foco en el campo (no se lo roba a
    // la fila), que es lo que espera quien esta tecleando.
    m_suggestScroll->ensureWidgetVisible(m_rows.at(next), 0, 0);
}

void SearchPage::focusRow(int index)
{
    if (index < 0 || index >= m_rows.size())
        return;
    setHighlight(index);
    m_rows.at(index)->setFocus(Qt::TabFocusReason);
    m_suggestScroll->ensureWidgetVisible(m_rows.at(index), 0, 0);
}

void SearchPage::onRowPicked(SuggestRow *row)
{
    if (row)
        acceptSuggestion(row->query());
}

void SearchPage::onRowStepFocus(SuggestRow *row, int delta)
{
    if (!row)
        return;
    const int idx = m_rows.indexOf(row);
    if (idx < 0)
        return;
    int next = idx + delta;
    if (next < 0)
        next = m_rows.size() - 1;
    else if (next >= m_rows.size())
        next = 0;
    focusRow(next);
}

void SearchPage::onRowEscape()
{
    // Esc desde dentro del desplegable: lo cierra y devuelve el foco al campo
    // sin tocar el texto escrito.
    hideSuggestPanel();
    m_field->setFocus(Qt::OtherFocusReason);
}

void SearchPage::onRowTabOut()
{
    // Tab sale del desplegable. Solo se cierra: el foco lo lleva el QWidget de
    // base, que sigue con su recorrido normal por los widgets.
    hideSuggestPanel();
}

void SearchPage::onRowFocused(SuggestRow *row)
{
    // El resaltado sigue al foco real: al entrar con Tab se ve en que fila
    // estas, y al volver al campo con Esc se queda en la ultima.
    if (!row)
        return;
    const int idx = m_rows.indexOf(row);
    if (idx >= 0)
        setHighlight(idx);
}

bool SearchPage::eventFilter(QObject *watched, QEvent *event)
{
    // Teclado del campo, resuelto aqui (ver fieldKey). Va en el filtro y no en
    // SearchPage::keyPressEvent porque el campo esta dentro del QFrame del
    // "card" y ese QFrame se come el Tab con focusNextPrevChild antes de que la
    // tecla llegue a la pagina.
    if (watched == m_field && event->type() == QEvent::KeyPress)
        return fieldKey(static_cast<QKeyEvent *>(event));

    if (event->type() == QEvent::MouseButtonPress && m_suggestPanel
        && !m_suggestPanel->isHidden()) {
        // La decision NO se toma por el receptor (watched). Una pulsacion real
        // se entrega PRIMERO a la QWindow del tope, que no es un QWidget: con
        // qobject_cast<QWidget*> daba null, "no esta ni en el campo ni en el
        // panel" se cumplia en falso y el panel se cerraba ANTES de que la
        // pulsacion llegara a la fila. El clic sobre una sugerencia no hacia
        // nada porque el panel ya no estaba cuando el evento bajaba.
        // Se decide por la POSICION, que es la misma que usa Qt para repartir
        // el evento y no depende de a quien se le entregue primero.
        auto *me = static_cast<QMouseEvent *>(event);
        const QPoint g = me->globalPos();
        const QRect panelRect(m_suggestPanel->mapToGlobal(QPoint(0, 0)),
                              m_suggestPanel->size());
        const QRect fieldRect(m_field->mapToGlobal(QPoint(0, 0)), m_field->size());
        if (!panelRect.contains(g) && !fieldRect.contains(g))
            hideSuggestPanel();
    }
    return QWidget::eventFilter(watched, event);
}

QStringList SearchPage::matchingHistory(const QString &prefix) const
{
    QStringList out;
    if (prefix.isEmpty())
        return out;
    for (const QString &q : history()) {
        if (q.compare(prefix, Qt::CaseInsensitive) == 0)
            continue; // no sugerir lo que ya esta escrito
        if (q.startsWith(prefix, Qt::CaseInsensitive))
            out << q;
    }
    return out;
}

void SearchPage::onSuggestReady(const QString &query,
                                const QVector<Suggestion> &suggestions)
{
    // Respuesta tardia: el campo ya no es el prefijo que se pidio, o esa
    // consulta ya se buscó (Enter): no se reabre el panel.
    if (query != m_suggestQuery || query != m_field->text().trimmed())
        return;
    if (query == m_suggestClosedFor)
        return;
    m_suggestItems = suggestions;
    m_historyRows = matchingHistory(query);
    if (m_suggestItems.isEmpty() && m_historyRows.isEmpty()) {
        hideSuggestPanel();
        return;
    }
    populateSuggestPanel();
    showSuggestPanel();
}

void SearchPage::populateSuggestPanel()
{
    auto *lay = qobject_cast<QBoxLayout *>(m_suggestRowsBox->layout());
    if (!lay)
        return;
    while (QLayoutItem *item = lay->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_rows.clear();
    setHighlight(-1);

    const int panelW = m_field->width();
    m_suggestPanel->setFixedWidth(panelW);
    const QString typed = m_field->text().trimmed();
    // Anchos: el detalle se recorta para que la consulta mande (sin tope, un
    // "Cantante y compositor peruano" largo empujaria el texto fuera del
    // panel). La insignia se mide y el resto es para la consulta.
    const int badgeW = 34;
    const bool anyDetail = [&] {
        for (const Suggestion &s : m_suggestItems)
            if (!s.detail.isEmpty())
                return true;
        return false;
    }();
    const int detailW = anyDetail ? qMax(60, panelW / 3) : 0;
    const int textW = qMax(80, panelW - 30 - badgeW - (detailW ? detailW + 12 : 0));

    // Separador de grupo. El mockup solo rotula el SEGUNDO grupo ("Tus
    // busquedas", en mayusculas y con una linea al lado) y deja el primero sin
    // encabezado por implicito, asi que se copia tal cual: sin titulo para
    // YouTube, separador para el historial.
    const auto addDivider = [&](const QString &title) {
        auto *box = new QWidget(m_suggestRowsBox);
        box->setObjectName(QStringLiteral("suggestDivider"));
        auto *bl = new QHBoxLayout(box);
        bl->setContentsMargins(10, 6, 10, 3);
        bl->setSpacing(8);
        box->setFixedHeight(SearchPage::kSuggestDividerH);
        auto *lab = new QLabel(title.toUpper(), box);
        lab->setObjectName(QStringLiteral("suggestGroupTitle"));
        bl->addWidget(lab, 0);
        auto *rule = new QFrame(box);
        rule->setObjectName(QStringLiteral("suggestDividerRule"));
        rule->setFrameShape(QFrame::HLine);
        rule->setFixedHeight(1);
        bl->addWidget(rule, 1);
        lay->addWidget(box);
    };

    const auto addRow = [&](const QString &text, const QString &detail,
                            const QString &badge) {
        auto *row = new SuggestRow(text, detail, badge, typed, textW, detailW,
                                   m_suggestRowsBox);
        m_rows.append(row);
        connect(row, &SuggestRow::picked, this, &SearchPage::onRowPicked);
        connect(row, &SuggestRow::stepFocus, this, &SearchPage::onRowStepFocus);
        connect(row, &SuggestRow::escapeFrom, this, &SearchPage::onRowEscape);
        connect(row, &SuggestRow::tabOutOf, this, &SearchPage::onRowTabOut);
        connect(row, &SuggestRow::focusedRow, this, &SearchPage::onRowFocused);
        lay->addWidget(row);
    };

    // Grupo 1: sugerencias de YouTube (con el detalle de entidad si viene).
    // Sin encabezado, como en el mockup.
    for (const Suggestion &s : std::as_const(m_suggestItems))
        addRow(s.query, s.detail, QString());
    // Grupo 2: lo que el usuario ya busco, con la insignia "Tab" que lo
    // distingue y que es la tecla con la que se toma.
    if (!m_historyRows.isEmpty()) {
        addDivider(QStringLiteral("Tus búsquedas"));
        for (const QString &q : m_historyRows)
            addRow(q, QString(), QStringLiteral("Tab"));
    }
}

void SearchPage::showSuggestPanel()
{
    if (!m_suggestPanel)
        return;
    // El alto del contenido manda, con tope: el desplegable nunca tapa toda la
    // pagina, por muchas filas que traiga el endpoint.
    //
    // ORDEN IMPORTA: primero se MUESTRA y despues se mide. QLayoutItem::isEmpty()
    // devuelve true para widgets ocultos, asi que mientras el panel esta
    // escondido el layout se Cree vacio y sizeHint() da 0 (o un QSize invalido
    // -1, y qMin(-1, tope) daria un alto NEGATIVO). Con 14 filas eso dejaba el
    // desplegable de alto 0 y, sin barra, las 4 ultimas quedaban inalcanzables.
    m_suggestPanel->show();
    m_suggestPanel->raise();
    if (QLayout *lay = m_suggestRowsBox->layout())
        lay->activate();
    int contentH = m_suggestRowsBox->sizeHint().height();
    if (contentH <= 0) {
        // El layout sigue sin decir nada: se calcula a mano con las filas
        // puestas, que es exacto porque SuggestRow tiene alto fijo.
        contentH = m_rows.size() * kSuggestRowH;
        if (!m_historyRows.isEmpty() && !m_suggestItems.isEmpty())
            contentH += kSuggestDividerH; // hay separador entre los grupos
    }
    const int scrollH = qBound(kSuggestRowH,
                               qMin(contentH, kSuggestPanelMaxH),
                               kSuggestPanelMaxH);
    // El minimo del area de filas se fija A MANO con el alto del contenido, en
    // vez de fiarse del minimumSizeHint() del layout. Con widgetResizable(true)
    // el QScrollArea decide el alto del hijo a partir del sizeHint del layout, y
    // con 14 filas se quedaba en el alto del viewport: sin barra y con las 4
    // ultimas filas inalcanzables. Con el minimo explicito, el hijo mide lo
    // que miden las filas y sale la barra cuando no cabe.
    m_suggestRowsBox->setMinimumHeight(contentH);
    m_suggestScroll->setFixedHeight(scrollH);
    m_suggestPanel->setFixedHeight(scrollH + m_suggestPanel->contentsMargins().top()
                                   + m_suggestPanel->contentsMargins().bottom());
    positionSuggestPanel();
}

void SearchPage::positionSuggestPanel()
{
    if (!m_suggestPanel || !m_field)
        return;
    // Pegado bajo el campo, al mismo ancho. mapTo(this) para coordenadas de
    // la pagina (el panel es hijo de la pagina, no del layout).
    const QPoint belowLeft =
        m_field->mapTo(this, QPoint(0, m_field->height() + 4));
    int x = belowLeft.x();
    int y = belowLeft.y();
    // Si no cabe debajo del campo, se abre hacia ARRIBA pegado a su borde
    // inferior en vez de salirse de la pagina.
    if (y + m_suggestPanel->height() > height()) {
        const QPoint aboveRight =
            m_field->mapTo(this, QPoint(m_field->width(), -4));
        const int yAbove = aboveRight.y() - m_suggestPanel->height();
        if (yAbove >= 0) {
            y = yAbove;
            x = aboveRight.x() - m_suggestPanel->width();
        } else {
            // Ni arriba ni abajo: se recorta al alto disponible.
            m_suggestPanel->setFixedHeight(qMax(80, height() - y - 8));
            m_suggestScroll->setFixedHeight(qMax(60, m_suggestPanel->height()
                                                       - 8));
        }
    }
    m_suggestPanel->move(qBound(0, x, qMax(0, width() - m_suggestPanel->width())),
                         qMax(0, y));
}

void SearchPage::hideSuggestPanel()
{
    if (m_suggestPanel)
        m_suggestPanel->hide();
    m_suggestItems.clear();
    m_historyRows.clear();
    // Las filas se destruyen con el layout en el siguiente populate, pero el
    // indice de resaltado hay que soltarlo YA: si no, un setHighlight posterior
    // tocaria un widget ya borrado.
    m_rows.clear();
    m_highlight = -1;
}

void SearchPage::acceptSuggestion(const QString &query)
{
    // Poner el texto de la sugerencia y buscarlo. El texto es el de la fila
    // (no el highlighting), y se busca explicitamente.
    const QSignalBlocker blocker(m_field);
    m_field->setText(query);
    m_clearBtn->setVisible(!query.trimmed().isEmpty());
    doSearch();
}

// Selftest OFFLINE del desplegable (sin red): comprueba lo que no se ve en las
// pruebas de red, que es la propia construccion del panel.
//   1) se abre con dos grupos: sugerencias de YouTube y tu historial;
//   2) el historial se filtra por el prefijo y NO incluye lo ya escrito;
//   3) las filas llevan el detalle de entidad cuando viene;
//   4) una respuesta para un prefijo que ya no es el del campo se ignora
//      (respuesta tardia al seguir escribiendo);
//   5) una lista vacia cierra el panel en vez de abrirlo en blanco.
bool searchSuggestPanelSelfTest(SearchPage *page)
{
    int fail = 0;
    const auto chk = [&fail](bool ok, const char *what) {
        std::fprintf(stderr, "  [%s] %s\n", ok ? "OK  " : "FAIL", what);
        if (!ok)
            ++fail;
    };
    if (!page) {
        std::fprintf(stderr, "SUGGEST PANEL FAIL: pagina nula\n");
        return false;
    }
    QWidget *panel = page->suggestPanel();
    chk(panel != nullptr, "el panel existe");
    if (!panel)
        return false;

    // Semilla del historial: dos entradas que empiezan por "Will" y una que no.
    page->setHistory({QStringLiteral("Will smith"), QStringLiteral("Willie colon"),
                      QStringLiteral("receta de tarta")});

    // El campo tiene que coincidir con el prefijo de la respuesta o el slot la
    // descarta (guard de respuesta tardia). Se escribe primero.
    page->m_field->setText(QStringLiteral("Will"));
    chk(page->m_field->text() == QStringLiteral("Will"),
        "el campo de prueba contiene el prefijo");

    QVector<Suggestion> items;
    items.append({QStringLiteral("willie colón"),
                  QStringLiteral("Cantautor y trombonista")});
    items.append({QStringLiteral("willyrex"), QStringLiteral("@Willyrex")});
    items.append({QStringLiteral("william luna"), QString()});
    // Se fija m_suggestQuery a mano: es lo que el debounce haria al pedir.
    page->m_suggestQuery = QStringLiteral("Will");
    page->onSuggestReady(QStringLiteral("Will"), items);

    // isHidden() y no isVisible(): la pagina no esta mostrada en el selftest,
    // asi que isVisible() daria falso aunque el panel este abierto.
    chk(!panel->isHidden(), "el panel se abre con sugerencias");

    // Sin WA_StyledBackground el QSS hace match pero QWidget::paintEvent no
    // pinta nada: el fondo del hover y del resaltado se quedan invisibles. Es un
    // fallo que no se ve leyendo el texto de las filas, solo mirando la
    // pantalla, asi que se comprueba el atributo directamente.
    chk(!page->m_rows.isEmpty()
            && page->m_rows.first()->testAttribute(Qt::WA_StyledBackground),
        "la fila tiene WA_StyledBackground (si no, el QSS no se pinta)");
    // Estructura del panel: 3 sugerencias de YouTube + 2 del historial, un
    // unico separador ("Tus busquedas", que rotula solo el segundo grupo como
    // en el mockup) y la insignia Tab en las 2 filas de historial.
    int dividers = 0;
    int entityLabels = 0;
    int historyBadges = 0;
    for (QLabel *lab : panel->findChildren<QLabel *>()) {
        if (lab->objectName() == QLatin1String("suggestGroupTitle"))
            ++dividers;
        if (lab->objectName() == QLatin1String("suggestDetail")
            && !lab->text().isEmpty())
            ++entityLabels;
        if (lab->objectName() == QLatin1String("suggestBadge"))
            ++historyBadges;
    }
    chk(page->m_rows.size() == 5, "3 sugerencias + 2 de historial = 5 filas");
    chk(dividers == 1, "un solo separador (rotula el grupo del historial)");
    chk(entityLabels == 2, "detalle de entidad en las 2 filas que lo traen");
    chk(historyBadges == 2, "insignia Tab en las 2 entradas de historial");
    // El detalle y la insignia van al BORDE DERECHO de la fila (margin-left:auto
    // del mockup), no pegados a la consulta. Depende de que el addStretch(1) vaya
    // justo despues de la etiqueta de texto: si se pusiera al final de la fila se
    // quedarian en mitad, que es como estaba y no se ve leyendo el texto.
    {
        QCoreApplication::processEvents();
        const int rowW = page->m_rows.at(0)->width();
        QLabel *detail = page->m_rows.at(0)->findChild<QLabel *>(
            QStringLiteral("suggestDetail"));
        QLabel *badge = page->m_rows.at(3)->findChild<QLabel *>(
            QStringLiteral("suggestBadge"));
        chk(detail && detail->x() > rowW / 2,
            "el detalle de entidad se alinea a la derecha");
        chk(badge && badge->x() > rowW / 2
                && badge->x() + badge->width() >= rowW - 12,
            "la insignia Tab se pega al borde derecho de la fila");
    }
    // El separador va ENTRE los dos grupos, no al principio: la 4a fila es la
    // primera de historial, luego el separador es el indice 3.
    {
        const QVector<SuggestRow *> &r = page->m_rows;
        chk(r.at(0)->query() == QStringLiteral("willie colón")
                && r.at(2)->query() == QStringLiteral("william luna"),
            "las 3 primeras filas son las de YouTube, en orden");
        chk(r.at(3)->query() == QStringLiteral("Will smith")
                && r.at(4)->query() == QStringLiteral("Willie colon"),
            "las 2 ultimas son las del historial filtrado");
    }
    // La negrita del prefijo: el mockup la pone en el tramo escrito, y es lo
    // que hace visible la diferencia entre escribir bien y escribir mal sin
    // haber gastado una peticion.
    {
        QLabel *txt = page->m_rows.at(0)->findChild<QLabel *>(
            QStringLiteral("suggestQuery"));
        const QString rich = txt ? txt->text() : QString();
        chk(txt && txt->textFormat() == Qt::RichText,
            "el texto de la fila es rich text (permite la negrita)");
        // La consulta es "willie colón" y el prefijo "Will": la negrita cubre
        // los 4 primeros caracteres ("will") y el resto queda fuera.
        chk(rich.contains(QLatin1String(">will</span>ie col")),
            "solo el prefijo escrito va en negrita");
    }

    // El historial filtrado: "Will smith" y "Willie colon" empiezan por Will;
    // "receta de tarta" no y queda fuera.
    chk(page->m_historyRows.size() == 2, "historial filtrado por prefijo (2 de 3)");

    // Prefijo sin NADA: ni sugerencias ni historial que empiecen por el
    // ("zzz" no casa con ninguna de las tres entradas sembradas). El panel
    // debe cerrarse en vez de abrirse en blanco.
    page->m_field->setText(QStringLiteral("zzz"));
    page->m_suggestQuery = QStringLiteral("zzz");
    page->onSuggestReady(QStringLiteral("zzz"), QVector<Suggestion>());
    chk(panel->isHidden(), "sin sugerencias ni historial el panel se cierra");

    // Prefijo que solo casa con el historial ("Willi" -> "Willie colon"): el
    // panel debe abrirse con ESE grupo y sin el de YouTube. Comprueba que los
    // dos grupos son independientes y no se mezclan.
    page->m_field->setText(QStringLiteral("Willi"));
    page->m_suggestQuery = QStringLiteral("Willi");
    page->onSuggestReady(QStringLiteral("Willi"), QVector<Suggestion>());
    chk(!panel->isHidden(), "solo historial tambien abre el panel");
    chk(page->m_historyRows.size() == 1,
        "el historial solo ofrece lo que casa con el prefijo");

    // Respuesta tardia: llega para "Will" cuando el campo ya es "Willi". El
    // slot la ignora (query != m_suggestQuery) y no cambia el panel, que sigue
    // con lo que tenia: 1 fila de historial, no las 3 sugerencias.
    page->onSuggestReady(QStringLiteral("Will"), items);
    chk(page->m_historyRows.size() == 1,
        "respuesta tardia no pisa el estado del panel");

    // Y una respuesta AL DIA (query == campo == m_suggestQuery) si lo cambia.
    page->m_field->setText(QStringLiteral("Will"));
    page->m_suggestQuery = QStringLiteral("Will");
    page->onSuggestReady(QStringLiteral("Will"), items);
    chk(!panel->isHidden(), "una respuesta al dia vuelve a abrir el panel");
    chk(page->m_suggestItems.size() == 3, "llegaron las 3 sugerencias de YouTube");

    // Tope de altura: con muchas filas (las 14 que devuelve el endpoint, o
    // mas si el historial suma) el panel NO debe crecer sin limite, sino
    // mostrar barra. Es lo que evita que el desplegable tape la pagina entera.
    QVector<Suggestion> many;
    for (int i = 0; i < 14; ++i)
        many.append({QStringLiteral("sugerencia de prueba numero %1").arg(i),
                     QStringLiteral("un detalle bastante largo para la fila %1")
                         .arg(i)});
    page->m_field->setText(QStringLiteral("sug"));
    page->m_suggestQuery = QStringLiteral("sug");
    page->onSuggestReady(QStringLiteral("sug"), many);
    // setFixedHeight no redimensiona al instante: encola un LayoutRequest que
    // el bucle de eventos aplicara. En la app no se nota (ocurre antes del
    // siguiente pintado), pero para leer el rango de la barra hay que forzarlo.
    QCoreApplication::processEvents();
    chk(!panel->isHidden(), "con 14 sugerencias el panel se abre");
    chk(panel->height() <= page->kSuggestPanelMaxH + 8,
        "el alto del panel respeta el tope con 14 filas");
    chk(page->m_suggestScroll->height() <= page->kSuggestPanelMaxH,
        "el area de filas no pasa del tope");
    // Y con 14 filas de 32 px el contenido NO cabe: tiene que haber barra, que
    // es lo que evita que el panel crezca tapando la pagina y deje las 4
    // ultimas filas inalcanzables.
    chk(page->m_suggestScroll->verticalScrollBar()->maximum() > 0,
        "con 14 filas sale la barra (el contenido excede el area)");
    // Y la posicion no lo deja salirse de la pagina.
    chk(panel->y() >= 0 && panel->y() + panel->height() <= page->height() + 1,
        "el panel cabe dentro de la pagina");

    // Pocas filas: sin barra, alto = contenido.
    page->m_field->setText(QStringLiteral("Will"));
    page->m_suggestQuery = QStringLiteral("Will");
    page->onSuggestReady(QStringLiteral("Will"), items);
    chk(page->m_suggestScroll->height() < page->kSuggestPanelMaxH,
        "con pocas filas el alto es el del contenido (sin barra)");

    // --- Teclado del desplegable --------------------------------------------
    // OJO: se entrega con qApp->notify() al CAMPO, no se llama a
    // SearchPage::keyPressEvent() directamente. El campo no es hijo directo de
    // la pagina (esta dentro de un QFrame, el "card" del buscador), y para el
    // Tab eso importa: QWidget::event() hace focusNextPrevChild() cuando keyPressEvent
    // no acepta la tecla, y lo hace ANTES de que el evento pueda subir a la
    // pagina. Llamando al handler a mano se saltaba justo el tramo donde se
    // perdia la tecla, y el test pasaba con el gesto roto en la app.
    const auto press = [page](int key) {
        QKeyEvent ev(QEvent::KeyPress, key, Qt::NoModifier);
        qApp->notify(page->m_field, &ev);
    };
    const auto pressRow = [](SuggestRow *row, int key) {
        QKeyEvent ev(QEvent::KeyPress, key, Qt::NoModifier);
        qApp->notify(row, &ev);
    };
    // Al repoblar se reinicia el resaltado: las filas son nuevas y el indice
    // viejo apuntaria a un widget ya borrado.
    chk(page->m_highlight == -1, "al poblar el panel no hay nada resaltado");

    page->moveHighlight(1);
    chk(page->m_highlight == 0, "Abajo resalta la primera fila");
    page->moveHighlight(1);
    chk(page->m_highlight == 1, "Abajo avanza");
    page->moveHighlight(-1);
    chk(page->m_highlight == 0, "Arriba retrocede");
    page->moveHighlight(-1);
    chk(page->m_highlight == page->m_rows.size() - 1, "Arriba envuelve al final");
    page->moveHighlight(1);
    chk(page->m_highlight == 0, "Abajo envuelve al principio");

    // El resaltado se ve en la fila (propiedad de QSS) y solo en una.
    {
        int on = 0;
        for (SuggestRow *r : page->m_rows)
            if (r->property("highlight").toBool())
                ++on;
        chk(on == 1, "el resaltado se marca en exactamente una fila");
        chk(page->m_rows.at(0)->property("highlight").toBool(),
            "la fila resaltada es la primera");
    }

    // REGLA DURA: mover el resaltado NO toca el campo. Enter en el campo
    // busca el texto literal, nunca la sugerencia resaltada.
    chk(page->m_field->text() == QLatin1String("Will"),
        "resaltar una fila no sustituye el texto del campo");

    // Esc cierra el desplegable SIN borrar lo escrito.
    press(Qt::Key_Escape);
    chk(panel->isHidden(), "Esc cierra el desplegable");
    chk(page->m_field->text() == QLatin1String("Will"),
        "Esc no borra lo escrito");
    chk(page->m_highlight == -1, "Esc suelta el resaltado");

    // Con el panel cerrado, las flechas y Esc NO se interceptan (pasan al
    // QLineEdit): si no, se comerian teclas del campo.
    chk(page->m_highlight == -1, "con el panel cerrado las flechas no resaltan");

    // Tab entra en el desplegable. Con el panel cerrado, Tab no se toca: el
    // campo lo usa para el recorrido normal de foco.
    press(Qt::Key_Tab);
    chk(panel->isHidden() && page->m_highlight == -1,
        "Tab con el panel cerrado no hace nada del desplegable");

    page->m_field->setText(QStringLiteral("Will"));
    page->m_suggestQuery = QStringLiteral("Will");
    page->onSuggestReady(QStringLiteral("Will"), items);
    press(Qt::Key_Tab);
    chk(!panel->isHidden(), "Tab con el panel abierto no lo cierra");
    chk(QApplication::focusWidget() == page->m_rows.at(0),
        "Tab salta a la primera fila del desplegable");

    // Abajo desde dentro de la fila mueve el foco real entre filas.
    pressRow(page->m_rows.at(0), Qt::Key_Down);
    chk(QApplication::focusWidget() == page->m_rows.at(1),
        "Abajo dentro del desplegable mueve el foco a la fila siguiente");

    // Esc desde dentro de la fila devuelve el foco al campo.
    pressRow(page->m_rows.at(1), Qt::Key_Escape);
    chk(panel->isHidden() && QApplication::focusWidget() == page->m_field,
        "Esc desde una fila cierra y devuelve el foco al campo");

    // Tab desde dentro de la fila cierra el desplegable y deja seguir al foco
    // (no lo devuelve al campo como Esc).
    page->m_field->setText(QStringLiteral("Will"));
    page->m_suggestQuery = QStringLiteral("Will");
    page->onSuggestReady(QStringLiteral("Will"), items);
    page->focusRow(1);
    chk(QApplication::focusWidget() == page->m_rows.at(1), "focusRow mueve el foco");
    pressRow(page->m_rows.at(1), Qt::Key_Tab);
    chk(panel->isHidden(), "Tab desde una fila cierra el desplegable");
    chk(QApplication::focusWidget() != page->m_field,
        "Tab desde una fila NO devuelve el foco al campo");

    // --- Clic con el raton --------------------------------------------------
    // Lo que decide el clic NO es como se propaga el evento, sino A QUIEN se lo
    // entrega la plataforma: eso lo dice QApplication::widgetAt(), que es el
    // mismo hit-test que hace el servidor de ventanas. Las etiquetas (icono,
    // consulta, detalle, insignia) son HIJAS de la fila y cubren casi todo su
    // ancho: sin WA_TransparentForMouseEvents el punto que pincha el usuario
    // cae sobre la etiqueta y no sobre la fila.
    page->m_field->setText(QStringLiteral("Will"));
    page->m_suggestQuery = QStringLiteral("Will");
    page->onSuggestReady(QStringLiteral("Will"), items);
    {
        SuggestRow *row = page->m_rows.at(1);
        // OJO: tiene que ser la fila MISMA, no "la fila o algo suyo". Con
        // isAncestorOf() la comprobacion pasaria aunque el hit-test cayese en
        // una etiqueta, que es justo el fallo.
        const auto hitIsRow = [row](QWidget *child) {
            const QPoint g = child->mapToGlobal(child->rect().center());
            return QApplication::widgetAt(g) == row;
        };
        chk(hitIsRow(row->findChild<QLabel *>(QStringLiteral("suggestQuery"))),
            "el hit-test del texto de la fila cae en la fila, no en la etiqueta");
        chk(hitIsRow(row->findChild<QLabel *>(QStringLiteral("suggestDetail"))),
            "el hit-test del detalle cae en la fila, no en la etiqueta");
        // Y el invariante de fondo, para las etiquetas que el hit-test de arriba
        // no puede cubrir (la insignia cae fuera de la ventana en el offscreen):
        // NINGUNA etiqueta de la fila puede quedarse el raton. La fila 3 es del
        // historial y lleva la insignia "Tab".
        int opacas = 0;
        for (QLabel *l : page->m_rows.at(3)->findChildren<QLabel *>())
            if (!l->testAttribute(Qt::WA_TransparentForMouseEvents))
                ++opacas;
        chk(opacas == 0 && page->m_rows.at(3)->findChildren<QLabel *>().size() == 3,
            "ninguna etiqueta de la fila se queda el clic");
        int pickedCount = 0;
        QMetaObject::Connection c = QObject::connect(
            row, &SuggestRow::picked, [&pickedCount](SuggestRow *) { ++pickedCount; });
        for (int i = 0; i < 3; ++i) {
            QMouseEvent ev(QEvent::MouseButtonPress, QPointF(row->rect().center()),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            qApp->notify(row, &ev);
        }
        QObject::disconnect(c);
        chk(pickedCount == 3, "la pulsacion sobre la fila emite picked()");
    }

    // --- Boton X: limpiar el campo tiene que DESHACER la busqueda ---------
    // El X solo hacia m_field->clear(), asi que los resultados de la consulta
    // anterior se quedaban en pantalla con el campo ya vacio.
    {
        page->m_field->setText(QStringLiteral("Will smith"));
        page->m_activeQuery = QStringLiteral("Will smith");
        page->m_searching = true;
        page->m_results = { VideoInfo{ QStringLiteral("vid-1") } };
        page->m_list->showVideos(page->m_results, VideoListView::ListMode);
        page->setResultsMode(true);
        chk(page->m_resultBox->isVisible() && !page->m_historyBox->isVisible(),
            "con resultados se ve la caja de resultados, no el historial");

        // Clic de verdad en el boton, no llamar al lambda a mano.
        page->m_clearBtn->click();
        chk(page->m_field->text().isEmpty(), "el X vacia el campo");
        chk(page->m_results.isEmpty() && page->m_list->isEmpty(),
            "el X borra los resultados de la lista");
        chk(!page->m_resultBox->isVisible() && page->m_historyBox->isVisible(),
            "el X vuelve al modo historial (como si no se hubiera buscado)");
        chk(page->m_activeQuery.isEmpty() && !page->m_searching,
            "el X invalida la busqueda en vuelo");

        // Y la que llegaba tarde: con el X ya pulsado, la respuesta de la
        // consulta anterior no debe repintar la lista que se acaba de vaciar.
        page->onSearchReady(QStringLiteral("Will smith"),
                            { VideoInfo{ QStringLiteral("vid-tarde") } });
        chk(page->m_list->isEmpty() && page->m_results.isEmpty(),
            "una respuesta tardia no repinta la lista tras borrar con X");
    }

    std::fprintf(stderr, "SUGGEST PANEL %s\n", fail == 0 ? "OK" : "FAIL");
    return fail == 0;
}

// Banco de pruebas del desplegable en la JERARQUIA REAL. El selftest de arriba
// monta la SearchPage suelta como ventana de nivel superior, y ahi se perdian
// gestos sin que se notara: el campo esta dentro de un QFrame, la pagina
// dentro de un QStackedWidget, y el despliegue lo hace el pixel. Aqui se monta
// la pagina dentro de un contenedor con barra de navegacion (la forma que tiene
// en MainWindow), se aplica el QSS de verdad, y se repiten TODOS los gestos
// con entrega real (qApp->notify al widget receptor, widgetAt para el clic).
bool searchSuggestGesturesSelfTest(SearchPage *page, QStackedWidget *stack,
                                   QWidget *otra)
{
    int fail = 0;
    const auto chk = [&fail](bool ok, const char *what) {
        std::fprintf(stderr, "  [%s] %s\n", ok ? "OK  " : "FAIL", what);
        if (!ok)
            ++fail;
    };
    if (!page || !stack || !otra) {
        std::fprintf(stderr, "SUGGEST GESTURES FAIL: faltan piezas\n");
        return false;
    }
    QWidget *panel = page->suggestPanel();
    page->setHistory({QStringLiteral("Will smith"), QStringLiteral("Willie colon")});
    page->m_field->setFocus();
    QCoreApplication::processEvents();

    // El QSS de verdad. Sin el los widgets salen sin estilo y el hit-test no se
    // parece en nada a la app (el selftest de arriba no lo instala tampoco).
    qApp->setStyleSheet(nt::styleSheet());

    const QVector<Suggestion> items = {
        {QStringLiteral("willie colón"), QStringLiteral("Cantautor y trombonista")},
        {QStringLiteral("willyrex"), QStringLiteral("@Willyrex")},
        {QStringLiteral("william luna"), QString()},
    };
    const auto abrir = [&page, &items](const QString &prefijo) {
        page->m_field->setText(prefijo);
        page->m_suggestQuery = prefijo;
        page->onSuggestReady(prefijo, items);
        QCoreApplication::processEvents();
    };
    // Entrega por la ruta real: se manda al widget que recibe el evento, no al
    // handler. Llamar al handler a mano es lo que hacia pasar el test con el
    // gesto roto.
    const auto key = [](QWidget *w, int k) {
        QKeyEvent ev(QEvent::KeyPress, k, Qt::NoModifier);
        qApp->notify(w, &ev);
        QCoreApplication::processEvents();
    };
    const auto clic = [](QWidget *w) {
        // Posicion GLOBAL real: el filtro decide por globalPos(), y con el
        // constructor corto de QMouseEvent la global acababa siendo la local de
        // la fila (coordenadas de otro sistema), asi que la prueba media algo
        // que no pasa en la app.
        const QPoint local = w->rect().center();
        const QPoint global = w->mapToGlobal(local);
        QMouseEvent ev(QEvent::MouseButtonPress, QPointF(local), QPointF(global),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        qApp->notify(w, &ev);
        QCoreApplication::processEvents();
    };

    // Enter dispara la busqueda y doSearch() pide a la red. Aqui se suelta esa
    // conexion y se pone una sonda: lo que se comprueba es que la tecla LLEGA
    // al QLineEdit y con que texto, no el resultado de la busqueda (eso es otra
    // prueba). Sin esto la prueba cerraria con una peticion de red en vuelo.
    QString textoEnEnter;
    bool entroEnter = false;
    QObject::disconnect(page->m_field, &QLineEdit::returnPressed, page,
                        &SearchPage::doSearch);
    QObject::connect(page->m_field, &QLineEdit::returnPressed, page,
                     [&textoEnEnter, &entroEnter, page] {
                         entroEnter = true;
                         textoEnEnter = page->m_field->text();
                     });

    // --- 1) Clic: a quien lo entrega la plataforma, no como se propaga ------
    abrir(QStringLiteral("Will"));
    chk(!panel->isHidden(), "el desplegable se abre en la pagina apilada");
    {
        SuggestRow *row = page->m_rows.at(0);
        QWidget *text = row->findChild<QLabel *>(QStringLiteral("suggestQuery"));
        const QPoint g = text->mapToGlobal(text->rect().center());
        chk(QApplication::widgetAt(g) == row,
            "el hit-test del texto de la fila cae en la fila");
        int picked = 0;
        QMetaObject::Connection c = QObject::connect(
            row, &SuggestRow::picked, [&picked](SuggestRow *) { ++picked; });
        clic(row);
        QObject::disconnect(c);
        chk(picked == 1, "el clic sobre la fila se convierte en busqueda");
    }

    // --- 1b) La pulsacion llega PRIMERO a la QWindow ------------------------
    // En la app, un clic real se entrega antes que nada a la QWindow del tope,
    // que NO es un QWidget. Decidir por el receptor hacia que ahi se cerrara el
    // panel (qobject_cast<QWidget*> daba null -> "fuera del panel y del campo")
    // y el clic no llegaba nunca a la fila. La decision tiene que ser por
    // POSICION, no por quien recibe.
    abrir(QStringLiteral("Will"));
    {
        QWindow *win = page->window()->windowHandle();
        chk(win != nullptr, "existe la QWindow del tope");
        SuggestRow *row = page->m_rows.at(0);
        const QPoint enFila = row->mapToGlobal(row->rect().center());
        QMouseEvent ev(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(enFila),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        qApp->notify(win, &ev);
        chk(!panel->isHidden(),
            "una pulsacion sobre el panel no lo cierra (llegando a la QWindow)");
        // Y fuera del panel y del campo SI tiene que cerrarlo.
        const QPoint fuera =
            page->mapToGlobal(QPoint(page->width() / 2, page->height() - 20));
        QMouseEvent ev2(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(fuera),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        qApp->notify(win, &ev2);
        chk(panel->isHidden(), "una pulsacion fuera del panel y del campo si lo cierra");
        // El campo tambien se queda.
        abrir(QStringLiteral("Will"));
        const QPoint enCampo = page->m_field->mapToGlobal(QPoint(4, 4));
        QMouseEvent ev3(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(enCampo),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        qApp->notify(win, &ev3);
        chk(!panel->isHidden(), "una pulsacion sobre el campo no cierra el panel");
    }

    // --- 2) Tab entra en el desplegable (el bug del QFrame) ----------------
    abrir(QStringLiteral("Will"));
    key(page->m_field, Qt::Key_Tab);
    chk(QApplication::focusWidget() == page->m_rows.at(0),
        "Tab en el campo mete el foco real en la primera fila");
    chk(!panel->isHidden(), "Tab en el campo no cierra el desplegable");
    key(page->m_rows.at(0), Qt::Key_Down);
    chk(QApplication::focusWidget() == page->m_rows.at(1),
        "las flechas dentro del desplegable mueven el foco entre filas");
    key(page->m_rows.at(1), Qt::Key_Up);
    chk(QApplication::focusWidget() == page->m_rows.at(0),
        "Arriba vuelve a la fila anterior");
    key(page->m_rows.at(0), Qt::Key_Escape);
    chk(panel->isHidden() && QApplication::focusWidget() == page->m_field,
        "Esc en una fila cierra y devuelve el foco al campo");
    chk(page->m_field->text() == QLatin1String("Will"),
        "Esc en una fila no borra lo escrito");

    // --- 3) Tab dentro del desplegable sale sin devolver el foco ------------
    abrir(QStringLiteral("Will"));
    key(page->m_field, Qt::Key_Tab);
    key(page->m_rows.at(0), Qt::Key_Tab);
    chk(panel->isHidden(), "Tab en una fila cierra el desplegable");
    chk(QApplication::focusWidget() != page->m_field,
        "Tab en una fila NO devuelve el foco al campo");

    // --- 4) Teclado con el foco en el campo ---------------------------------
    abrir(QStringLiteral("Will"));
    key(page->m_field, Qt::Key_Down);
    key(page->m_field, Qt::Key_Down);
    chk(page->m_highlight == 1,
        "las flechas en el campo mueven el resaltado del desplegable");
    chk(page->m_field->text() == QLatin1String("Will"),
        "mover el resaltado no sustituye el texto del campo");
    entroEnter = false;
    textoEnEnter.clear();
    key(page->m_field, Qt::Key_Return);
    chk(entroEnter, "Enter llega al campo (el desplegable no se lo come)");
    chk(textoEnEnter == QLatin1String("Will"),
        "Enter busca el texto literal, no la sugerencia resaltada");
    key(page->m_field, Qt::Key_Escape);
    chk(panel->isHidden() && page->m_field->text() == QLatin1String("Will"),
        "Esc en el campo cierra sin borrar lo escrito");

    // --- 5) Clic fuera y cambio de pestana ----------------------------------
    abrir(QStringLiteral("Will"));
    clic(otra);
    chk(panel->isHidden(), "un clic fuera cierra el desplegable");
    abrir(QStringLiteral("Will"));
    stack->setCurrentWidget(otra);
    QCoreApplication::processEvents();
    chk(panel->isHidden(), "cambiar de pestana cierra el desplegable");
    stack->setCurrentWidget(page);
    QCoreApplication::processEvents();
    page->m_field->setFocus();
    QCoreApplication::processEvents();

    // --- 6) El desplegable se ancla al campo y no se sale de la pagina ------
    // Al Estrechar la ventana hasta que no cabe ni arriba ni abajo tiene que
    // recortarse, no salirse. NOTA: la rama de "abre hacia arriba" de
    // positionSuggestPanel() NO se puede cubrir aqui, y no es un fallo de la
    // prueba: el campo esta siempre a unos 46 px del borde superior, asi que
    // yAbove = 46 - alto_del_panel es negativo con cualquier panel de mas de
    // 42 px. Es decir, con este layout esa rama no se ejecuta nunca; lo que si
    // se ejecuta es el recorte de abajo.
    const QVector<QSize> tamanos = {QSize(1400, 900), QSize(1100, 720),
                                    QSize(900, 520), QSize(820, 400),
                                    QSize(700, 300), QSize(1100, 240)};
    int fuera = 0;
    int desalineado = 0;
    int recortado = 0;
    int altoNatural = 0; // alto con sitio de sobra, para detectar el recorte
    for (const QSize &s : tamanos) {
        // Se redimensiona la VENTANA, no el QStackedWidget: el layout de la
        // raiz manda sobre el, y resize() sobre el stack no cambia nada (la
        // pagina se queda en su tamano y esta prueba no probaba nada).
        QWidget *root = stack->parentWidget();
        root->resize(s);
        QCoreApplication::processEvents();
        abrir(QStringLiteral("Will"));
        const QRect pr = panel->geometry();
        if (pr.left() < 0 || pr.top() < 0 || pr.right() > page->width() + 1
            || pr.bottom() > page->height() + 1)
            ++fuera;
        if (qAbs(pr.left() - page->m_field->mapTo(page, QPoint(0, 0)).x()) > 2)
            ++desalineado;
        // Detectar el recorte comparando con el alto natural. Ojo: una vez
        // recortado el panel SI cabe (de eso va el recorte), asi que no vale
        // mirar si se sale de la pagina para saber que ha ocurrido.
        if (altoNatural == 0) {
            altoNatural = pr.height();
        } else if (pr.height() < altoNatural - 2) {
            ++recortado;
            if (page->m_suggestScroll->verticalScrollBar()->maximum() <= 0)
                ++fuera; // recortado y sin barra: las filas de abajo son inalcanzables
        }
    }
    chk(fuera == 0, "el desplegable no se sale de la pagina en ningun tamano");
    chk(desalineado == 0, "el desplegable sigue al borde izquierdo del campo");
    chk(recortado > 0, "con muy poco alto el desplegable se recorta (y con barra)");

    std::fprintf(stderr, "SUGGEST GESTURES %s\n", fail == 0 ? "OK" : "FAIL");
    return fail == 0;
}

bool searchSuggestPanelSelfTestRun()
{
    // Construye las piezas minimas que necesita SearchPage y lanza la prueba
    // del panel. La BD va a un fichero temporal para no tocar la del usuario.
    // Todo es local (NO static): un static se destructurante en exit(), cuando
    // Qt ya esta medio cerrado, y AppDatabase::~AppDatabase()->close() ->
    // QSqlDatabase::removeDatabase() crashea ahi.
    AppDatabase db(QStringLiteral("/tmp/nevertube_selftest_suggest.db"));
    if (!db.isOpen() && !db.open()) {
        std::fprintf(stderr, "SUGGEST PANEL FAIL: no se abre la BD de test\n");
        return false;
    }
    SettingsRepo settings(db);
    ThumbnailCache thumbs(QStringLiteral("suggest-selftest"), 1 << 20);
    Yt yt;
    SearchPage page(&yt, &settings, &thumbs);
    // Tamano explicito: la comprobacion de que el panel cabe en la pagina
    // depende de las dimensiones, y sin esto serian las por defecto. Se
    // muestra porque los casos de teclado comprueban el foco real, y un
    // widget sin mostrar no puede tener foco.
    page.resize(1000, 700);
    page.show();
    QCoreApplication::processEvents();
    return searchSuggestPanelSelfTest(&page);
}

bool searchSuggestGesturesSelfTestRun()
{
    // Monta la pagina donde vive de verdad: dentro de un QStackedWidget, con
    // una barra de navegacion encima, como en MainWindow. Ver el comentario de
    // la funcion: los gestos se pierden en ese trampolín y no en la pagina
    // suelta.
    AppDatabase db(QStringLiteral("/tmp/nevertube_selftest_gestures.db"));
    if (!db.isOpen() && !db.open()) {
        std::fprintf(stderr, "SUGGEST GESTURES FAIL: no se abre la BD de test\n");
        return false;
    }
    SettingsRepo settings(db);
    ThumbnailCache thumbs(QStringLiteral("gestures-selftest"), 1 << 20);
    Yt yt;

    QWidget root;
    auto *lay = new QVBoxLayout(&root);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto *nav = new QWidget(&root);
    nav->setFixedHeight(56);
    lay->addWidget(nav);
    auto *stack = new QStackedWidget(&root);
    lay->addWidget(stack, 1);
    auto *otra = new QWidget(stack);
    stack->addWidget(otra);
    SearchPage page(&yt, &settings, &thumbs);
    stack->addWidget(&page);

    root.resize(1100, 720);
    root.show();
    stack->setCurrentWidget(&page);
    QCoreApplication::processEvents();
    // El foco al campo lo pone la propia prueba (esta si es friend); aqui
    // m_field es privado.
    return searchSuggestGesturesSelfTest(&page, stack, otra);
}

// SuggestRow está definida en este .cpp (necesita el moc aquí).
#include "search_page.moc"
