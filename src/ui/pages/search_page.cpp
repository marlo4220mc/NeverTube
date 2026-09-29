#include "search_page.h"

#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QPainterPath>
#include <QRegularExpression>

#include <utility>

#include "../../debug.h"
#include "../../db/repositories.h"
#include "../../models.h"
#include "../../yt.h"
#include "../channel_row.h"
#include "../format.h"
#include "../icons.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"
#include "../flow_layout.h"

// Canal con avatar arriba (132 x 114 px).

QPixmap circularAvatar(const QPixmap &pm, int size)
{
    if (pm.isNull())
        return QPixmap();
    const QPixmap scaled = pm.scaled(size, size, Qt::KeepAspectRatioByExpanding,
                                         Qt::SmoothTransformation);
    const QPixmap base = scaled.copy((scaled.width() - size) / 2,
                                      (scaled.height() - size) / 2, size, size);
    QPixmap out(size, size);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addEllipse(0, 0, size, size);
    p.setClipPath(clip);
    p.drawPixmap(0, 0, base);
    return out;
}

// Filtro de elision para 2 lineas: la primera línea lleva tanto texto
// como quepa; el resto va en la segunda (tambien recortado).
static QStringList elideTwoLines(const QFontMetricsF &fm,
                                  const QString &text, qreal width)
{
    QString first;
    for (const QChar &c : text) {
        const QString cand = first.isEmpty() ? QString(c) : first + c;
        if (fm.horizontalAdvance(cand) <= width)
            first = cand;
        else
            break;
    }
    QString second = text.mid(first.size());
    while (!second.isEmpty() && second.at(0) == QLatin1Char(' '))
        second.remove(0, 1);
    QStringList out;
    out << first;
    if (second.isEmpty()) {
        out << QString();
    } else {
        out << fm.elidedText(second, Qt::ElideRight, width);
    }
    return out;
}

class ChannelCard : public QWidget {
    Q_OBJECT
public:
    ChannelCard(ThumbnailCache *thumbs, const QString &channelId,
                const QString &name, const QString &subs,
                QWidget *parent = nullptr)
        : QWidget(parent)
        , m_thumbs(thumbs)
        , m_channelId(channelId)
        , m_name(name)
        , m_subs(subs)
    {
        setFixedSize(132, 114);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        connect(m_thumbs, &ThumbnailCache::ready,
                this, &ChannelCard::onThumbReady);
    }

    void setAvatarUrl(const QString &url) {
        if (url.isEmpty())
            return;
        const QPixmap cached = m_thumbs->pixmap(m_avatarKey);
        if (!cached.isNull()) {
            update();
        } else {
            m_thumbs->requestChannelAvatar(m_channelId, url);
        }
    }
    void setAvatarKey(const QString &key) { m_avatarKey = key; update(); }
    void setSubs(const QString &s) { m_subs = s; update(); }

    void setActivate(std::function<void()> fn) { m_activate = std::move(fn); }

signals:
    void activated();

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Fondo de la tarjeta
        p.setBrush(m_hover ? nt::kHover : nt::kPanel);
        p.setPen(QPen(nt::kBorder, 1));
        p.drawRoundedRect(QRectF(rect()).adjusted(0, 0, -1, -1), 8, 8);

        // Avatar circular (48 px)
        const qreal av = 48;
        const qreal ax = (width() - av) / 2;
        const qreal ay = 8;
        QRectF avR(ax, ay, av, av);
        const QPixmap pm = m_thumbs->pixmap(m_avatarKey);
        if (pm.isNull()) {
            p.setBrush(nt::kElevated);
            p.setPen(Qt::NoPen);
            p.drawEllipse(avR);
        } else {
            QPainterPath clip;
            clip.addEllipse(avR);
            p.save();
            p.setClipPath(clip);
            p.drawPixmap(avR.topLeft(),
                          pm.scaled(avR.size().toSize(),
                                    Qt::KeepAspectRatioByExpanding,
                                    Qt::SmoothTransformation));
            p.restore();
            p.setPen(QPen(nt::kBorder, 1));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(avR);
        }

