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
#include "../../i18n.h"
#include "../../models.h"
#include "../../yt.h"
#include "../../youtube_url.h"
#include "../channel_row.h"
#include "../format.h"
#include "../confirm_dialog.h"
#include "../icons.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

// --- Suggest dropdown row ---------------------------------------------------
//
// It is a QWidget and not a QPushButton for two reasons, both from the
// approved mockup:
//   1. bold on the typed span. Qt draws the text of a button with
//      QStyle::drawItemText, which does NOT parse rich text, so with a button
//      partial bold is impossible. A QLabel with Qt::RichText does give it.
//   2. the keyboard highlight has to be a state apart from the mouse hover, so
//      the arrow keys can move it without the mouse riding over it.
// Hover and highlight are resolved with QSS (#suggestRow:hover and
// #suggestRow[highlight="true"]), hence the WA_Hover: without it Qt does not
// emit the hover change on a plain QWidget.
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
    // Tab leaves the dropdown: it closes but focus continues wherever it should.
    void tabOutOf(SuggestRow *row);
    void focusedRow(SuggestRow *row);

protected:
    // Tab/Backtab are intercepted in event(), not in keyPressEvent:
    // QWidget::event() resolves them with focusNextPrevChild BEFORE calling
    // keyPressEvent, so a Qt::Key_Tab case in the switch would never run.
    bool event(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;

private:
    QString m_query;
    QLabel *m_text = nullptr;
};

namespace {
// The typed span goes bold. Elide BEFORE applying the bold: the cut only
// touches the end and the prefix is at the front, so bolding the first n
// characters of the already-elided text is still the correct prefix. If the
// whole text does not fit, the prefix is cut along with it and bolded in full,
// which is the only representable option.
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
    // The typed span goes lighter than the rest (kText over the row's
    // kTextSoft): that is the gesture of the mockup, to read the intent
    // without having paid for the request. The color comes from the theme
    // palette, not from a literal.
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
    // Essential on a QWidget that is not a QFrame: without WA_StyledBackground
    // the QSS "matches" but QWidget::paintEvent paints nothing, and the
    // hover/highlight background stays invisible (the QSS only paints on the
    // QFrame).
    setAttribute(Qt::WA_StyledBackground, true);
    setCursor(Qt::PointingHandCursor);
    setToolTip(query);
    setFixedHeight(SearchPage::kSuggestRowH);

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(10, 0, 10, 0);
    lay->setSpacing(10);

    // The row labels are transparent to the mouse. Not cosmetic: they are
    // CHILDREN of the row and cover almost its whole width (the text is what
    // gets clicked), so if they kept the click nobody could handle it and
    // pressing a suggestion would do nothing. The row is always the one that
    // receives the click.
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
    // NoWrap + fixed width: the text is elided, it does not wrap to two lines
    // (a 32 px row with two lines throws the whole panel off balance).
    m_text->setWordWrap(false);
    m_text->setFixedWidth(qMax(40, textW));
    m_text->setText(boldPrefix(query, typed, textW, fm));
    m_text->setToolTip(query);
    lay->addWidget(m_text, 0, Qt::AlignVCenter);
    // The leftover space goes HERE, right after the query: it is what pushes
    // the entity detail and the "Tab" badge to the right edge (in the mockup
    // it is a margin-left:auto). If the stretch were placed at the end they
    // would sit glued to the query, in the middle of the row.
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
        nt::bindTooltip(b, "search.tabHint");
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
    // A re-polish is required: changing a dynamic property does not repaint by
    // itself.
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
    // Tab and Backtab are handled HERE and not in keyPressEvent:
    // QWidget::event() deals with them before keyPressEvent is reached
    // (focusNextPrevChild), so a Qt::Key_Tab case in the switch would never
    // run. We signal to close the panel and let the base class move the focus;
    // the focus CONTINUES on its way, it does not return to the field as with
    // Esc.
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
        // Here the suggestion IS taken: focus is already inside the dropdown
        // (it was entered with Tab), which is what distinguishes this Enter
        // from the field's one, where the search is always for the literal
        // text.
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
    // Type-ahead: 300 ms. It is the only writing timer: it used to fire a
    // /search 600 ms after you stopped typing (the problem of searching in the
    // middle of a word), and now it only requests suggestions. The search
    // happens on Enter or the Buscar button, with the literal text.
    m_suggestDebounce = new QTimer(this);
    m_suggestDebounce->setSingleShot(true);
    m_suggestDebounce->setInterval(300);
    connect(m_suggestDebounce, &QTimer::timeout, this, [this]() {
        const QString q = m_field->text().trimmed();
        if (q.isEmpty() || q.size() < 2) {
            hideSuggestPanel();
            return;
        }
        // A pasted YouTube link is not a query: asking YouTube to autocomplete
        // an address makes no sense and its "suggestions" would open a panel of
        // noise over the field. Enter opens the video (see doSearch).
        if (!youtubeVideoIdFromText(q).isEmpty()) {
            hideSuggestPanel();
            return;
        }
        m_suggestQuery = q;
        m_yt->suggest(q);
    });

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(6);

