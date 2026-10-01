#pragma once

#include <QWidget>

#include <functional>

class QMenu;
class QPushButton;
class QLabel;
class SettingsRepo;

class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(SettingsRepo *settings, QWidget *parent = nullptr);

    // Refresca la fila "Aceleración de vídeo" con el perfil vigente.
    void updateCodecProfile(const QString &profile);
    // Re-aplica el botón "Restablecer aplicación" con el acento vigente
    // (MainWindow lo llama al cambiar el color de acento en Ajustes).
    void applyAccent();

    // "Tamaño de las miniaturas" (Ajustes → Interfaz): valor persistido
    // ("small" | "normal" | "large"), ya migrado desde el antiguo ajuste
    // "Densidad de cuadrícula". MainWindow lo lee al arrancar para pintar con
    // el tamaño correcto desde el primer frame, sin esperar a un cambio.
    QString thumbSize() const { return m_thumbSize; }
    // El valor se traduce a DOS efectos, porque las rejillas y las listas no
    // se dimensionan igual (ver AGENTS.md):
    //  - las VISTAS DE LISTA (búsqueda, canal, historial, playlists, cola
    //    "A continuación") escalan su fila y su miniatura con un factor;
    //  - las REJILLAS del Inicio son dimensionadas por el ancho con un número
    //    de columnas FIJO, así que un factor no hace nada visible: la palanca
    //    es el número de columnas.
    static double thumbListScale(const QString &value);
    static int thumbGridColumns(const QString &value);

signals:
    void qualityChanged(const QString &value);
    void nextModeChanged(const QString &mode);
    void miniVideoModeChanged(const QString &mode);
    void audioLanguageChanged(const QString &code);
    void volumeMaxChanged(const QString &value);
    void thumbSizeChanged(const QString &value);
    void accentChanged(const QString &hex);
    void clearThumbsRequested();
    void resetRequested();
    void redetectHwRequested();

private:
    void buildUI();
    // Fila "etiqueta + botón de valor": el botón abre un QMenu con las
    // alternativas (mismo patrón que los tres puntos de Biblioteca). Al
    // elegir una opción llama a onPick(displayText).
    // Botón de acción simple: usado p. ej. por "Volver a detectar".
    QPushButton *makeActionButton(const QString &text);
    // Botón de VALOR: abre un QMenu con las alternativas. Va con el mismo
    // aspecto que el de acción y SIN triángulo final (decisión del usuario:
    // el "▼" sobraba; ver el comentario en la definición).
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
    // Confirmaciones: nt::confirm, el MISMO overlay modal de la app que usan
    // Biblioteca, Historial, Playlist y Búsqueda. Antes esta página tenía su
    // propio popup (380 px en vez de los 400 de nt::confirm, sin velo, sin
    // modal y con su propia pieza de recentrado), o sea un cuarto sitio donde
    // "confirmar antes de borrar" se veía y se comportaba de otra manera.

    SettingsRepo *m_settings;
    QLabel *m_codecProfileLabel = nullptr;
    // Valor persistido de "Tamaño de las miniaturas" (ver thumbSize()).
    QString m_thumbSize;
};