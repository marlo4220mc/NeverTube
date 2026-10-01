#include "playlist_page.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "../../db/repositories.h"
#include "../confirm_dialog.h"
#include "../icons.h"
#include "../theme.h"
#include "../thumbnail_cache.h"
#include "../video_list_view.h"

PlaylistPage::PlaylistPage(PlaylistsRepo *playlists, VideoCacheRepo *cache,
                           ThumbnailCache *thumbs, QWidget *parent)
    : QWidget(parent)
    , m_playlists(playlists)
    , m_cache(cache)
    , m_thumbs(thumbs)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(10);

    auto *top = new QHBoxLayout;
    auto *back = new QPushButton(QStringLiteral("Volver"), this);
    back->setObjectName(QStringLiteral("ghost"));
    back->setCursor(Qt::PointingHandCursor);
    connect(back, &QPushButton::clicked, this,
            &PlaylistPage::backRequested);
    top->addWidget(back);
    top->addStretch(1);
    root->addLayout(top);

    // --- Ficha de la playlist ----------------------------------------------
    // La MISMA caja que la ficha del canal (#channelCard, #channelName,
    // #channelMeta): el nombre y el recuento en una columna, las cuatro
    // acciones a la derecha. Antes iba todo suelto sobre el fondo de la página,
    // en tres filas (título, meta, acciones), y la única caja de la superficie
    // era la lista, así que la cabecera no se leía como una ficha.
    // Los tokens son los mismos a propósito: es una sola forma para las dos
    // superficies de detalle, no dos cosas parecidas.
    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("channelCard"));
    m_card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *cardLay = new QVBoxLayout(m_card);
    cardLay->setContentsMargins(16, 14, 16, 14);

    auto *head = new QHBoxLayout;
    head->setSpacing(14);

    auto *titleCol = new QVBoxLayout;
    titleCol->setContentsMargins(0, 0, 0, 0);
    titleCol->setSpacing(3);
    m_title = new QLabel(QStringLiteral("Playlist"), m_card);
    m_title->setObjectName(QStringLiteral("channelName"));
    m_title->setWordWrap(true);
    titleCol->addWidget(m_title);
    m_meta = new QLabel(m_card);
    m_meta->setObjectName(QStringLiteral("channelMeta"));
    titleCol->addWidget(m_meta);
    head->addLayout(titleCol, 1);

    // Reproducir es la acción principal de la página (#primary) y las otras tres
    // son de gestión de la lista, no de reproducción: por eso van como ghosts
    // al lado y no como cuatro botones del mismo peso.
    auto *playBtn = new QPushButton(QStringLiteral("Reproducir"), m_card);
    playBtn->setObjectName(QStringLiteral("primary"));
    playBtn->setCursor(Qt::PointingHandCursor);
    playBtn->setFixedHeight(38);
    connect(playBtn, &QPushButton::clicked, this, [this]() {
        if (!m_videos.isEmpty())
            emit openRequested(m_videos, 0);
    });
    head->addWidget(playBtn, 0, Qt::AlignVCenter);

    // El botón quita la fila MARCADA, no una "seleccionada": la lista es
    // NoSelection y el único estado es la fila activa (m_currentRow, la del
    // último clic, que además es la que se está reproduciendo). Decir
    // "seleccionado" prometía algo que no existe, así que el texto dice lo que
    // hace. Sigue empezando deshabilitado: sin fila marcada no hay nada que
    // quitar, y con el texto corto el botón no compite con "Reproducir".
    m_removeBtn = new QPushButton(QStringLiteral("Quitar"), m_card);
    m_removeBtn->setObjectName(QStringLiteral("ghost"));
    m_removeBtn->setCursor(Qt::PointingHandCursor);
    m_removeBtn->setFixedHeight(38);
    m_removeBtn->setEnabled(false);
    m_removeBtn->setToolTip(QStringLiteral("Quitar de la playlist el vídeo marcado"));
    connect(m_removeBtn, &QPushButton::clicked, this,
            &PlaylistPage::onRemoveMarked);
    head->addWidget(m_removeBtn, 0, Qt::AlignVCenter);

    m_renameBtn = new QPushButton(QStringLiteral("Renombrar"), m_card);
    m_renameBtn->setObjectName(QStringLiteral("ghost"));
    m_renameBtn->setCursor(Qt::PointingHandCursor);
    m_renameBtn->setFixedHeight(38);
    connect(m_renameBtn, &QPushButton::clicked, this, &PlaylistPage::onRename);
    head->addWidget(m_renameBtn, 0, Qt::AlignVCenter);

    m_deleteBtn = new QPushButton(QStringLiteral("Eliminar"), m_card);
    m_deleteBtn->setObjectName(QStringLiteral("ghost"));
    m_deleteBtn->setCursor(Qt::PointingHandCursor);
    m_deleteBtn->setFixedHeight(38);
    connect(m_deleteBtn, &QPushButton::clicked, this,
            &PlaylistPage::onDelete);
    head->addWidget(m_deleteBtn, 0, Qt::AlignVCenter);

    cardLay->addLayout(head);
    root->addWidget(m_card);

    m_list = new VideoListView(m_thumbs, this);
    connect(m_list, &VideoListView::videoActivated, this,
            [this](int index, const VideoInfo &) {
                m_list->setCurrentRow(index);
                m_removeBtn->setEnabled(index >= 0 && index < m_videos.size());
                emit openRequested(m_videos, index);
            });
    // Botón de opciones por fila, igual que Search/Canal/Historial: la página
    // pide el vídeo y MainWindow muestra el menú compartido.
    m_list->setRowMoreVisible(true);
    connect(m_list, &VideoListView::moreRequested, this,
            [this](int, const VideoInfo &v, const QPoint &pos) {
                emit rowMenuRequested(v, pos);
            });
    // Estado vacío: una playlist sin vídeos no dejaba más que la lista en negro
    // pelado. Es la misma caja punteada #emptyState de la Biblioteca, de las
    // pestañas del Canal y del Historial, con el alto FIJO de 96 px para que no
    // se estire a todo el alto del área.
    // Sin el glifo U+22EE (tres puntos verticales) en el texto: no está en la
    // fuente de la app y saldría el marcador de glifo ausente. De ahí que se
    // diga "el menú" y no "el ⋮"; --selftest-glyphs avisa de esto.
    m_empty = new QLabel(
        QStringLiteral("Esta playlist todavía no tiene vídeos. Añádelos desde el menú de cualquier vídeo."),
        this);
    m_empty->setObjectName(QStringLiteral("emptyState"));
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setFixedHeight(96);
    m_empty->hide();

    // Lista y caja vacía van en un CONTENEDOR con stretch 1, y no sueltas en el
    // layout de la página. Motivo medido: al ocultar la lista, un QVBoxLayout
    // reparte el sobrante vertical como separación EXTRA entre sus elementos, así
    // que la cabecera se iba hacia abajo (con la lista vacía, "Volver" caía a
    // y=195 en vez de y=16 y la ficha a y=417 en vez de y=59). Con el contenedor
    // el sobrante se lo queda él y la cabecera no se mueve: la lista y la caja
    // vacía empiezan siempre en el mismo sitio.
    m_area = new QWidget(this);
    auto *areaLay = new QVBoxLayout(m_area);
    areaLay->setContentsMargins(0, 0, 0, 0);
    areaLay->setSpacing(0);
    areaLay->addWidget(m_list, 1);
    // La caja vacía va con stretch 1 y SIN alineación, a propósito: con
    // Qt::AlignHCenter el layout le da solo el sizeHint y la caja sale estrecha
    // (medido: 174 px) con el texto cortado, y la política Expanding no lo
    // cambia. Sin alineación ocupa todo el ancho del área y, como el alto es
    // fijo de 96, Qt la centra en el hueco que le reparte (medido: 909x96 en
    // y=500 dentro del área de y=141..956), igual que las pestañas del Canal.
    areaLay->addWidget(m_empty, 1);
    root->addWidget(m_area, 1);
}

