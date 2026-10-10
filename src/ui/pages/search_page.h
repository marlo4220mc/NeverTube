#pragma once

#include <QVector>
#include <QHash>
#include <QWidget>
#include <QFrame>
#include <QSpacerItem>

#include "../../models.h"
#include "../../yt_innertube.h"

class QJsonDocument;
class QLineEdit;
class QScrollArea;
class QStackedWidget;
class QTimer;
class VideoListView;
class ThumbnailCache;
class SettingsRepo;
class ChannelRow;
class Yt;
class QPushButton;
class QKeyEvent;
class SuggestRow;

class SearchPage : public QWidget
{
    Q_OBJECT

public:
    explicit SearchPage(Yt *yt, SettingsRepo *settings,
                        ThumbnailCache *thumbs, QWidget *parent = nullptr);
    ~SearchPage() override;
    void startSearch(const QString &query);

    // Sizes of the dropdown. The endpoint returns up to 14 suggestions and the
    // history adds more; without a cap the panel grew downward and covered the
    // results. The row and the divider are also needed for the height when the
    // layout has not computed anything yet (see showSuggestPanel), and
    // SuggestRow uses them too, as it is a separate class.
    static constexpr int kSuggestPanelMaxH = 320;
    static constexpr int kSuggestRowH = 32;
    static constexpr int kSuggestDividerH = 22;

    // Suggestions panel, read-only. The dropdown selftest uses it to check that
    // it opens with both groups without depending on the network.
    QWidget *suggestPanel() const { return m_suggestPanel; }

    // OFFLINE selftest of the dropdown (no network). It is a friend so it can
    // seed the history and the field and call the slot directly, without opening
    // a bunch of public accessors just for the test.
    friend bool searchSuggestPanelSelfTest(SearchPage *page);
    // The gesture bench also seeds the field and the history, and needs the
    // container to test the tab switch.
    friend bool searchSuggestGesturesSelfTest(SearchPage *page, QStackedWidget *stack,
                                              QWidget *otra);

signals:
    void openRequested(const QVector<VideoInfo> &videos, int startIndex);
    void channelRequested(const QString &channelId);
    // Click on the "⋯" of a results row: the page has no repos
    // (watch later, playlists), so it asks for the video and MainWindow shows
    // the shared menu.
    void rowMenuRequested(const VideoInfo &video, const QPoint &globalPos);

private slots:
    void doSearch();
    void onSearchReady(const QString &query, const QVector<VideoInfo> &videos);
    void onSuggestReady(const QString &query,
                        const QVector<Suggestion> &suggestions);
    void onChannelInfo(const ChannelInfo &channel);
    void onHistoryClear();
    void onHistoryQuery(const QString &query);
    void onHistoryRemove(const QString &query);
    // From the dropdown, via keyboard navigation.
    void onRowPicked(SuggestRow *row);
    void onRowStepFocus(SuggestRow *row, int delta);
    void onRowEscape();
    void onRowTabOut();
    void onRowFocused(SuggestRow *row);

protected:
    // Closes the dropdown when clicking outside, and resolves the field's
    // keyboard (fieldKey). See the constructor's installEventFilter: NO
    // editingFinished is used because of the event order, and the keyboard is
    // NOT in the page's keyPressEvent because the "card" QFrame eats the Tab.
    bool eventFilter(QObject *watched, QEvent *event) override;
    // When the page width changes the field widens; the dropdown is anchored to
    // it, so it must be repositioned or it stays misaligned.
    void resizeEvent(QResizeEvent *event) override;
    // Closes when switching tabs. Without this the panel keeps its "shown"
    // state and reappears when coming back to Search with stale suggestions
    // from whatever was typed last time.
    void hideEvent(QHideEvent *event) override;

private:
    // Field keyboard with the dropdown open: arrows move the highlight, Tab
    // enters the dropdown and Esc closes it. Called from eventFilter (not from
    // a keyPressEvent) for the QFrame reason.
    // Enter is deliberately NOT touched: the field ALWAYS searches the literal
    // text, highlighted or not (see the constructor's comment and
    // acceptSuggestion).
    bool fieldKey(QKeyEvent *event);
    void showHistory();
    void setResultsMode(bool active);
    void buildChannelButtons();
    QStringList history() const;
    void setHistory(const QStringList &queries);
    // Type-ahead dropdown. It anchors to the field and floats over the
    // page; it does not touch the layout, so the page looks the same.
    void showSuggestPanel();
    void hideSuggestPanel();
    void populateSuggestPanel();
    void positionSuggestPanel();
    // History rows starting with the typed text (for the dropdown's second
    // group), in order of recurrence and without duplicates.
    QStringList matchingHistory(const QString &prefix) const;
    // Presses a dropdown row: puts the LITERAL text in the field (without
    // replacing it with the suggestion) and launches the search.
    void acceptSuggestion(const QString &query);
    // Keyboard highlight. -1 = none. Only changes the row background: the
    // field text is never replaced by the highlighted suggestion.
    void setHighlight(int index);
    void moveHighlight(int delta);
    // Moves the real focus to row index (or to the field with -1). That is what
    // Tab does when entering the dropdown.
    void focusRow(int index);

