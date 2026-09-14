#pragma once

#include <QWidget>

class QComboBox;
class SettingsRepo;

class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(SettingsRepo *settings, QWidget *parent = nullptr);

private:
    void buildUI();

    SettingsRepo *m_settings;
    QComboBox *m_qualityCombo;
    QComboBox *m_languageCombo;
};