void PlaylistPage::openPlaylist(const QString &id, const QString &title)
{
    m_playlistId = id;
    m_title->setText(title);
    reload();
}

void PlaylistPage::reload()
{
    if (m_playlistId.isEmpty())
        return;
    const QStringList ids = m_playlists->videoIds(m_playlistId);
    m_videos = m_cache->getMany(ids);
    m_list->showVideos(m_videos, VideoListView::ListMode);
    m_list->setCurrentRow(-1);
    m_removeBtn->setEnabled(false);
    m_meta->setText(QStringLiteral("%1 vídeos").arg(m_videos.size()));
    // Lista o caja vacía, nunca las dos (ver el comentario del constructor).
    m_empty->setVisible(m_videos.isEmpty());
    m_list->setVisible(!m_videos.isEmpty());
}

// Quita de la playlist la fila MARCADA (currentRow, la del último clic). La
// lista es NoSelection: no hay selección múltiple, así que esto quita UN vídeo,
// el que el botón muestra marcado con el borde de acento.
void PlaylistPage::onRemoveMarked()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_videos.size())
        return;
    const QString id = m_videos.at(row).id;
    m_playlists->removeVideo(m_playlistId, id);
    reload();
}

void PlaylistPage::onRename()
{
    QString newTitle;
    if (nt::inputText(this, QStringLiteral("Renombrar playlist"),
                      QStringLiteral("Nuevo nombre:"),
                      m_title->text(), QStringLiteral("Guardar"), &newTitle)) {
        if (newTitle.isEmpty())
            return;
        m_playlists->rename(m_playlistId, newTitle);
        m_title->setText(newTitle);
    }
}

void PlaylistPage::onDelete()
{
    // Confirmación antes de borrar la playlist entera: es lo único que queda de
    // ella, y los vídeos sí se pueden volver a añadir uno a uno. El diálogo de la
    // app (nt::confirm) y no QMessageBox, por el mismo motivo que el de la
    // página Buscar. El cuerpo lleva el nombre de la playlist y su recuento, que
    // es lo que dice qué se va a perder.
    if (m_playlistId.isEmpty())
        return;
    const QString name = m_title->text().trimmed();
    const QString where = name.isEmpty()
        ? QStringLiteral("esta playlist")
        : QStringLiteral("«%1»").arg(name);
    nt::confirm(this, QStringLiteral("Eliminar playlist"),
                QStringLiteral("Se eliminará %1 con sus %2 vídeos. Esta acción "
                               "no se puede deshacer.")
                    .arg(where, QStringLiteral("%1").arg(m_videos.size())),
                QStringLiteral("Eliminar"), [this]() {
                    m_playlists->deletePlaylist(m_playlistId);
                    m_playlistId.clear();
                    reload();
                    emit backRequested();
                });
}