        // Nombre (2 lineas, 13 px)
        p.setPen(nt::kText);
        QFont f = p.font();
        f.setPixelSize(13);
        p.setFont(f);
        const QFontMetricsF fm(f);
        const qreal nameW = width() - 20; // 10 px de margen cada lado
        const qreal nameX = (width() - nameW) / 2;
        const qreal nameY = ay + av + 6;
        const QStringList lines = elideTwoLines(fm, m_name, nameW);
        p.drawText(QRectF(nameX, nameY, nameW, 14),
                    Qt::AlignCenter, lines[0]);
        if (!lines[1].isEmpty()) {
            p.drawText(QRectF(nameX, nameY + 14, nameW, 14),
                        Qt::AlignCenter, lines[1]);
        }

        // Suscriptores (11 px, muted)
        p.setPen(nt::kMuted);
        f.setPixelSize(11);
        p.setFont(f);
        const qreal subsY = nameY + 28 + 2;
        p.drawText(QRectF(10, subsY, width() - 20, 14),
                    Qt::AlignCenter, m_subs);
    }

    void enterEvent(QEvent *) override { m_hover = true; update(); }
    void leaveEvent(QEvent *) override { m_hover = false; update(); }
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && m_activate)
            m_activate();
    }

private slots:
    void onThumbReady(const QString &key) {
        if (key != m_avatarKey)
            return;
        update();
    }

private:
    ThumbnailCache *m_thumbs;
    QString m_channelId;   // raw id (sin prefijo ch_)
    QString m_avatarKey;   // "ch_" + channelId
    QString m_name, m_subs;
    std::function<void()> m_activate;
    bool m_hover = false;
};


