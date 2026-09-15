#pragma once

#include <QWidget>

#include <functional>

class QFrame;
class QMenu;
class QPushButton;
class QLabel;
class QResizeEvent;
class SettingsRepo;

class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(SettingsRepo *settings, QWidget *parent = nullptr);

signals:
    void qualityChanged(const QString &value);
    void nextModeChanged(const QString &mode);
    void audioLanguageChanged(const QString &code);
    void gridDensityChanged(const QString &density);
    void clearThumbsRequested();
    void resetRequested();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void buildUI();
    // Fila "etiqueta + botón de valor": el botón abre un QMenu con las
    // alternativas (mismo patrón que los tres puntos de Biblioteca). Al
    // elegir una opción llama a onPick(displayText).
    QPushButton *makeValueButton(const QString &text);
    void setupValueMenu(QPushButton *btn, const QStringList &options,
                        const QString &current,
                        const std::function<void(const QString &)> &onPick);
    // Popup flotante no modal (mismo patrón que Nueva playlist), centrado en
    // la página; no bloquea ni secuestra el puntero.
    void showConfirmPopup(const QString &title, const QString &text,
                          const QString &acceptText,
                          std::function<void()> onConfirm);
    void closeConfirmPopup();
    void centerPopup();

    SettingsRepo *m_settings;
    QFrame *m_confirmPopup = nullptr;
    QLabel *m_confirmTitle = nullptr;
    QLabel *m_confirmText = nullptr;
    QPushButton *m_confirmCancel = nullptr;
    QPushButton *m_confirmOk = nullptr;
    std::function<void()> m_confirmCb;
};