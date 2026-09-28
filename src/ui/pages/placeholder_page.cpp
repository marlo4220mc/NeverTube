#include "placeholder_page.h"

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

PlaceholderPage::PlaceholderPage(const QString &title, QWidget *parent)
    : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(16, 12, 16, 8);
    m_title = new QLabel(title, this);
    m_title->setObjectName(QStringLiteral("pageTitle"));
    row->addWidget(m_title);
    row->addStretch(1);
    lay->addLayout(row);

    auto *hint = new QLabel(QStringLiteral("Próximamente."), this);
    hint->setObjectName(QStringLiteral("placeholderHint"));
    hint->setAlignment(Qt::AlignCenter);
    lay->addWidget(hint, 1);
    lay->addStretch(8);
}