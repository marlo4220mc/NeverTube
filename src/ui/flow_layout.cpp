#include "flow_layout.h"

#include <QWidget>
#include <QWidgetItem>

// Brief description of doLayout: wrapped rows, each item at its sizeHint,
// fixed horizontal and vertical spacing.

FlowLayout::FlowLayout(QWidget *parent, int margin, int hSpacing, int vSpacing)
    : QLayout(parent)
    , m_hSpace(hSpacing)
    , m_vSpace(vSpacing)
{
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem *item = takeAt(0))
        delete item;
}

void FlowLayout::addItem(QLayoutItem *item)
{
    m_items.append(item);
}

void FlowLayout::addWidget(QWidget *w)
{
    addItem(new QWidgetItem(w));
}

int FlowLayout::horizontalSpacing() const
{
    if (m_hSpace >= 0)
        return m_hSpace;
    return smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
}

int FlowLayout::verticalSpacing() const
{
    if (m_vSpace >= 0)
        return m_vSpace;
    return smartSpacing(QStyle::PM_LayoutVerticalSpacing);
}

int FlowLayout::smartSpacing(QStyle::PixelMetric pm) const
{
    const QObject *parentObj = parent();
    if (!parentObj)
        return -1;
    if (parentObj->isWidgetType()) {
        const QWidget *pw = static_cast<const QWidget *>(parentObj);
        return pw->style()->pixelMetric(pm, nullptr, pw);
    }
    return static_cast<const QLayout *>(parentObj)->spacing();
}

int FlowLayout::count() const
{
    return m_items.size();
}

QLayoutItem *FlowLayout::itemAt(int index) const
{
    return m_items.value(index);
}

QLayoutItem *FlowLayout::takeAt(int index)
{
    if (index >= 0 && index < m_items.size())
        return m_items.takeAt(index);
    return nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const
{
    return Qt::Orientations();
}

bool FlowLayout::hasHeightForWidth() const
{
    return true;
}

int FlowLayout::heightForWidth(int width) const
{
    return doLayout(QRect(0, 0, width, 0), true);
}

QSize FlowLayout::sizeHint() const
{
    return minimumSize();
}

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (const QLayoutItem *item : m_items)
        size = size.expandedTo(item->minimumSize());
    const QMargins margins = contentsMargins();
    size += QSize(margins.left() + margins.right(),
                  margins.top() + margins.bottom());
    return size;
}

void FlowLayout::setGeometry(const QRect &rect)
{
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

int FlowLayout::doLayout(const QRect &rect, bool testOnly) const
{
    const int left = rect.x();
    const int top = rect.y();
    const int right = rect.right();
    int x = left;
    int y = top;
    int lineHeight = 0;

    const QMargins margins = contentsMargins();
    const int effectiveLeft = left + margins.left();
    const int effectiveRight = right - margins.right();
    const int effectiveTop = top + margins.top();
    x = effectiveLeft;
    y = effectiveTop;

    const int hSpacing = horizontalSpacing();
    const int vSpacing = verticalSpacing();
    const auto pad = [](int v) { return v >= 0 ? v : 0; };
    const int hh = pad(hSpacing);
    const int vv = pad(vSpacing);

    for (QLayoutItem *item : m_items) {
        int nextX = x + item->sizeHint().width() + hh;
        if (nextX - hh > effectiveRight && lineHeight > 0) {
            x = effectiveLeft;
            y += lineHeight + vv;
            nextX = x + item->sizeHint().width() + hh;
            lineHeight = 0;
        }
        if (!testOnly)
            item->setGeometry(QRect(QPoint(x, y), item->sizeHint()));
        x = nextX;
        lineHeight = qMax(lineHeight, item->sizeHint().height());
    }
    return y + lineHeight - rect.y() + margins.bottom();
}