    auto *title = new QLabel(this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    nt::bindText(title, "search.title");
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
    nt::bindPlaceholder(m_field, "search.placeholder");
    // 36 px, not 32: the global QSS applies padding 8px 12px, so with 32 the
    // vertical padding ate the text (8+8+line_height > 32). 36 leaves the 8+8
    // and a 13 px line comfortable.
    m_field->setFixedHeight(36);
    // Icon instead of the "✕" glyph (U+2715): that character is NOT in the
    // default font (Liberation Sans on Linux; on Windows it depends on the
    // font) and Qt painted a 2x5 px dot in its place, i.e. an almost invisible
    // button: you had to know it was there to press it. nt::icon() draws with
    // QPainter and does not depend on any font. The iconOnly property removes
    // the global padding, which otherwise leaves the icon no room inside the
    // 28 px.
    m_clearBtn = new QPushButton(searchCard);
    m_clearBtn->setObjectName(QStringLiteral("ghost"));
    m_clearBtn->setProperty("iconOnly", true);
    m_clearBtn->setIcon(nt::icon(nt::Icon::Clear, nt::kText, 16));
    m_clearBtn->setCursor(Qt::PointingHandCursor);
    m_clearBtn->setFixedSize(28, 28);
    m_clearBtn->setFocusPolicy(Qt::NoFocus);
    m_clearBtn->hide();
    auto *searchBtn = new QPushButton(searchCard);
    searchBtn->setObjectName(QStringLiteral("primary"));
    nt::bindText(searchBtn, "search.button");
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
        // Close the panel here rather than waiting for the debounce: otherwise,
        // after clearing the field, the previous text's suggestions stayed for
        // ~300 ms.
        m_suggestDebounce->stop();
        hideSuggestPanel();
        m_field->clear();
        // When clearing everything: back to history mode and clear the
        // results, as if nothing had ever been searched (that way the X button
        // "undoes" the search).
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

    // Floating suggestions panel. It is a child of the page (not of the
    // layout), so it reserves no space nor alters the layout: it is positioned
    // right under the field in showSuggestPanel(). Hidden by default.
    m_suggestPanel = new QFrame(this);
    m_suggestPanel->setObjectName(QStringLiteral("suggestPanel"));
    m_suggestPanel->setFrameShape(QFrame::NoFrame);
    m_suggestPanel->hide();
    {
        auto *sb = new QVBoxLayout(m_suggestPanel);
        sb->setContentsMargins(0, 0, 0, 0);
        sb->setSpacing(0);

        // The rows live in a QScrollArea with a capped height. The endpoint
        // returns up to 14 suggestions and the history may add more: without a
        // cap the panel grew downwards and ran off the page, covering the
        // results. With a cap its scrollbar shows and the dropdown never
        // exceeds kSuggestPanelMaxH.
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
    // Busy and empty share the #sectionSub look. They are MEMBERS, not locals:
    // the show/hide used to be done by matching their TRANSLATED text
    // ("Buscando" / "Sin..."), which only matched in Spanish — in English the
    // spinner was never hidden and the empty state never showed, and it broke on
    // a live language switch.
    m_busyLabel = new QLabel(this);
    m_busyLabel->setObjectName(QStringLiteral("sectionSub"));
    nt::bindText(m_busyLabel, "search.busy");
    m_busyLabel->setAlignment(Qt::AlignCenter);
    m_busyLabel->setContentsMargins(0, 40, 0, 0);
    m_busyLabel->hide();
    m_emptyLabel = new QLabel(this);
    m_emptyLabel->setObjectName(QStringLiteral("sectionSub"));
    nt::bindText(m_emptyLabel, "search.noResults");
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setContentsMargins(0, 40, 0, 0);
    m_emptyLabel->hide();
    m_channelBox = new QWidget(this);
    auto *channelLayout = new QVBoxLayout(m_channelBox);
    channelLayout->setContentsMargins(0, 0, 0, 0);
    channelLayout->setSpacing(2);
    m_channelBox->hide();
    m_list = new VideoListView(m_thumbs, this);
    m_list->showVideos(QVector<VideoInfo>(), VideoListView::ListMode);
    // Per-row options button, as on the rest of the row surfaces. The menu is
    // not built here: the page has no repos, so it asks for the video and the
    // window shows it (MainWindow::showVideoRowMenu).
    m_list->setRowMoreVisible(true);
    connect(m_list, &VideoListView::moreRequested, this,
            [this](int, const VideoInfo &v, const QPoint &pos) {
                emit rowMenuRequested(v, pos);
            });
    m_moreButton = new QPushButton(this);
    // Centered ghost, like the Channel page's "Cargar más vídeos": it used to
    // be #primary (red) with 180 px fixed, a wide bar under the list for an
    // action that only extended what was already visible.
    m_moreButton->setObjectName(QStringLiteral("ghost"));
    nt::bindText(m_moreButton, "search.more");
    m_moreButton->setCursor(Qt::PointingHandCursor);
    m_moreButton->hide();
    m_moreButton->setFixedHeight(34);
    connect(m_moreButton, &QPushButton::clicked, this, [this]() {
        m_moreButton->setEnabled(false);
        m_yt->search(m_activeQuery, 20, m_results.size());
    });
    rBox->addWidget(m_busyLabel);
    rBox->addWidget(m_emptyLabel);
    rBox->addWidget(m_channelBox);
    rBox->addWidget(m_list, 1);
    rBox->addWidget(m_moreButton, 0, Qt::AlignHCenter);
    outer->addWidget(m_resultBox, 1);
    m_resultBox->hide();
    m_bottomStretch = new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding);
    outer->addSpacerItem(m_bottomStretch);

    // Typing NO LONGER triggers a search: it only requests suggestions for the
    // dropdown. The search happens on Enter or the Buscar button and uses the
    // literal text. That way a half-typed word does not burn a /search.
    connect(m_field, &QLineEdit::textChanged, this, [this](const QString &) {
        // Typing again re-enables suggestions for the new text.
        m_suggestClosedFor.clear();
        m_suggestDebounce->start();
    });
    // Enter and the Buscar button ALWAYS search for the field's literal text,
    // without correcting it with the highlighted suggestion.
    connect(m_field, &QLineEdit::returnPressed, this, &SearchPage::doSearch);
    connect(searchBtn, &QPushButton::clicked, this, &SearchPage::doSearch);
    connect(m_yt, &Yt::searchFinished, this, &SearchPage::onSearchReady);
    connect(m_yt, &Yt::suggestReady, this, &SearchPage::onSuggestReady);
    connect(m_yt, &Yt::channelInfoReady, this, &SearchPage::onChannelInfo);

    // Any press outside the field and the panel closes it. Done with an
    // eventFilter on the application (not with editingFinished: pressing a row
    // fires editingFinished BEFORE the row's clicked, and the panel would
    // close before the suggestion could be read).
    m_app = qApp;
    if (m_app)
        m_app->installEventFilter(this);
    // Filter ALSO on the field, for the dropdown keyboard. It has to be a
    // filter, not a keyPressEvent of the page: between the field and the page
    // sits the "card" QFrame, and QWidget::event() resolves Tab/Backtab with
    // focusNextPrevChild() when keyPressEvent does not accept the key. That
    // focusNextPrevChild eats the Tab inside the QFrame and the page never
    // sees it (the "Tab then arrows" gesture did nothing). The application's
    // filter was already receiving these events; this one makes them explicit
    // and survives a change of the outer one.
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
    m_resultsMode = active;
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
    // Searching always closes the dropdown: the page switches to results mode.
    // The debounce is stopped too: if the user types fast and hits Enter
    // before the 300 ms, the suggestion request came out AFTERWARDS and the
    // panel reappeared on top of the results. And the query is marked as
    // already searched, so an in-flight response cannot reopen it either.
    m_suggestDebounce->stop();
    hideSuggestPanel();
    const QString q = m_field->text().trimmed();
    m_suggestClosedFor = q;
    if (q.isEmpty()) {
        setResultsMode(false);
        showHistory();
        return;
    }
    // A pasted YouTube link is a video, not a query: it is not searched and it
    // is not written in the history (a 60-character URL among the queries is
    // noise, and it would be added again on every opening). The PLAYER is
    // opened and what the history gets is the TITLE, which arrives later (see
    // applyVideoTitle). Everything that is not a link of YouTube falls through
    // to the normal search below.
    const QString videoId = youtubeVideoIdFromText(q);
    if (!videoId.isEmpty()) {
        m_pendingUrlId = videoId;
        m_pendingUrlText = q;
        // The results belong to the PREVIOUS query and none was run for this
        // text: back to the history view, which is what the page shows when
        // nothing is being searched.
        setResultsMode(false);
        m_searching = false;
        showHistory();
        NT_DEBUG("E2E searchUrl id=%s\n", videoId.toUtf8().constData());
        emit videoUrlRequested(videoId, q);
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
    if (m_busyLabel)
        m_busyLabel->show();
    if (m_emptyLabel)
        m_emptyLabel->hide();
    m_error.clear();

    QStringList hist = history();
    hist.removeAll(q);
    hist.prepend(q);
    while (hist.size() > 8)
        hist.removeLast();
    setHistory(hist);

    m_yt->search(q);
}

void SearchPage::applyVideoTitle(const QString &videoId, const QString &title)
{
    const QString clean = title.trimmed();
    if (clean.isEmpty() || m_pendingUrlId.isEmpty() || videoId != m_pendingUrlId)
        return;
    const QString url = m_pendingUrlText;
    m_pendingUrlId.clear();
    m_pendingUrlText.clear();
    // The field shows the NAME of what is being played, and only if the address
    // is still the text of the field: if the person typed something else while
    // the video was resolving, that text belongs to them.
    if (!url.isEmpty() && m_field->text().trimmed() == url) {
        const QSignalBlocker blocker(m_field);
        m_field->setText(clean);
        m_clearBtn->setVisible(true);
    }
    QStringList hist = history();
    hist.removeAll(clean);
    hist.prepend(clean);
    while (hist.size() > 8)
        hist.removeLast();
    setHistory(hist);
    // The history view is rebuilt NOW and not when coming back to the page,
    // which has no showEvent: without this the new title only appeared after
    // the next action that redrew it. Only in history mode, so that a list of
    // results is never covered by it.
    if (!m_resultsMode)
        showHistory();
    NT_DEBUG("E2E searchUrlTitle save=%s\n", clean.toUtf8().constData());
}

void SearchPage::onSearchReady(const QString &query, const QVector<VideoInfo> &videos)
{
    // The query must be the current one... and the field cannot be empty: if
    // the user presses X while this search is still in flight, the response
    // arrives with query == m_activeQuery and repaints the list that was just
    // emptied.
    if (query != m_activeQuery || m_field->text().trimmed().isEmpty())
        return;
    m_searching = false;
    NT_DEBUG("E2E searchReady count=%d\n", (int)videos.size());
    if (m_busyLabel)
        m_busyLabel->hide();
    if (m_results.isEmpty())
        m_results = videos;
    else
        m_results += videos;
    if (m_results.isEmpty()) {
        m_list->clearVideos();
        m_moreButton->hide();
        if (m_emptyLabel)
            m_emptyLabel->show();
        return;
    }
    if (m_emptyLabel)
        m_emptyLabel->hide();
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
    m_channelInfos.clear();
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
    auto *title = new QLabel(this);
    title->setObjectName(QStringLiteral("sectionTitle"));
    nt::bindText(title, "search.channels");
    m_channelBox->layout()->addWidget(title);
    auto *scroll = new QScrollArea(m_channelBox);
    scroll->setWidgetResizable(false);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    // The horizontal scrollbar lives inside the area and consumes viewport
    // height: the minimum must include that strip (as in the Library), or the
    // card will be clipped at the bottom.
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
    // The QScrollArea (widgetResizable=false) sizes the host from its layout;
    // without activating it/adjustSize the strip collapses to width 0 (the
    // same the Library does with updateSubsRowGeometry).
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
    m_channelInfos.insert(channel.id, channel);
    card->setTitle(channel.name);
    if (!channel.avatarUrl.isEmpty())
        card->setAvatarUrl(channel.avatarUrl);
    QString meta = channelHandleFor(channel.name);
    if (channel.subscriberCount > 0)
        meta = nt::tr("search.subscribers")
                   .arg(meta, compactCount(channel.subscriberCount));
    card->setMeta(meta);
    card->applyContentWidth();
    if (m_channelStrip && m_channelStrip->layout()) {
        m_channelStrip->layout()->invalidate();
        m_channelStrip->layout()->activate();
        m_channelStrip->adjustSize();
    }
}

// Called by MainWindow when the language changes. The static chrome ("Canales",
// "Recientes", "Borrar historial", the search field, the empty/busy labels) is
// live-bound; the channel cards carry a composed meta ("@handle · N
// suscriptores") that is re-applied from the stored ChannelInfo.
void SearchPage::retranslate()
{
    for (auto it = m_channelInfos.constBegin();
         it != m_channelInfos.constEnd(); ++it) {
        ChannelRow *card = m_channelButtons.value(it.key(), nullptr);
        if (!card)
            continue;
        const ChannelInfo &ch = it.value();
        QString meta = channelHandleFor(ch.name);
        if (ch.subscriberCount > 0)
            meta = nt::tr("search.subscribers")
                       .arg(meta, compactCount(ch.subscriberCount));
        card->setMeta(meta);
        card->applyContentWidth();
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
    // Section header: title on the left and the action on the RIGHT, in the
    // same row. It goes here because both things that were tried failed:
    //
    //  - #ghost centered at the foot of the list: rows have no box (the QSS
    //    gives them `background: transparent`), so the capsule was the only
    //    framed element of the block and read as a loose page control.
    //    Measured: its edges spanned x=709..924 with the rows at x=22..157.
    //  - #sectionLink at the foot, left-aligned: well placed but invisible. It
    //    is @MUTED@ at 12 px, i.e. the dullest element on the page for an
    //    action that is also destructive: the user did not see it.
    //
    // In the header the button is in view (where the title is already looked
    // at) and its row pair anchors it to the section, without looking like a
    // loose control.
    auto *head = new QWidget(m_historyBox);
    auto *headLay = new QHBoxLayout(head);
    headLay->setContentsMargins(0, 0, 0, 0);
    headLay->setSpacing(8);
    auto *hTitle = new QLabel(head);
    hTitle->setObjectName(QStringLiteral("sectionSub"));
    nt::bindText(hTitle, "search.recent");
    // "Borrar historial de búsquedas", not "Borrar historial": the app has two
    // histories (this one, of queries, and the one of watched videos in
    // Library) and alone the name did not say which one was deleted. The
    // dialog already said it in the body; now the button says it too. Still
    // shorter than the "historial de búsqueda" that nt::metaLine composes for
    // the rows.
    auto *clear = new QPushButton(head);
    clear->setObjectName(QStringLiteral("ghost"));
    nt::bindText(clear, "search.clearHistory");
    clear->setCursor(Qt::PointingHandCursor);
    // 30 and not 34: it is a header control, not the page's big button. The
    // global padding (7px 14px) plus a 13 px line fit in just right without
    // clipping.
    clear->setFixedHeight(30);
    connect(clear, &QPushButton::clicked, this, &SearchPage::onHistoryClear);
    headLay->addWidget(hTitle, 1);
    headLay->addWidget(clear, 0, Qt::AlignVCenter);
    m_historyBox->layout()->addWidget(head);

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
        nt::bindTooltip(del, "search.removeOne");
        del->setFixedSize(28, 28);
        del->setCursor(Qt::PointingHandCursor);
        connect(del, &QPushButton::clicked, this,
                [this, q]() { onHistoryRemove(q); });
        rowLay->addWidget(btn, 1);
        rowLay->addWidget(del);
        m_historyBox->layout()->addWidget(row);
    }
    if (auto *hLayout = qobject_cast<QBoxLayout *>(m_historyBox->layout()))
        hLayout->addStretch(1);
}

void SearchPage::onHistoryClear()
{
    // The app's own dialog (nt::confirm) and not QMessageBox: the system
    // message came out with the desktop theme, the last remaining trace of a
    // foreign look. Cancel comes focused and there is no default button, so a
    // stray Enter does not wipe the history.
    nt::confirm(this, nt::tr("search.clearHistory"),
                nt::tr("search.clearBody"),
                nt::tr("common.erase"), [this]() {
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

// --- Type-ahead: suggestions dropdown --------------------------------------
//
// It does not use QLineEdit's Highlight on purpose: Enter and the Buscar
// button search the LITERAL text, without correcting it with the pointed
// suggestion. The rows are plain QPushButton, like historyRow's, so the text
// can be left-aligned with the icon.

void SearchPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // isHidden() and not isVisible(): the panel's state is its own, it does
    // not depend on the page being visible (in a selftest, or when switching
    // tabs, the page may not be shown and isVisible() would return false).
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

// KEYS of the FIELD with the dropdown open. It goes in the field's
// eventFilter, not in SearchPage::keyPressEvent: the field is not a direct
// child of the page (it is inside the "card" QFrame) and for the Tab that
// breaks everything. QWidget::event() resolves Backtab/Tab with
// focusNextPrevChild() when keyPressEvent does not accept the key, and it does
// so in the intermediate QFrame BEFORE the event can rise to the page: the Tab
// was consumed moving focus to the Buscar button and the page never saw it.
// Intercepting in the field (the receiver) leaves nothing in between.
bool SearchPage::fieldKey(QKeyEvent *event)
{
    if (!m_suggestPanel || m_suggestPanel->isHidden())
        return false; // dropdown closed: the QLineEdit works normally
    switch (event->key()) {
    case Qt::Key_Down:
        // With the dropdown open, Down enters it. Down with no panel is still
        // the normal QLineEdit (field history, empty).
        moveHighlight(1);
        return true;
    case Qt::Key_Up:
        moveHighlight(-1);
        return true;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        // Tab jumps to the dropdown, as in the mockup. Shift+Tab too (which
        // arrives as Key_Backtab on X11), otherwise it would leave the user
        // with no way back.
        focusRow(m_highlight >= 0 ? m_highlight : 0);
        return true;
    case Qt::Key_Escape:
        // Closes WITHOUT clearing what was typed and returns focus to the field.
        hideSuggestPanel();
        m_field->setFocus(Qt::OtherFocusReason);
        return true;
    default:
        break;
    }
    // Enter, and everything else, passes to the QLineEdit. Note: the
    // highlight is NOT consulted here. Searching is searching what was typed.
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
    // With no highlight, the first arrow enters through the end it should:
    // Down via the first row, Up via the last.
    if (m_highlight < 0)
        next = delta > 0 ? 0 : m_rows.size() - 1;
    else if (next < 0)
        next = m_rows.size() - 1;
    else if (next >= m_rows.size())
        next = 0;
    setHighlight(next);
    // Moving with the arrows keeps focus in the field (it does not steal it
    // from the row), which is what whoever is typing expects.
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
    // Esc from inside the dropdown: closes it and returns focus to the field
    // without touching the typed text.
    hideSuggestPanel();
    m_field->setFocus(Qt::OtherFocusReason);
}

void SearchPage::onRowTabOut()
{
    // Tab leaves the dropdown. It only closes: focus is carried by the base
    // QWidget, which continues its normal traversal of widgets.
    hideSuggestPanel();
}

void SearchPage::onRowFocused(SuggestRow *row)
{
    // The highlight follows the real focus: entering with Tab shows which row
    // you are on, and returning to the field with Esc leaves it on the last
    // one.
    if (!row)
        return;
    const int idx = m_rows.indexOf(row);
    if (idx >= 0)
        setHighlight(idx);
}

bool SearchPage::eventFilter(QObject *watched, QEvent *event)
{
    // Field keyboard, resolved here (see fieldKey). It goes in the filter and
    // not in SearchPage::keyPressEvent because the field is inside the "card"
    // QFrame and that QFrame eats the Tab with focusNextPrevChild before the
    // key reaches the page.
    if (watched == m_field && event->type() == QEvent::KeyPress)
        return fieldKey(static_cast<QKeyEvent *>(event));

    if (event->type() == QEvent::MouseButtonPress && m_suggestPanel
        && !m_suggestPanel->isHidden()) {
        // The decision is NOT taken by the receiver (watched). A real press is
        // delivered FIRST to the top-level QWindow, which is not a QWidget:
        // with qobject_cast<QWidget*> it gave null, "neither in the field nor
        // in the panel" evaluated to false and the panel closed BEFORE the
        // press reached the row. Clicking a suggestion did nothing because the
        // panel was already gone when the event went down. The decision is
        // taken by POSITION, the same one Qt uses to dispatch the event, which
        // does not depend on who it is delivered to first.
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
            continue; // do not suggest what is already typed
        if (q.startsWith(prefix, Qt::CaseInsensitive))
            out << q;
    }
    return out;
}

void SearchPage::onSuggestReady(const QString &query,
                                const QVector<Suggestion> &suggestions)
{
    // Late response: the field is no longer the prefix that was requested, or
    // that query was already searched (Enter): the panel does not reopen.
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
    // Widths: the detail is clipped so the query wins (without a cap, a long
    // "Cantante y compositor peruano" would push the text out of the panel).
    // The badge is measured and the rest goes to the query.
    const int badgeW = 34;
    const bool anyDetail = [&] {
        for (const Suggestion &s : m_suggestItems)
            if (!s.detail.isEmpty())
                return true;
        return false;
    }();
    const int detailW = anyDetail ? qMax(60, panelW / 3) : 0;
    const int textW = qMax(80, panelW - 30 - badgeW - (detailW ? detailW + 12 : 0));

    // Group divider. The mockup only labels the SECOND group ("Tus búsquedas",
    // in capitals and with a line next to it) and leaves the first one without
    // a header by implication, so it is copied as-is: no title for YouTube, a
    // divider for the history.
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

    // Group 1: YouTube suggestions (with the entity detail if it comes).
    // No header, as in the mockup.
    for (const Suggestion &s : std::as_const(m_suggestItems))
        addRow(s.query, s.detail, QString());
    // Group 2: what the user has already searched, with the "Tab" badge that
    // distinguishes it and which is the key that takes it.
    if (!m_historyRows.isEmpty()) {
        addDivider(nt::tr("search.suggestHistory"));
        for (const QString &q : m_historyRows)
            addRow(q, QString(), QStringLiteral("Tab"));
    }
}

void SearchPage::showSuggestPanel()
{
    if (!m_suggestPanel)
        return;
    // The content height rules, with a cap: the dropdown never covers the
    // whole page, however many rows the endpoint brings.
    //
    // ORDER MATTERS: it is SHOWN first and measured afterwards.
    // QLayoutItem::isEmpty() returns true for hidden widgets, so while the
    // panel is hidden the layout reads as EMPTY and sizeHint() gives 0 (or an
    // invalid QSize of -1, and qMin(-1, cap) would give a NEGATIVE height).
    // With 14 rows that left the dropdown at height 0 and, with no scrollbar,
    // the last 4 rows were unreachable.
    m_suggestPanel->show();
    m_suggestPanel->raise();
    if (QLayout *lay = m_suggestRowsBox->layout())
        lay->activate();
    int contentH = m_suggestRowsBox->sizeHint().height();
    if (contentH <= 0) {
        // The layout still says nothing: it is computed by hand with the rows
        // in place, which is exact because SuggestRow has a fixed height.
        contentH = m_rows.size() * kSuggestRowH;
        if (!m_historyRows.isEmpty() && !m_suggestItems.isEmpty())
            contentH += kSuggestDividerH; // there is a divider between groups
    }
    const int scrollH = qBound(kSuggestRowH,
                               qMin(contentH, kSuggestPanelMaxH),
                               kSuggestPanelMaxH);
    // The minimum of the rows area is set BY HAND to the content height,
    // instead of trusting the layout's minimumSizeHint(). With
    // widgetResizable(true) the QScrollArea decides the child's height from
    // the layout's sizeHint, and with 14 rows it stayed at the viewport
    // height: no scrollbar and the last 4 rows unreachable. With the explicit
    // minimum the child measures what the rows measure and the scrollbar
    // appears when it does not fit.
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
    // Right under the field, at the same width. mapTo(this) for page
    // coordinates (the panel is a child of the page, not of the layout).
    const QPoint belowLeft =
        m_field->mapTo(this, QPoint(0, m_field->height() + 4));
    int x = belowLeft.x();
    int y = belowLeft.y();
    // If it does not fit below the field, it opens UPWARDS glued to its
    // bottom edge instead of running off the page.
    if (y + m_suggestPanel->height() > height()) {
        const QPoint aboveRight =
            m_field->mapTo(this, QPoint(m_field->width(), -4));
        const int yAbove = aboveRight.y() - m_suggestPanel->height();
        if (yAbove >= 0) {
            y = yAbove;
            x = aboveRight.x() - m_suggestPanel->width();
        } else {
            // Neither above nor below: it is clipped to the available height.
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
    // The rows are destroyed with the layout on the next populate, but the
    // highlight index must be dropped NOW: otherwise a later setHighlight
    // would touch an already-deleted widget.
    m_rows.clear();
    m_highlight = -1;
}

void SearchPage::acceptSuggestion(const QString &query)
{
    // Put the suggestion's text in and search it. The text is the row's (not
    // the highlighting), and it is searched explicitly.
    const QSignalBlocker blocker(m_field);
    m_field->setText(query);
    m_clearBtn->setVisible(!query.trimmed().isEmpty());
    doSearch();
}

// OFFLINE selftest of the dropdown (no network): checks what network tests do
// not show, which is the panel construction itself.
//   1) it opens with two groups: YouTube suggestions and your history;
//   2) the history is filtered by prefix and does NOT include what is already
//      typed;
//   3) rows carry the entity detail when it comes;
//   4) a response for a prefix that is no longer the field's is ignored
//      (late response while typing on);
//   5) an empty list closes the panel instead of opening it blank.
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

    // History seed: two entries starting with "Will" and one that does not.
    page->setHistory({QStringLiteral("Will smith"), QStringLiteral("Willie colon"),
                      QStringLiteral("receta de tarta")});

    // The field has to match the response's prefix or the slot discards it
    // (late-response guard). It is written first.
    page->m_field->setText(QStringLiteral("Will"));
    chk(page->m_field->text() == QStringLiteral("Will"),
        "el campo de prueba contiene el prefijo");

    QVector<Suggestion> items;
    items.append({QStringLiteral("willie colón"),
                  QStringLiteral("Cantautor y trombonista")});
    items.append({QStringLiteral("willyrex"), QStringLiteral("@Willyrex")});
    items.append({QStringLiteral("william luna"), QString()});
    // m_suggestQuery is set by hand: it is what the debounce would do when
    // requesting.
    page->m_suggestQuery = QStringLiteral("Will");
    page->onSuggestReady(QStringLiteral("Will"), items);

    // isHidden() and not isVisible(): the page is not shown in the selftest,
    // so isVisible() would return false even with the panel open.
    chk(!panel->isHidden(), "el panel se abre con sugerencias");

    // Without WA_StyledBackground the QSS matches but QWidget::paintEvent
    // paints nothing: the hover and highlight backgrounds stay invisible. It
    // is a fault you cannot see by reading the row text, only by looking at
    // the screen, so the attribute is checked directly.
    chk(!page->m_rows.isEmpty()
            && page->m_rows.first()->testAttribute(Qt::WA_StyledBackground),
        "la fila tiene WA_StyledBackground (si no, el QSS no se pinta)");
    // Panel structure: 3 YouTube suggestions + 2 from history, a single
    // divider ("Tus busquedas", which labels only the second group as in the
    // mockup) and the Tab badge on the 2 history rows.
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
    // The detail and the badge go to the RIGHT EDGE of the row (the mockup's
    // margin-left:auto), not glued to the query. It depends on addStretch(1)
    // coming right after the text label: if it were placed at the end of the
    // row they would sit in the middle, which is how it was and cannot be seen
    // by reading the text.
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
    // The divider goes BETWEEN the two groups, not at the beginning: the 4th
    // row is the first history one, so the divider is index 3.
    {
        const QVector<SuggestRow *> &r = page->m_rows;
        chk(r.at(0)->query() == QStringLiteral("willie colón")
                && r.at(2)->query() == QStringLiteral("william luna"),
            "las 3 primeras filas son las de YouTube, en orden");
        chk(r.at(3)->query() == QStringLiteral("Will smith")
                && r.at(4)->query() == QStringLiteral("Willie colon"),
            "las 2 ultimas son las del historial filtrado");
    }
    // The prefix bold: the mockup puts it on the typed span, and it is what
    // makes the difference between typing right and typing wrong visible
    // without having spent a request.
    {
        QLabel *txt = page->m_rows.at(0)->findChild<QLabel *>(
            QStringLiteral("suggestQuery"));
        const QString rich = txt ? txt->text() : QString();
        chk(txt && txt->textFormat() == Qt::RichText,
            "el texto de la fila es rich text (permite la negrita)");
        // The query is "willie colón" and the prefix "Will": the bold covers
        // the first 4 characters ("will") and the rest stays outside.
        chk(rich.contains(QLatin1String(">will</span>ie col")),
            "solo el prefijo escrito va en negrita");
    }

    // The filtered history: "Will smith" and "Willie colon" start with Will;
    // "receta de tarta" does not and is left out.
    chk(page->m_historyRows.size() == 2, "historial filtrado por prefijo (2 de 3)");

    // Prefix matching NOTHING: neither suggestions nor history start with it
    // ("zzz" matches none of the three seeded entries). The panel must close
    // instead of opening blank.
    page->m_field->setText(QStringLiteral("zzz"));
    page->m_suggestQuery = QStringLiteral("zzz");
    page->onSuggestReady(QStringLiteral("zzz"), QVector<Suggestion>());
    chk(panel->isHidden(), "sin sugerencias ni historial el panel se cierra");

    // Prefix that only matches the history ("Willi" -> "Willie colon"): the
    // panel must open with THAT group and without the YouTube one. It checks
    // that the two groups are independent and do not mix.
    page->m_field->setText(QStringLiteral("Willi"));
    page->m_suggestQuery = QStringLiteral("Willi");
    page->onSuggestReady(QStringLiteral("Willi"), QVector<Suggestion>());
    chk(!panel->isHidden(), "solo historial tambien abre el panel");
    chk(page->m_historyRows.size() == 1,
        "el historial solo ofrece lo que casa con el prefijo");

    // Late response: it arrives for "Will" when the field is already "Willi".
    // The slot ignores it (query != m_suggestQuery) and does not change the
    // panel, which keeps what it had: 1 history row, not the 3 suggestions.
    page->onSuggestReady(QStringLiteral("Will"), items);
    chk(page->m_historyRows.size() == 1,
        "respuesta tardia no pisa el estado del panel");

    // And an UP-TO-DATE response (query == field == m_suggestQuery) does change it.
    page->m_field->setText(QStringLiteral("Will"));
    page->m_suggestQuery = QStringLiteral("Will");
    page->onSuggestReady(QStringLiteral("Will"), items);
    chk(!panel->isHidden(), "una respuesta al dia vuelve a abrir el panel");
    chk(page->m_suggestItems.size() == 3, "llegaron las 3 sugerencias de YouTube");

    // Height cap: with many rows (the 14 the endpoint returns, or more if the
    // history adds up) the panel must NOT grow without limit, but show a
    // scrollbar. It is what keeps the dropdown from covering the whole page.
    QVector<Suggestion> many;
    for (int i = 0; i < 14; ++i)
        many.append({QStringLiteral("sugerencia de prueba numero %1").arg(i),
                     QStringLiteral("un detalle bastante largo para la fila %1")
                         .arg(i)});
    page->m_field->setText(QStringLiteral("sug"));
    page->m_suggestQuery = QStringLiteral("sug");
    page->onSuggestReady(QStringLiteral("sug"), many);
    // setFixedHeight does not resize instantly: it queues a LayoutRequest that
    // the event loop will apply. In the app it is not noticeable (it happens
    // before the next paint), but to read the scrollbar range it must be forced.
    QCoreApplication::processEvents();
    chk(!panel->isHidden(), "con 14 sugerencias el panel se abre");
    chk(panel->height() <= page->kSuggestPanelMaxH + 8,
        "el alto del panel respeta el tope con 14 filas");
    chk(page->m_suggestScroll->height() <= page->kSuggestPanelMaxH,
        "el area de filas no pasa del tope");
    // And with 14 rows of 32 px the content does NOT fit: there has to be a
    // scrollbar, which is what keeps the panel from growing over the page and
    // leaving the last 4 rows unreachable.
    chk(page->m_suggestScroll->verticalScrollBar()->maximum() > 0,
        "con 14 filas sale la barra (el contenido excede el area)");
    // And the position does not let it run off the page.
    chk(panel->y() >= 0 && panel->y() + panel->height() <= page->height() + 1,
        "el panel cabe dentro de la pagina");

    // Few rows: no scrollbar, height = content.
    page->m_field->setText(QStringLiteral("Will"));
    page->m_suggestQuery = QStringLiteral("Will");
    page->onSuggestReady(QStringLiteral("Will"), items);
    chk(page->m_suggestScroll->height() < page->kSuggestPanelMaxH,
        "con pocas filas el alto es el del contenido (sin barra)");

    // --- Dropdown keyboard --------------------------------------------------
    // NOTE: it is delivered with qApp->notify() to the FIELD, not by calling
    // SearchPage::keyPressEvent() directly. The field is not a direct child of
    // the page (it is inside a QFrame, the search "card"), and for the Tab
    // that matters: QWidget::event() calls focusNextPrevChild() when
    // keyPressEvent does not accept the key, and it does so BEFORE the event
    // can rise to the page. Calling the handler by hand skipped exactly the
    // stretch where the key was lost, and the test passed with the gesture
    // broken in the app.
    const auto press = [page](int key) {
        QKeyEvent ev(QEvent::KeyPress, key, Qt::NoModifier);
        qApp->notify(page->m_field, &ev);
    };
    const auto pressRow = [](SuggestRow *row, int key) {
        QKeyEvent ev(QEvent::KeyPress, key, Qt::NoModifier);
        qApp->notify(row, &ev);
    };
    // When repopulating, the highlight is reset: the rows are new and the old
    // index would point to an already-deleted widget.
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

    // The highlight shows on the row (QSS property) and only on one.
    {
        int on = 0;
        for (SuggestRow *r : page->m_rows)
            if (r->property("highlight").toBool())
                ++on;
        chk(on == 1, "el resaltado se marca en exactamente una fila");
        chk(page->m_rows.at(0)->property("highlight").toBool(),
            "la fila resaltada es la primera");
    }

    // HARD RULE: moving the highlight does NOT touch the field. Enter in the
    // field searches the literal text, never the highlighted suggestion.
    chk(page->m_field->text() == QLatin1String("Will"),
        "resaltar una fila no sustituye el texto del campo");

    // Esc closes the dropdown WITHOUT clearing what was typed.
    press(Qt::Key_Escape);
    chk(panel->isHidden(), "Esc cierra el desplegable");
    chk(page->m_field->text() == QLatin1String("Will"),
        "Esc no borra lo escrito");
    chk(page->m_highlight == -1, "Esc suelta el resaltado");

    // With the panel closed, the arrows and Esc are NOT intercepted (they pass
    // to the QLineEdit): otherwise they would eat the field's keys.
    chk(page->m_highlight == -1, "con el panel cerrado las flechas no resaltan");

    // Tab enters the dropdown. With the panel closed, Tab is not touched: the
    // field uses it for its normal focus traversal.
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

    // Down from inside the row moves the real focus between rows.
    pressRow(page->m_rows.at(0), Qt::Key_Down);
    chk(QApplication::focusWidget() == page->m_rows.at(1),
        "Abajo dentro del desplegable mueve el foco a la fila siguiente");

    // Esc from inside the row returns focus to the field.
    pressRow(page->m_rows.at(1), Qt::Key_Escape);
    chk(panel->isHidden() && QApplication::focusWidget() == page->m_field,
        "Esc desde una fila cierra y devuelve el foco al campo");

    // Tab from inside the row closes the dropdown and lets focus continue
    // (it does not return it to the field like Esc).
    page->m_field->setText(QStringLiteral("Will"));
    page->m_suggestQuery = QStringLiteral("Will");
    page->onSuggestReady(QStringLiteral("Will"), items);
    page->focusRow(1);
    chk(QApplication::focusWidget() == page->m_rows.at(1), "focusRow mueve el foco");
    pressRow(page->m_rows.at(1), Qt::Key_Tab);
    chk(panel->isHidden(), "Tab desde una fila cierra el desplegable");
    chk(QApplication::focusWidget() != page->m_field,
        "Tab desde una fila NO devuelve el foco al campo");

    // --- Mouse click -------------------------------------------------------
    // What decides the click is not how the event propagates, but WHO it is
    // delivered to by the platform: QApplication::widgetAt() says that, which
    // is the same hit-test the window server does. The labels (icon, query,
    // detail, badge) are CHILDREN of the row and cover almost its whole width:
    // without WA_TransparentForMouseEvents the point the user clicks lands on
    // the label and not on the row.
    page->m_field->setText(QStringLiteral("Will"));
    page->m_suggestQuery = QStringLiteral("Will");
    page->onSuggestReady(QStringLiteral("Will"), items);
    {
        SuggestRow *row = page->m_rows.at(1);
        // NOTE: it has to be the row ITSELF, not "the row or something of
        // its". With isAncestorOf() the check would pass even if the hit-test
        // fell on a label, which is exactly the fault.
        const auto hitIsRow = [row](QWidget *child) {
            const QPoint g = child->mapToGlobal(child->rect().center());
            return QApplication::widgetAt(g) == row;
        };
        chk(hitIsRow(row->findChild<QLabel *>(QStringLiteral("suggestQuery"))),
            "el hit-test del texto de la fila cae en la fila, no en la etiqueta");
        chk(hitIsRow(row->findChild<QLabel *>(QStringLiteral("suggestDetail"))),
            "el hit-test del detalle cae en la fila, no en la etiqueta");
        // And the underlying invariant, for the labels the hit-test above
        // cannot cover (the badge falls outside the window in the offscreen
        // run): NO label of the row may keep the mouse. Row 3 is the history
        // one and carries the "Tab" badge.
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

    // --- X button: clearing the field must UNDO the search ---------------
    // The X only did m_field->clear(), so the previous query's results stayed
    // on screen with the field already empty.
    {
        page->m_field->setText(QStringLiteral("Will smith"));
        page->m_activeQuery = QStringLiteral("Will smith");
        page->m_searching = true;
        page->m_results = { VideoInfo{ QStringLiteral("vid-1") } };
        page->m_list->showVideos(page->m_results, VideoListView::ListMode);
        page->setResultsMode(true);
        chk(page->m_resultBox->isVisible() && !page->m_historyBox->isVisible(),
            "con resultados se ve la caja de resultados, no el historial");

        // A real click on the button, not calling the lambda by hand.
        page->m_clearBtn->click();
        chk(page->m_field->text().isEmpty(), "el X vacia el campo");
        chk(page->m_results.isEmpty() && page->m_list->isEmpty(),
            "el X borra los resultados de la lista");
        chk(!page->m_resultBox->isVisible() && page->m_historyBox->isVisible(),
            "el X vuelve al modo historial (como si no se hubiera buscado)");
        chk(page->m_activeQuery.isEmpty() && !page->m_searching,
            "el X invalida la busqueda en vuelo");

        // And the late one: with the X already pressed, the previous query's
        // response must not repaint the list that was just emptied.
        page->onSearchReady(QStringLiteral("Will smith"),
                            { VideoInfo{ QStringLiteral("vid-tarde") } });
        chk(page->m_list->isEmpty() && page->m_results.isEmpty(),
            "una respuesta tardia no repinta la lista tras borrar con X");
    }

    std::fprintf(stderr, "SUGGEST PANEL %s\n", fail == 0 ? "OK" : "FAIL");
    return fail == 0;
}

// Test bench for the dropdown in the REAL HIERARCHY. The selftest above mounts
// the SearchPage loose as a top-level window, and there gestures were lost
// without it being noticed: the field is inside a QFrame, the page inside a
// QStackedWidget, and the one doing the deployment is the pixel. Here the page
// is mounted inside a container with a navigation bar (the shape it has in
// MainWindow), the real QSS is applied, and ALL the gestures are repeated with
// real delivery (qApp->notify to the receiving widget, widgetAt for the
// click).
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

    // The real QSS. Without it the widgets come out unstyled and the hit-test
    // looks nothing like the app (the selftest above does not install it
    // either).
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
    // Delivery through the real path: it is sent to the widget that receives
    // the event, not to the handler. Calling the handler by hand is what made
    // the test pass with the gesture broken.
    const auto key = [](QWidget *w, int k) {
        QKeyEvent ev(QEvent::KeyPress, k, Qt::NoModifier);
        qApp->notify(w, &ev);
        QCoreApplication::processEvents();
    };
    const auto clic = [](QWidget *w) {
        // Real GLOBAL position: the filter decides by globalPos(), and with the
        // short QMouseEvent constructor the global ended up being the row's
        // local (coordinates of another system), so the test measured something
        // that does not happen in the app.
        const QPoint local = w->rect().center();
        const QPoint global = w->mapToGlobal(local);
        QMouseEvent ev(QEvent::MouseButtonPress, QPointF(local), QPointF(global),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        qApp->notify(w, &ev);
        QCoreApplication::processEvents();
    };

    // Enter fires the search and doSearch() hits the network. Here that
    // connection is dropped and a probe is installed: what is checked is that
    // the key REACHES the QLineEdit and with what text, not the search result
    // (that is another test). Without this the test would end with a network
    // request in flight.
    QString textoEnEnter;
    bool entroEnter = false;
    QObject::disconnect(page->m_field, &QLineEdit::returnPressed, page,
                        &SearchPage::doSearch);
    QObject::connect(page->m_field, &QLineEdit::returnPressed, page,
                     [&textoEnEnter, &entroEnter, page] {
                         entroEnter = true;
                         textoEnEnter = page->m_field->text();
                     });

    // --- 1) Click: to whom the platform delivers it, not how it propagates --
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

    // --- 1b) The press arrives FIRST to the QWindow -------------------------
    // In the app, a real click is delivered above all to the top-level
    // QWindow, which is NOT a QWidget. Deciding by the receiver made the panel
    // close right there (qobject_cast<QWidget*> gave null -> "outside the
    // panel and the field") and the click never reached the row. The decision
    // has to be by POSITION, not by who receives.
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
        // And outside the panel and the field it DOES have to close it.
        const QPoint fuera =
            page->mapToGlobal(QPoint(page->width() / 2, page->height() - 20));
        QMouseEvent ev2(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(fuera),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        qApp->notify(win, &ev2);
        chk(panel->isHidden(), "una pulsacion fuera del panel y del campo si lo cierra");
        // The field also keeps it open.
        abrir(QStringLiteral("Will"));
        const QPoint enCampo = page->m_field->mapToGlobal(QPoint(4, 4));
        QMouseEvent ev3(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(enCampo),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        qApp->notify(win, &ev3);
        chk(!panel->isHidden(), "una pulsacion sobre el campo no cierra el panel");
    }

    // --- 2) Tab enters the dropdown (the QFrame bug) -----------------------
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

    // --- 3) Tab inside the dropdown leaves without returning focus ---------
    abrir(QStringLiteral("Will"));
    key(page->m_field, Qt::Key_Tab);
    key(page->m_rows.at(0), Qt::Key_Tab);
    chk(panel->isHidden(), "Tab en una fila cierra el desplegable");
    chk(QApplication::focusWidget() != page->m_field,
        "Tab en una fila NO devuelve el foco al campo");

    // --- 4) Keyboard with focus in the field -------------------------------
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

    // --- 5) Click outside and tab switch -----------------------------------
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

    // --- 6) The dropdown anchors to the field and never leaves the page -----
    // Narrowing the window until it fits neither above nor below has to clip
    // it, not run off. NOTE: the "opens upwards" branch of
    // positionSuggestPanel() cannot be covered here, and it is not a failure
    // of the test: the field is always about 46 px from the top edge, so
    // yAbove = 46 - panel_height is negative for any panel taller than 42 px.
    // That is, with this layout that branch never runs; what does run is the
    // clipping below.
    const QVector<QSize> tamanos = {QSize(1400, 900), QSize(1100, 720),
                                    QSize(900, 520), QSize(820, 400),
                                    QSize(700, 300), QSize(1100, 240)};
    int fuera = 0;
    int desalineado = 0;
    int recortado = 0;
    int altoNatural = 0; // height with room to spare, to detect the clipping
    for (const QSize &s : tamanos) {
        // The WINDOW is resized, not the QStackedWidget: the root layout
        // rules over it, and resize() over the stack changes nothing (the page
        // stays at its size and this test tested nothing).
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
        // Detect the clipping by comparing against the natural height. Note:
        // once clipped the panel DOES fit (that is what clipping is for), so
        // checking whether it runs off the page is no way to know it happened.
        if (altoNatural == 0) {
            altoNatural = pr.height();
        } else if (pr.height() < altoNatural - 2) {
            ++recortado;
            if (page->m_suggestScroll->verticalScrollBar()->maximum() <= 0)
                ++fuera; // clipped with no scrollbar: the bottom rows are unreachable
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
    // Builds the minimum pieces SearchPage needs and runs the panel test. The
    // DB goes to a temporary file so the user's one is not touched. Everything
    // is local (NOT static): a static would be destroyed at exit(), when Qt is
    // already half closed, and AppDatabase::~AppDatabase()->close() ->
    // QSqlDatabase::removeDatabase() crashes there.
    AppDatabase db(QStringLiteral("/tmp/nevertube_selftest_suggest.db"));
    if (!db.isOpen() && !db.open()) {
        std::fprintf(stderr, "SUGGEST PANEL FAIL: no se abre la BD de test\n");
        return false;
    }
    SettingsRepo settings(db);
    ThumbnailCache thumbs(QStringLiteral("suggest-selftest"), 1 << 20);
    Yt yt;
    SearchPage page(&yt, &settings, &thumbs);
    // Explicit size: the check that the panel fits on the page depends on the
    // dimensions, and without this they would be the defaults. It is shown
    // because the keyboard cases check the real focus, and a widget that is
    // not shown cannot have focus.
    page.resize(1000, 700);
    page.show();
    QCoreApplication::processEvents();
    return searchSuggestPanelSelfTest(&page);
}

// OFFLINE selftest of a YouTube link pasted in the search box (no network):
// the link path returns BEFORE m_yt->search, so nothing is asked. It checks the
// three things the feature promises:
//   1) the pasted address opens the video (with the id, and the text as is);
//   2) the address is NOT written in the search history;
//   3) the TITLE is, replacing the address in the field, and only when it is
//      the video that was opened.
bool searchUrlSelfTest(SearchPage *page)
{
    int fail = 0;
    const auto chk = [&fail](bool ok, const char *what) {
        std::fprintf(stderr, "  [%s] %s\n", ok ? "OK  " : "FAIL", what);
        if (!ok)
            ++fail;
    };
    if (!page)
        return false;
    page->setHistory({QStringLiteral("gato")});

    QString gotId;
    QString gotUrl;
    const QMetaObject::Connection conn = QObject::connect(
        page, &SearchPage::videoUrlRequested,
        [&gotId, &gotUrl](const QString &id, const QString &url) {
            gotId = id;
            gotUrl = url;
        });

    const QString link = QStringLiteral("https://youtu.be/dQw4w9WgXcQ?t=10");
    page->m_field->setText(link);
    page->doSearch();
    chk(gotId == QLatin1String("dQw4w9WgXcQ"), "el enlace pegado abre el video");
    chk(gotUrl == link, "la peticion lleva el enlace tal cual se pego");
    chk(page->history() == QStringList{QStringLiteral("gato")},
        "el enlace NO se guarda en el historial");
    chk(!page->m_resultsMode, "la pagina no se queda en modo resultados");

    // The title arrives from the player.
    const QString title = QStringLiteral("Never Gonna Give You Up");
    page->applyVideoTitle(QStringLiteral("dQw4w9WgXcQ"), title);
    const QStringList afterTitle = page->history();
    chk(afterTitle.size() == 2 && afterTitle.at(0) == title,
        "el titulo queda el primero del historial");
    chk(!afterTitle.contains(link), "la URL no aparece en el historial");
    chk(page->m_field->text() == title, "el campo muestra el titulo");

    // The link is opened again; a title from ANOTHER video must not touch
    // either the history or the field.
    page->m_field->setText(link);
    page->doSearch();
    page->applyVideoTitle(QStringLiteral("otroVideo01"), QStringLiteral("Otro"));
    chk(page->history() == afterTitle, "un titulo de otro video no toca el historial");
    chk(page->m_field->text() == link, "ni cambia lo que hay en el campo");

    // If the video never resolves, nothing of the link is left behind.
    chk(page->history() == afterTitle,
        "sin titulo el enlace NO se guarda (ni la URL ni el id)");

    QObject::disconnect(conn);
    return fail == 0;
}

bool searchUrlSelfTestRun()
{
    AppDatabase db(QStringLiteral("/tmp/nevertube_selftest_searchurl.db"));
    if (!db.isOpen() && !db.open()) {
        std::fprintf(stderr, "SEARCH URL FAIL: no se abre la BD de test\n");
        return false;
    }
    SettingsRepo settings(db);
    ThumbnailCache thumbs(QStringLiteral("searchurl-selftest"), 1 << 20);
    Yt yt;
    SearchPage page(&yt, &settings, &thumbs);
    page.resize(1000, 700);
    page.show();
    QCoreApplication::processEvents();
    return searchUrlSelfTest(&page);
}

bool searchSuggestGesturesSelfTestRun()
{
    // Mounts the page where it really lives: inside a QStackedWidget, with a
    // navigation bar on top, as in MainWindow. See the function's comment:
    // the gestures are lost in that trampoline and not in the loose page.
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
    // The test itself puts focus on the field (this one IS a friend); here
    // m_field is private.
    return searchSuggestGesturesSelfTest(&page, stack, otra);
}

// SuggestRow is defined in this .cpp (it needs the moc here).
#include "search_page.moc"
