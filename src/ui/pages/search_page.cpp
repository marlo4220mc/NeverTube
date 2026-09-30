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
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QRegularExpression>

#include <utility>

#include "../../debug.h"
#include "../../db/app_database.h"
#include "../../db/repositories.h"
#include "../../models.h"
#include "../../yt.h"
#include "../channel_row.h"
#include "../format.h"
#include "../icons.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

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
    m_clearBtn = new QPushButton(QStringLiteral("✕"), searchCard);
    m_clearBtn->setObjectName(QStringLiteral("ghost"));
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
        m_field->clear();
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
    m_moreButton = new QPushButton(QStringLiteral("Mostrar más"), this);
    m_moreButton->setObjectName(QStringLiteral("primary"));
    m_moreButton->setCursor(Qt::PointingHandCursor);
    m_moreButton->hide();
    m_moreButton->setFixedWidth(180);
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
    hideSuggestPanel();
    const QString q = m_field->text().trimmed();
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
    if (query != m_activeQuery)
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
    auto *clear = new QPushButton(QStringLiteral("Borrar historial"), this);
    clear->setObjectName(QStringLiteral("primary"));
    clear->setCursor(Qt::PointingHandCursor);
    clear->setFixedWidth(180);
    connect(clear, &QPushButton::clicked, this, &SearchPage::onHistoryClear);
    if (auto *box = qobject_cast<QBoxLayout *>(m_historyBox->layout()))
        box->addWidget(clear, 0, Qt::AlignHCenter);
    if (auto *box = qobject_cast<QBoxLayout *>(m_historyBox->layout()))
        box->addStretch(1);
}

