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

    // Refresca la fila "Aceleración de vídeo" con el perfil vigente.
    void updateCodecProfile(const QString &profile);

signals:
    void qualityChanged(const QString &value);
    void nextModeChanged(const QString &mode);
    void miniVideoModeChanged(const QString &mode);
    void audioLanguageChanged(const QString &code);
    void volumeMaxChanged(const QString &value);
    void gridDensityChanged(const QString &density);
    void clearThumbsRequested();
    void resetRequested();
    void redetectHwRequested();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void buildUI();
    // Fila "etiqueta + botón de valor": el botón abre un QMenu con las
    // alternativas (mismo patrón que los tres puntos de Biblioteca). Al
    // elegir una opción llama a onPick(displayText).
    // Botón de acción simple (mismo estilo y tamaño que los de valor, sin el
    // sufijo "▾"): usado p. ej. por "Volver a detectar".
    QPushButton *makeActionButton(const QString &text);
    QPushButton *makeValueButton(const QString &text);
    void setupValueMenu(QPushButton *btn, const QStringList &options,
                        const QString &current,
                        const std::function<void(const QString &)> &onPick);
    // Variante con confirmación asíncrona (modal). `confirm(opt, applyVisual)`
    // decide por opción: aplica directo o muestra modal; al Aceptar llama
    // applyVisual (que registra la opción y actualiza botón + checks), al
    // Cancelar NO lo llama. `current` es el texto exacto de una QAction.
    void setupValueMenuWithConfirm(
        QPushButton *btn, const QStringList &options, const QString &current,
        const std::function<void(const QString &,
                                  const std::function<void(const QString &)> &)>
            &confirm);
    // Popup flotante no modal (mismo patrón que Nueva playlist), centrado en
    // la página; no bloquea ni secuestra el puntero.
    void showConfirmPopup(const QString &title, const QString &text,
                          const QString &acceptText,
                          std::function<void()> onConfirm);
    void closeConfirmPopup();
    void centerPopup();

    SettingsRepo *m_settings;
    QLabel *m_codecProfileLabel = nullptr;
    QFrame *m_confirmPopup = nullptr;
    QLabel *m_confirmTitle = nullptr;
    QLabel *m_confirmText = nullptr;
    QPushButton *m_confirmCancel = nullptr;
    QPushButton *m_confirmOk = nullptr;
    std::function<void()> m_confirmCb;
};