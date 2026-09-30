#pragma once

#include <QVector>
#include <QHash>
#include <QWidget>
#include <QFrame>
#include <QSpacerItem>

#include "../../models.h"
#include "../../yt_innertube.h"

class QJsonDocument;
class QLineEdit;
class QScrollArea;
class QTimer;
class VideoListView;
class ThumbnailCache;
class SettingsRepo;
class ChannelRow;
class Yt;
class QPushButton;
class QKeyEvent;
class SuggestRow;

class SearchPage : public QWidget
{
    Q_OBJECT

public:
    explicit SearchPage(Yt *yt, SettingsRepo *settings,
                        ThumbnailCache *thumbs, QWidget *parent = nullptr);
    ~SearchPage() override;
    void startSearch(const QString &query);

    // Medidas del desplegable. El endpoint devuelve hasta 14 sugerencias y el
    // historial suma mas; sin tope el panel crecia hacia abajo y tapaba los
    // resultados. La fila y el separador se necesitan ademas para el alto
    // cuando el layout aun no ha calculado nada (ver showSuggestPanel), y las
    // usa tambien SuggestRow, que es una clase aparte.
    static constexpr int kSuggestPanelMaxH = 320;
    static constexpr int kSuggestRowH = 32;
    static constexpr int kSuggestDividerH = 22;

    // Panel de sugerencias, solo lectura. Lo usa el selftest del desplegable
    // para comprobar que se abre con los dos grupos sin depender de la red.
    QWidget *suggestPanel() const { return m_suggestPanel; }

    // Selftest OFFLINE del desplegable (sin red). Es friend para poder sembrar
    // el historial y el campo y llamar al slot directamente, sin abrir un
    // monton de accesores publicos solo para test.
    friend bool searchSuggestPanelSelfTest(SearchPage *page);

signals:
    void openRequested(const QVector<VideoInfo> &videos, int startIndex);
    void channelRequested(const QString &channelId);

private slots:
    void doSearch();
    void onSearchReady(const QString &query, const QVector<VideoInfo> &videos);
    void onSuggestReady(const QString &query,
                        const QVector<Suggestion> &suggestions);
    void onChannelInfo(const ChannelInfo &channel);
    void onHistoryClear();
    void onHistoryQuery(const QString &query);
    void onHistoryRemove(const QString &query);
    // Del desplegable, desde la navegacion por teclado.
    void onRowPicked(SuggestRow *row);
    void onRowStepFocus(SuggestRow *row, int delta);
    void onRowEscape();
    void onRowTabOut();
    void onRowFocused(SuggestRow *row);

protected:
    // Cierra el desplegable al pulsar fuera. Ver el installEventFilter del
    // constructor: NO se usa editingFinished por el orden de los eventos.
    bool eventFilter(QObject *watched, QEvent *event) override;
    // Al cambiar el ancho de la pagina el campo se ensancha; el desplegable va
    // anclado a el, asi que hay que recolocarlo o se queda desalineado.
    void resizeEvent(QResizeEvent *event) override;
    // Al cambiar de pestana se cierra. Sin esto el panel se queda con su
    // estado de "mostrado" y reaparece al volver a Buscar con sugerencias
    // viejas de lo que se escribio la ultima vez.
    void hideEvent(QHideEvent *event) override;
    // Teclado del campo: flechas mueven el resaltado, Tab entra en el
    // desplegable y Esc lo cierra. Enter NO se toca a proposito: el campo
    // busca SIEMPRE el texto literal, resaltado o no (ver el comentario del
    // constructor y acceptSuggestion).
    void keyPressEvent(QKeyEvent *event) override;

private:
    void showHistory();
    void setResultsMode(bool active);
    void buildChannelButtons();
    QStringList history() const;
    void setHistory(const QStringList &queries);
    // Desplegable de type-ahead. Se ancla al campo y flota encima de la
    // pagina; no toca el layout, asi que la pagina se ve igual.
    void showSuggestPanel();
    void hideSuggestPanel();
    void populateSuggestPanel();
    void positionSuggestPanel();
    // Filas del historial que empiezan por el texto escrito (para el segundo
    // grupo del desplegable), en orden de recurrencia y sin repetir.
    QStringList matchingHistory(const QString &prefix) const;
    // Pulsa una fila del desplegable: pone el texto LITERAL en el campo (sin
    // sustituirlo por la sugerencia) y lanza la busqueda.
    void acceptSuggestion(const QString &query);
    // Resaltado por teclado. -1 = ninguno. Solo cambia el fondo de la fila: el
    // texto del campo nunca se sustituye por la sugerencia resaltada.
    void setHighlight(int index);
    void moveHighlight(int delta);
    // Mueve el foco real a la fila index (o al campo con -1). Es lo que hace
    // Tab al entrar en el desplegable.
    void focusRow(int index);

    Yt *m_yt;
    SettingsRepo *m_settings;
    ThumbnailCache *m_thumbs;

    QLineEdit *m_field;
    QPushButton *m_clearBtn = nullptr;
    QTimer *m_suggestDebounce = nullptr;
    QWidget *m_historyBox;
    QWidget *m_resultBox;
    QWidget *m_channelBox;
    QSpacerItem *m_bottomStretch = nullptr;
    VideoListView *m_list;
    QPushButton *m_moreButton;

    QVector<VideoInfo> m_results;
    QVector<ChannelInfo> m_channels;
    QHash<QString, ChannelRow *> m_channelButtons;
    QWidget *m_channelStrip = nullptr;
    QString m_error;
    bool m_searching = false;
    QString m_activeQuery;

    // Type-ahead. m_suggestPanel flota sobre la pagina; m_suggestItems son las
    // consultas de YouTube (con su detalle de entidad) y m_historyRows las del
    // historial ya filtradas. m_suggestScroll lleva las filas con barra cuando
    // superan el tope, y m_suggestRowsBox es su contenido (se reconstruye
    // entero en cada respuesta).
    QFrame *m_suggestPanel = nullptr;
    QScrollArea *m_suggestScroll = nullptr;
    QWidget *m_suggestRowsBox = nullptr;
    QVector<Suggestion> m_suggestItems;
    QStringList m_historyRows;
    // Las filas como widgets, en el orden en que se pintan, y el indice de la
    // resaltada (-1 = ninguna). El orden es: grupo de YouTube y luego de
    // historial, asi que el indice es plano.
    QVector<SuggestRow *> m_rows;
    int m_highlight = -1;
    // Texto para el que se pidio la ultima tanda de sugerencias: permite
    // ignorar tanto respuestas tardias como la peticion en si misma si el
    // campo ya no coincide.
    QString m_suggestQuery;
    // Aplicacion para el eventFilter del cierre por clic fuera.
    QObject *m_app = nullptr;
};

// Selftest OFFLINE del desplegable de sugerencias (sin red): construye una
// SearchPage con las piezas minimas y comprueba que el panel se abre con los
// dos grupos, que el historial se filtra, y que una respuesta tardia se
// ignora. Se llama desde --selftest-suggest-panel.
bool searchSuggestPanelSelfTestRun();