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
    // Fila del vídeo en reproducción: se dibuja con un recuadro de marca.
    // -1 (o un índice fuera de rango) oculta la marca.
    void setCurrentRow(int row);
    int currentRow() const { return m_currentRow; }
    // Barra de progreso (0..1) sobre la miniatura de un vídeo concreto,
    // para la fila "Continuar viendo". No afecta al resto de páginas
    // (solo se dibuja en el modo rejilla y si el valor es > 0).
    void setProgress(const QString &videoId, double fraction);
    bool isEmpty() const;
    QVector<VideoInfo> videos() const;
    void refreshThumbnails();
    // Escala de tamaño de las celdas (1.0 = tamaño base).
    void setScale(qreal scale);
    // Densidad de cuadrícula aplicada sobre la escala base (1.0 = densidad
    // normal, <1 compacta, >1 cómoda). No pisa la escala base de la página.
    void setDensity(qreal density);
    // Muestra un botón "X" a la derecha de cada fila (modo lista) que permite
    // eliminar el ítem. El click sobre la X emite removeRequested() en lugar
    // de videoActivated(). Por defecto está desactivado: no afecta a otras
    // páginas que reutilizan la lista.
    void setRowRemoveVisible(bool visible);
    // Similar a setRowRemoveVisible pero con el botón de opciones "⋯" (modo
    // lista): el click sobre él emite moreRequested() en lugar de
    // videoActivated(). Puede combinarse con la X; el botón de opciones queda
    // a la izquierda de la X.
    void setRowMoreVisible(bool visible);
    // Líneas del bloque de meta de cada fila (modo lista):
    //   1 (defecto) = una sola línea "canal • vistas", como hoy;
    //   2 = bloque enriquecido que aprovecha el hueco libre de la fila de 86 px
    //       (no crece): título a 2 líneas, "vistas • fecha" y el canal con su
    //       avatar en una tercera línea, más hover redondeado.
    // Solo lo pide Search; el resto de listas se quedan en 1 y no cambian.
    void setMetaLines(int lines);
    // Hace que las celdas del modo rejilla se estiren para llenar todo el ancho
    // disponible (0 desactiva: se usa el tamaño base fijo). El ancho mínimo de
    // celda fija cuántas columnas entran; el sobrante se reparte entre ellas.
    void setGridFillWidth(int minCellWidth);
    // Fuerza un número fijo de columnas en el modo rejilla (0 = automático por
    // ancho mínimo). Usado por el Home para mostrar los vídeos por fila que elija
    // el usuario (4/5/6 según "Tamaño de las miniaturas").
    void setGridColumns(int columns);
    int gridColumns() const;

    // Ancho de celda por debajo del cual el bloque de texto (miniatura 16:9 +
    // hueco + título de 2 líneas + línea de canal) se recorta. Es la constante
    // que justifica el tope de columnas del Home.
    static constexpr int kMinCellW = 130;

    // Recorta las columnas pedidas para que la celda no baje de minCellW: una
    // rejilla es dimensionada por el ancho con columnas fijas, así que con 6
    // columnas en una ventana estrecha (celda de 113 px a 700 px de viewport) el
    // título y el canal saldrían cortados; es mejor mostrar menos vídeos por
    // fila que metidos. Función PURA (sin widget) para poder testearla.
    // viewportW <= 0 = ancho aún no conocido: no se recorta nada.
    static int clampGridColumns(int wanted, int viewportW,
                                int minCellW = kMinCellW);

signals:
    void videoActivated(int index, const VideoInfo &video);
    // Click sobre el botón "X" de una fila (solo si setRowRemoveVisible(true)).
    void removeRequested(int index, const VideoInfo &video);
    // Click sobre el botón de opciones "⋯" de una fila
    // (solo si setRowMoreVisible(true)).
    void moreRequested(int index, const VideoInfo &video);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onThumbReady(const QString &key);

private:
    // Recalcula el ancho de celda del modo rejilla cuando está activo el
    // relleno de ancho (setGridFillWidth). No hace nada en modo lista.
    void applyGridGeometry();

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
    int m_metaLines = 1;
    int m_gridMinCellW = 0;
    int m_gridColumns = 0;
    QHash<QString, double> m_progress;
};