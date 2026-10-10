#pragma once

#include <QHash>
#include <QListView>
#include <QVector>

#include "../models.h"

class QStandardItemModel;
class ThumbnailCache;

class VideoListView : public QListView
{
    Q_OBJECT

public:
    enum Mode { GridMode, ListMode };

    explicit VideoListView(ThumbnailCache *thumbs, QWidget *parent = nullptr);

    void showVideos(const QVector<VideoInfo> &videos, Mode mode);
    void appendVideos(const QVector<VideoInfo> &videos);
    void clearVideos();
    // Row of the video being played: drawn with a highlight frame.
    // -1 (or an out-of-range index) hides the mark.
    void setCurrentRow(int row);
    int currentRow() const { return m_currentRow; }
    // Progress bar (0..1) over the thumbnail of a specific video,
    // for the "Continuar viendo" row. It does not affect the rest of the
    // pages (only drawn in grid mode and when the value is > 0).
    void setProgress(const QString &videoId, double fraction);
    bool isEmpty() const;
    QVector<VideoInfo> videos() const;
    void refreshThumbnails();
    // Cell size scale (1.0 = base size).
    void setScale(qreal scale);
    // Grid density applied on top of the base scale (1.0 = normal density,
    // <1 compact, >1 roomy). It does not override the page's base scale.
    void setDensity(qreal density);
    // Shows an "X" button on the right of each row (list mode) that removes
    // the item. Clicking the X emits removeRequested() instead of
    // videoActivated(). It is disabled by default: it does not affect other
    // pages that reuse the list.
    void setRowRemoveVisible(bool visible);
    // Same as setRowRemoveVisible but with the "⋯" options button (list
    // mode): clicking it emits moreRequested() instead of
    // videoActivated(). It can be combined with the X; the options button
    // stays to the left of the X.
    void setRowMoreVisible(bool visible);
    // Makes grid-mode cells stretch to fill the whole available width
    // (0 disables it: the fixed base size is used). The minimum cell width
    // fixes how many columns fit; the remainder is shared among them.
    void setGridFillWidth(int minCellWidth);
    // Forces a fixed number of columns in grid mode (0 = automatic by
    // minimum width). Used by Home to show the videos per row chosen by the
    // user (4/5/6 according to "Tamaño de las miniaturas").
    void setGridColumns(int columns);
    int gridColumns() const;

    // Cell width below which the text block (16:9 thumbnail + gap + 2-line
    // title + channel line) gets clipped. It is the constant that justifies
    // Home's column cap.
    static constexpr int kMinCellW = 130;

    // Trims the requested columns so the cell does not fall below minCellW: a
    // grid is sized by the width with fixed columns, so with 6 columns in a
    // narrow window (cell of 113 px at 700 px of viewport) the title and the
    // channel would be cut off; it is better to show fewer videos per row
    // than crammed in. PURE function (no widget) so it can be tested.
    // viewportW <= 0 = width not known yet: nothing is trimmed.
    static int clampGridColumns(int wanted, int viewportW,
                                int minCellW = kMinCellW);

    // REAL rect of the cell of row `row` as QListView places it. In
    // IconMode the MODEL's sizeHint wins, not gridSize(), so this rect
    // may not match gridSize() if they diverge: that is exactly what to
    // check, because the page sizes the section with gridSize() and the last
    // row went off the view. --selftest-grid uses it.
    QRect gridItemRect(int row) const;

    // Declared spacing between grid cells. NOTE: in IconMode
    // QListView does NOT add it to the grid (measured: with a 177x195 cell
    // the items come out with a rect of 177x195 and a step of 195 between
    // rows, no gap). The gap seen between thumbnails is the internal 6 px margin the delegate paints.
    static constexpr int kGridSpacing = 4;

    // Title size of the grid cell DERIVED from the cell width.
    // It used to be a fixed 13 px with a fixed 34 px text block, and since
    // the grid has FIXED columns the thumbnail grows with the window while
    // the text stayed put. Now it goes from 12 px (cell of 140 px) to 16 px
    // (cell of 340 px) linearly, with caps at both ends. The whole range
    // dropped 1 px (and the ceiling from 18 to 16) because at 14 px bold it
    // looked too big. PURE function (no widget or fonts): it is used both by
    // the geometry (applyGridGeometry) and the painting (paintGrid), so the
    // cell always reserves what the title occupies and there is no overlap.
    static int gridTitlePx(int cellW);

    // Title lines of the grid cell. With 3 lines at 14 px bold the title was
    // read in full in 13% of cases (measured over the 90 titles in
    // the cache with the 161 px box that comes out at 5 columns in a 941 px
    // window) but the cell held too much text and the user found it too big.
    // Reduced to 2: legibility is paid for (3% whole, 55% of the visible text
    // on average) in exchange for a cell of 175 px instead of 195. Bold is
    // kept, which is what the user asked for after seeing it without bold.
    // It is shared between geometry and painting so they cannot diverge.
    static constexpr int kGridTitleLines = 2;

    // Line spacing of the title lines, in px (1.28 of the font size).
    // At 12 px it gives 15 px; at 13 px it gave 17. PURE function.
    static int gridTitleLineH(int cellW);
    // Height of the text block under the thumbnail: kGridTitleLines title
    // lines + gap to the channel line + footer. PURE function.
    static int gridTextBlockH(int cellW);

signals:
    void videoActivated(int index, const VideoInfo &video);
    // Click on the "X" button of a row (only if setRowRemoveVisible(true)).
    void removeRequested(int index, const VideoInfo &video);
    // Click on the "⋯" options button of a row
    // (only if setRowMoreVisible(true)). The button's global position
    // accompanies it in the signal because the view knows where it is: the
    // page that re-emits it upward passes it as-is instead of recomputing it.
    void moreRequested(int index, const VideoInfo &video,
                       const QPoint &globalPos);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onThumbReady(const QString &key);

private:
    // Recomputes the grid-mode cell width when width filling is active
    // (setGridFillWidth). It does nothing in list mode.
    void applyGridGeometry();
    // Size the grid items must use: the one computed by applyGridGeometry, or
    // the fixed base if it has not been computed yet (still no viewport
    // width). Single place where it is decided, so showVideos and appendVideos
    // cannot leave items with a height different from the cell's.
    QSize gridItemSize() const;

    ThumbnailCache *m_thumbs;
    QStandardItemModel *m_model;
    QVector<VideoInfo> m_videos;
    Mode m_mode = ListMode;
    int m_currentRow = -1;
    qreal m_scale = 1.0;
    qreal m_baseScale = 1.0;
    qreal m_density = 1.0;
    bool m_rowRemove = false;
    bool m_rowMore = false;
    int m_gridMinCellW = 0;
    int m_gridColumns = 0;
    // Cell size computed by applyGridGeometry. It is the single source: the
    // model's sizeHint, gridSize, the delegate and Home's section height
    // (HomePage::adjustGridHeight uses gridSize) must all come from here, or
    // the rows measure one thing and the section reserves another.
    QSize m_gridCell;
    QHash<QString, double> m_progress;
};