void SearchPage::onHistoryClear()
{
    const auto btn = QMessageBox::question(
        this, QStringLiteral("Borrar historial"),
        QStringLiteral("¿Borrar todo el historial de búsquedas? Esta acción no "
                       "se puede deshacer."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (btn != QMessageBox::Yes)
        return;
    setHistory(QStringList());
    showHistory();
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

bool SearchPage::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress && m_suggestPanel
        && !m_suggestPanel->isHidden()) {
        auto *w = qobject_cast<QWidget *>(watched);
        // El panel y el campo se quedan; cualquier otra pulsacion lo cierra.
        // NO se mira editingFinished porque al pulsar una fila se dispara
        // ANTES que su clicked(), y el panel se cerraria sin poder leer la
        // sugerencia.
        const bool inPanel = w && (w == m_suggestPanel
                                   || m_suggestPanel->isAncestorOf(w));
        const bool inField = w && (w == m_field || m_field->isAncestorOf(w));
        if (!inPanel && !inField)
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
    // Respuesta tardia: el campo ya no es el prefijo que se pidio.
    if (query != m_suggestQuery || query != m_field->text().trimmed())
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
    const int panelW = m_field->width();
    m_suggestPanel->setFixedWidth(panelW);
    // El detalle de entidad se recorta para que la consulta mande: sin tope un
    // "Cantante y compositor peruano" largo empuja el texto fuera del panel.
    const int detailW = qMax(60, panelW / 2);

    const auto addGroup = [&](const QString &title) {
        auto *lab = new QLabel(title, m_suggestRowsBox);
        lab->setObjectName(QStringLiteral("suggestGroupTitle"));
        lay->addWidget(lab);
    };
    const auto addRow = [&](const QString &text, const QString &detail,
                            const QString &badge) {
        auto *row = new QWidget(m_suggestRowsBox);
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(0, 0, 0, 0);
        rl->setSpacing(6);
        auto *btn = new QPushButton(text, row);
        btn->setObjectName(QStringLiteral("suggestRow"));
        btn->setIcon(nt::icon(nt::Icon::Search, nt::kMuted, 14));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setToolTip(QStringLiteral("Buscar esta sugerencia"));
        connect(btn, &QPushButton::clicked, this,
                [this, text]() { acceptSuggestion(text); });
        rl->addWidget(btn, 1);
        if (!detail.isEmpty()) {
            auto *d = new QLabel(m_suggestRowsBox);
            d->setObjectName(QStringLiteral("suggestDetail"));
            // El texto completo va al tooltip; en la fila va recortado.
            d->setToolTip(detail);
            QFontMetrics fm(d->font());
            d->setText(fm.elidedText(detail, Qt::ElideRight, detailW));
            d->setFixedWidth(detailW);
            d->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
            rl->addWidget(d, 0, Qt::AlignVCenter);
        }
        if (!badge.isEmpty()) {
            auto *b = new QLabel(badge, row);
            b->setObjectName(QStringLiteral("suggestDetail"));
            rl->addWidget(b, 0, Qt::AlignVCenter);
        }
        lay->addWidget(row);
    };

    // Grupo 1: sugerencias de YouTube (con el detalle de entidad si viene).
    if (!m_suggestItems.isEmpty()) {
        addGroup(QStringLiteral("Sugerencias de YouTube"));
        for (const Suggestion &s : std::as_const(m_suggestItems))
            addRow(s.query, s.detail, QString());
    }
    // Grupo 2: tu historial, con la insignia "Tab" que lo distingue.
    if (!m_historyRows.isEmpty()) {
        addGroup(QStringLiteral("Tu historial"));
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
    const int contentH = m_suggestRowsBox->sizeHint().height();
    m_suggestScroll->setFixedHeight(qMin(contentH, kSuggestPanelMaxH));
    m_suggestPanel->setFixedHeight(m_suggestScroll->height()
                                   + m_suggestPanel->contentsMargins().top()
                                   + m_suggestPanel->contentsMargins().bottom());
    positionSuggestPanel();
    m_suggestPanel->raise();
    m_suggestPanel->show();
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
    // Dos grupos: dos titulos (Sugerencias / Tu historial).
    int groupTitles = 0;
    int entityLabels = 0;
    int historyBadges = 0;
    for (QLabel *lab : panel->findChildren<QLabel *>()) {
        if (lab->objectName() == QLatin1String("suggestGroupTitle"))
            ++groupTitles;
        if (lab->objectName() == QLatin1String("suggestDetail")) {
            if (lab->text() == QLatin1String("Tab"))
                ++historyBadges;
            else if (!lab->text().isEmpty())
                ++entityLabels;
        }
    }
    chk(groupTitles == 2, "dos grupos (Sugerencias + Tu historial)");
    chk(entityLabels == 2, "detalle de entidad en las 2 filas que lo traen");
    chk(historyBadges == 2, "insignia Tab en las 2 entradas de historial");

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
    chk(!panel->isHidden(), "con 14 sugerencias el panel se abre");
    chk(panel->height() <= page->kSuggestPanelMaxH + 8,
        "el alto del panel respeta el tope con 14 filas");
    chk(page->m_suggestScroll->height() == page->kSuggestPanelMaxH,
        "el area de filas se queda en el tope (sale la barra)");
    // Y la posicion no lo deja salirse de la pagina.
    chk(panel->y() >= 0 && panel->y() + panel->height() <= page->height() + 1,
        "el panel cabe dentro de la pagina");

    // Pocas filas: sin barra, alto = contenido.
    page->m_field->setText(QStringLiteral("Will"));
    page->m_suggestQuery = QStringLiteral("Will");
    page->onSuggestReady(QStringLiteral("Will"), items);
    chk(page->m_suggestScroll->height() < page->kSuggestPanelMaxH,
        "con pocas filas el alto es el del contenido (sin barra)");

    std::fprintf(stderr, "SUGGEST PANEL %s\n", fail == 0 ? "OK" : "FAIL");
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
    // depende de las dimensiones, y sin esto serian las por defecto.
    page.resize(1000, 700);
    return searchSuggestPanelSelfTest(&page);
}
