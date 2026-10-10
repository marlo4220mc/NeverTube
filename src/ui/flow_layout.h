#ifndef NT_FLOW_LAYOUT_H
#define NT_FLOW_LAYOUT_H

// Flow layout: places the items in rows that wrap when the width is
// insufficient (flexbox style). Reusable for button rows that must not
// overflow nor produce horizontal scroll in narrow columns.

#include <QLayout>
#include <QStyle>

class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent, int margin = -1, int hSpacing = -1,
                        int vSpacing = -1);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    void addWidget(QWidget *w);
    int horizontalSpacing() const;
    int verticalSpacing() const;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect &rect) override;
    QSize sizeHint() const override;
    QLayoutItem *takeAt(int index) override;

private:
    int smartSpacing(QStyle::PixelMetric pm) const;
    int doLayout(const QRect &rect, bool testOnly) const;

    QList<QLayoutItem *> m_items;
    int m_hSpace;
    int m_vSpace;
};

#endif // NT_FLOW_LAYOUT_H