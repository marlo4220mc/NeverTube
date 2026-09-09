#pragma once

#include <QString>
#include <QWidget>

class QLabel;

class PlaceholderPage : public QWidget
{
    Q_OBJECT

public:
    explicit PlaceholderPage(const QString &title, QWidget *parent = nullptr);

signals:
    void backRequested();

private:
    QLabel *m_title;
};