SearchPage::SearchPage(Yt *yt, SettingsRepo *settings, ThumbnailCache *thumbs,
                         QWidget *parent)
    : QWidget(parent)
    , m_yt(yt)
    , m_settings(settings)
    , m_thumbs(thumbs)
{
    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(600);
    connect(m_debounce, &QTimer::timeout, this, &SearchPage::doSearch);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(6);

    // Campo de busqueda 44 px con acciones internas (lupa + ✕)
    m_field = new QLineEdit(this);
    m_field->setPlaceholderText(QStringLiteral("Buscar en YouTube…"));
    m_field->setFixedHeight(44);
    QFont f = m_field->font();
    f.setPixelSize(15);
    m_field->setFont(f);
    QAction *searchAct = m_field->addAction(
        nt::icon(nt::Icon::Search, nt::kMuted, 18),
        QLineEdit::LeadingPosition);
    searchAct->setToolTip(QStringLiteral("Buscar"));
    m_clearAct = m_field->addAction(
        nt::icon(nt::Icon::Clear, nt::kMuted, 14),
        QLineEdit::TrailingPosition);
    m_clearAct->setToolTip(QStringLiteral("Borrar"));
    m_clearAct->setVisible(false);
    connect(m_clearAct, &QAction::triggered, this, [this]() {
        m_field->clear();
        m_field->setFocus();
    });
    connect(m_field, &QLineEdit::textChanged, this,
            [this](const QString &t) { m_clearAct->setVisible(!t.isEmpty()); });

    // Atajo "/" para enfocar el campo (ignora si ya se escribe en un campo)
    auto *slash = new QShortcut(QKeySequence(QStringLiteral("/")), this);
    connect(slash, &QShortcut::activated, this, [this]() {
        if (qobject_cast<QLineEdit *>(QApplication::focusWidget()))
            return;
        m_field->setFocus();
        m_field->selectAll();
    });

    // Historial reciente (chips con wrap)
    m_historyBox = new QWidget(this);
    auto *hBox = new QVBoxLayout(m_historyBox);
    hBox->setContentsMargins(0, 4, 0, 0);
    hBox->setSpacing(6);

    // Resultados
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
    m_list->setMetaLines(2);

    m_moreButton = new QPushButton(QStringLiteral("Mostrar más"), this);
    m_moreButton->setObjectName(QStringLiteral("ghost"));
    m_moreButton->setCursor(Qt::PointingHandCursor);
    m_moreButton->hide();
    m_moreButton->setFixedSize(170, 32);

    rBox->addWidget(spinner);
    rBox->addWidget(empty);
    rBox->addWidget(m_channelBox);
    rBox->addWidget(m_list, 1);
    rBox->addWidget(m_moreButton, 0, Qt::AlignHCenter);
    m_resultBox->hide();

    m_bottomStretch = new QSpacerItem(0, 0, QSizePolicy::Minimum,
                                          QSizePolicy::Expanding);
    outer->addWidget(m_field);
    outer->addWidget(m_historyBox);
    outer->addWidget(m_resultBox, 1);
    outer->addSpacerItem(m_bottomStretch);

    connect(m_field, &QLineEdit::textChanged, this,
            [this](const QString &) { m_debounce->start(); });
    connect(m_field, &QLineEdit::returnPressed, this, &SearchPage::doSearch);
    connect(searchAct, &QAction::triggered, this, &SearchPage::doSearch);
    connect(m_yt, &Yt::searchFinished, this, &SearchPage::onSearchReady);
    connect(m_yt, &Yt::channelInfoReady, this, &SearchPage::onChannelInfo);

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
    m_clearAct->setVisible(!query.trimmed().isEmpty());
    doSearch();
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
    m_debounce->stop();
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
    // Limpiar tarjetas de canales
    while (QLayoutItem *item = m_channelBox->layout()->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_channelCards.clear();
    m_channels.clear();
    m_channelBox->hide();
    if (m_emptyBox) {
        m_emptyBox->hide();
        m_emptyBox->deleteLater();
        m_emptyBox = nullptr;
    }
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

void SearchPage::onSearchReady(const QString &query,
                                      const QVector<VideoInfo> &videos)
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
        if (!m_emptyBox) {
            m_emptyBox = new QWidget(this);
            auto *lay = new QVBoxLayout(m_emptyBox);
            lay->setContentsMargins(0, 20, 0, 0);
            lay->setSpacing(10);
            auto *msg = new QLabel(
                QStringLiteral("Sin resultados para «%1»").arg(query),
                this);
            msg->setObjectName(QStringLiteral("emptyState"));
            msg->setAlignment(Qt::AlignCenter);
            lay->addWidget(msg);
            auto *flow = new FlowLayout(m_emptyBox, 0, 6, 6);
            for (const QString &s :
                 {query + QLatin1String(" música"),
                  query + QLatin1String(" tutoriales"),
                  QStringLiteral("música")}) {
                auto *chip = new QPushButton(s, m_emptyBox);
                chip->setObjectName(QStringLiteral("chip"));
                chip->setCursor(Qt::PointingHandCursor);
                connect(chip, &QPushButton::clicked, this,
                        [this, s]() { startSearch(s); });
                flow->addWidget(chip);
            }
            lay->addLayout(flow);
        }
        m_emptyBox->show();
        return;
    }
    if (m_emptyBox) {
        m_emptyBox->hide();
    }
    for (auto *child : m_resultBox->findChildren<QLabel *>()) {
        if (child->text().startsWith(QStringLiteral("Sin")))
            child->hide();
    }
    if (m_list->isEmpty()) {
        buildChannelCards();
        m_list->showVideos(m_results, VideoListView::ListMode);
    } else {
        m_list->appendVideos(videos);
    }
    m_moreButton->setVisible(videos.size() >= 20);
    m_moreButton->setEnabled(true);
}

