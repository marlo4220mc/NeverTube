#ifndef NT_FLOW_LAYOUT_H
#define NT_FLOW_LAYOUT_H

// Layout fluido: coloca los items en filas que se envuelven cuando el ancho es
// insuficiente (estilo flexbox). Reutilizable para filas de botones que no
// deben desbordar ni generar scroll horizontal en columnas estrechas.

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