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

    // Rect REAL de la celda de la fila `row` tal y como la coloca QListView. En
    // IconMode manda el sizeHint del MODELO, no gridSize(), así que este rect
    // puede no coincidir con gridSize() si divergen: eso es justo lo que hay que
    // mirar, porque la página dimensiona la sección con gridSize() y la última
    // fila se salía de la vista. Lo usa --selftest-grid.
    QRect gridItemRect(int row) const;

    // Separación declarada entre celdas de la rejilla. OJO: en IconMode
    // QListView NO la suma a la rejilla (medido: con celda 177x195 los items
    // salen con rect 177x195 y paso 195 entre filas, sin hueco). El hueco que se
    // ve entre miniaturas es el margen interno de 6 px que pinta el delegate.
    static constexpr int kGridSpacing = 4;

    // Tamaño del título de la celda de rejilla DERIVADO del ancho de la celda.
    // Antes eran 13 px fijos con un bloque de texto de 34 px fijos, y como la
    // rejilla tiene columnas FIJAS la miniatura crece con la ventana mientras el
    // texto se quedaba. Ahora va de 12 px (celda de 140 px) a 16 px (celda de
    // 340 px) en lineal, con topes en los dos extremos. Todo el rango bajó 1 px
    // (y el techo de 18 a 16) porque con 14 px en negrita se veía demasiado
    // grande. Función PURA (sin widget ni fuentes): la usan tanto la geometría
    // (applyGridGeometry) como la pintura (paintGrid), de modo que la celda
    // siempre reserva lo que el título ocupa y no hay solape.
    static int gridTitlePx(int cellW);

    // Líneas de título de la celda de rejilla. Con 3 líneas a 14 px en negrita el
    // título se leía entero en el 13% de los casos (medido sobre los 90 títulos de
    // la caché con la caja de 161 px que sale a 5 columnas en una ventana de 941
    // px) pero la celda quedaba con mucho texto y el usuario lo veía demasiado
    // grande. Bajaron a 2: se paga legibilidad (3% enteros, 55% del texto visible
    // de media) a cambio de una celda de 175 px en vez de 195. La negrita se
    // mantiene, que es lo que el usuario pidió tras verla sin ella. Se comparte
    // entre geometría y pintura para que no puedan divergir.
    static constexpr int kGridTitleLines = 2;

    // Interlineado de las líneas de título, en px (1,28 del tamaño de fuente).
    // A 12 px da 15 px, a 13 px daba 17. Función PURA.
    static int gridTitleLineH(int cellW);
    // Alto del bloque de texto bajo la miniatura: kGridTitleLines líneas de
    // título + hueco a la línea de canal + pie. Función PURA.
    static int gridTextBlockH(int cellW);

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
    // Tamaño que deben llevar los items de la rejilla: el que calculó
    // applyGridGeometry, o la base fija si aún no se ha calculado (aún sin ancho
    // de viewport). Único lugar donde se decide, para que showVideos y
    // appendVideos no puedan dejar items con un alto distinto al de la celda.
    QSize gridItemSize() const;

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
    // Tamaño de celda que ha calculado applyGridGeometry. Es la única fuente: el
    // sizeHint del modelo, el gridSize, el delegate y el alto de sección del Home
    // (HomePage::adjustGridHeight usa gridSize) tienen que salir de aquí, o las
    // filas miden una cosa y la sección reserva otra.
    QSize m_gridCell;
    QHash<QString, double> m_progress;
};