void SearchPage::buildChannelCards()
{
    while (QLayoutItem *item = m_channelBox->layout()->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_channels.clear();
    m_channelCards.clear();
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
    title->setObjectName(QStringLiteral("sectionSub"));
    m_channelBox->layout()->addWidget(title);

    auto *wrap = new QWidget(m_channelBox);
    auto *flow = new FlowLayout(wrap, 0, 8, 8);
    wrap->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    for (const ChannelInfo &channel : std::as_const(m_channels)) {
        QString subs;
        if (channel.subscriberCount > 0)
            subs = QStringLiteral("%1 suscriptores")
                       .arg(compactCount(channel.subscriberCount));
        auto *card = new ChannelCard(m_thumbs, channel.id, channel.name,
                                      subs, wrap);
        card->setAvatarKey(QStringLiteral("ch_%1").arg(channel.id));
        card->setActivate(
            [this, channel]() { emit channelRequested(channel.id); });
        flow->addWidget(card);
        m_channelCards.insert(channel.id, card);
        m_yt->channelInfo(channel.id);
    }
    qobject_cast<QVBoxLayout *>(m_channelBox->layout())->addWidget(wrap, 0, Qt::AlignHCenter);
    m_channelBox->show();
}

void SearchPage::onChannelInfo(const ChannelInfo &channel)
{
    ChannelCard *card = m_channelCards.value(channel.id, nullptr);
    if (!card)
        return;
    if (!channel.avatarUrl.isEmpty())
        card->setAvatarUrl(channel.avatarUrl);
    if (channel.subscriberCount > 0) {
        QString subs = QStringLiteral("%1 suscriptores")
                           .arg(compactCount(channel.subscriberCount));
        card->setSubs(subs);
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

    auto *flowWidget = new QWidget(m_historyBox);
    auto *flow = new FlowLayout(flowWidget, 0, 6, 6);
    for (const QString &q : hist) {
        auto *row = new QWidget(m_historyBox);
        auto *rowLay = new QHBoxLayout(row);
        rowLay->setContentsMargins(0, 0, 0, 0);
        rowLay->setSpacing(4);
        auto *chip = new QPushButton(QStringLiteral("  %1  ").arg(q), row);
        chip->setObjectName(QStringLiteral("chip"));
        chip->setCursor(Qt::PointingHandCursor);
        auto *del = new QPushButton(row);
        del->setObjectName(QStringLiteral("iconBtn"));
        del->setIcon(nt::icon(nt::Icon::Clear, nt::kMuted, 10));
        del->setFixedSize(22, 22);
        del->setCursor(Qt::PointingHandCursor);
        del->setToolTip(QStringLiteral("Borrar esta búsqueda"));
        rowLay->addWidget(chip, 1);
        rowLay->addWidget(del);
        flow->addWidget(row);
        connect(chip, &QPushButton::clicked, this,
                [this, q]() { onHistoryQuery(q); });
        connect(del, &QPushButton::clicked, this,
                [this, q]() { onHistoryRemove(q); });
    }

    auto *clearAll = new QPushButton(QStringLiteral("Borrar historial"), this);
    clearAll->setObjectName(QStringLiteral("ghost"));
    clearAll->setCursor(Qt::PointingHandCursor);
    connect(clearAll, &QPushButton::clicked, this, &SearchPage::onHistoryClear);
    flow->addWidget(clearAll);

    m_historyBox->layout()->addWidget(flowWidget);
}

void SearchPage::onHistoryClear()
{
    QVector<QString> hist = history().toVector();
    hist.clear();
    setHistory(QStringList());
    showHistory();
}

void SearchPage::onHistoryRemove(const QString &query)
{
    QStringList hist = history();
    hist.removeAll(query);
    setHistory(hist);
    showHistory();
    m_field->setFocus();
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
    for (const QJsonValue &v : arr) {
        if (v.isString())
            list << v.toString();
    }
    return list;
}

void SearchPage::setHistory(const QStringList &queries)
{
    if (!m_settings)
        return;
    QJsonArray arr;
    for (const QString &q : queries) {
        if (!q.isEmpty())
            arr << q;
    }
    m_settings->set(QStringLiteral("search_history"),
                    QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

#include "search_page.moc"