    Yt *m_yt;
    SettingsRepo *m_settings;
    ThumbnailCache *m_thumbs;

    QLineEdit *m_field;
    QPushButton *m_clearBtn = nullptr;
    QTimer *m_suggestDebounce = nullptr;
    QWidget *m_historyBox;
    QWidget *m_resultBox;
    QWidget *m_channelBox;
    QSpacerItem *m_bottomStretch = nullptr;
    VideoListView *m_list;
    QPushButton *m_moreButton;

    QVector<VideoInfo> m_results;
    QVector<ChannelInfo> m_channels;
    QHash<QString, ChannelRow *> m_channelButtons;
    QWidget *m_channelStrip = nullptr;
    QString m_error;
    bool m_searching = false;
    QString m_activeQuery;

    // Type-ahead. m_suggestPanel floats over the page; m_suggestItems are the
    // YouTube queries (with their entity detail) and m_historyRows the already
    // filtered history ones. m_suggestScroll carries the rows with a scrollbar
    // when they exceed the cap, and m_suggestRowsBox is its content (rebuilt in
    // full on every response).
    QFrame *m_suggestPanel = nullptr;
    QScrollArea *m_suggestScroll = nullptr;
    QWidget *m_suggestRowsBox = nullptr;
    QVector<Suggestion> m_suggestItems;
    QStringList m_historyRows;
    // The rows as widgets, in the order they are painted, and the index of the
    // highlighted one (-1 = none). The order is: YouTube group and then
    // history, so the index is flat.
    QVector<SuggestRow *> m_rows;
    int m_highlight = -1;
    // Text for which the last batch of suggestions was requested: allows
    // ignoring both late responses and the request itself if the field no
    // longer matches.
    QString m_suggestQuery;
    // Query already SEARCHED (Enter or button): a suggest arriving later for
    // that same query must not reopen the dropdown (it happened when typing
    // fast and pressing Enter: the 300 ms debounce fired afterwards and the
    // panel appeared over the results). Cleared when typing again.
    QString m_suggestClosedFor;
    // Application target for the click-outside close eventFilter.
    QObject *m_app = nullptr;
};

// OFFLINE selftest of the suggestions dropdown (no network): builds a
// SearchPage with the minimal pieces and checks that the panel opens with the
// two groups, that the history is filtered, and that a late response is
// ignored. Called from --selftest-suggest-panel.
bool searchSuggestPanelSelfTestRun();

// Gesture bench of the dropdown in the real hierarchy (page inside a
// QStackedWidget, with a navigation bar and the real QSS). Checks the
// gestures with real delivery, which is where they failed. Called from
// --selftest-suggest-gestures.
bool searchSuggestGesturesSelfTestRun();
