#pragma once

#include <QWidget>

#include <functional>

#include "../models.h"

class QLabel;
class ThumbnailCache;

// Alto fijo y tamaño de avatar de la tarjeta de canal (un único punto de
// verdad para Biblioteca y Search).
inline constexpr int kChannelRowHeight = 140;
inline constexpr int kChannelRowAvatar = 36;

// Deriva un @handle en minúsculas a partir del nombre del canal (mismo
// criterio que usa la Biblioteca para las filas de "Suscripciones").
inline QString channelHandleFor(const QString &title)
{
    QString h;
    for (const QChar &c : title) {
        if (c.isLetterOrNumber() && c.unicode() < 0x80)
            h += c.toLower();
        else if (c.isSpace() && !h.isEmpty() && !h.endsWith(QLatin1Char('.')))
            h += QLatin1Char('.');
    }
    while (h.endsWith(QLatin1Char('.')))
        h.chop(1);
    if (h.isEmpty())
        h = QStringLiteral("canal");
    return QStringLiteral("@%1").arg(h);
}

// Tarjeta de canal horizontal, igual que la fila de "Suscripciones" de la
// Biblioteca: avatar circular a la izquierda, nombre + meta (descripción) a la
// derecha, ancho adaptativo al contenido real, alto fijo, hover y click.
//
// Reutilizada por Biblioteca (fila de suscripciones) y por Search (sección
// "Canales").
class ChannelRow : public QWidget
{
    Q_OBJECT

public:
    explicit ChannelRow(ThumbnailCache *thumbs, const QString &channelId,
                        const QString &avatarUrl, QWidget *parent = nullptr);

    void setTitle(const QString &title);
    // 1 = una línea de texto, 2 = dos líneas (por defecto 2).
    void setTitleLines(int lines);
    void setMeta(const QString &meta);
    // Aplica el ancho mínimo que la tarjeta necesita para mostrar su texto a
    // tamaño real (una línea de nombre a 14px bold y la línea de meta más
    // ancha a 12px). Sin él, QLabel con wordWrap encoge la tarjeta.
    void applyContentWidth();
    void setAvatarUrl(const QString &url);
    void setActionWidget(QWidget *widget);
    void setActivate(std::function<void()> fn);

    QLabel *metaLabel() const;

signals:
    void activated();

private:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void onThumbReady(const QString &key);

    ThumbnailCache *m_thumbs;
    QString m_channelId;
    QString m_avatarKey;
    QLabel *m_avatar = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    std::function<void()> m_activate;
    bool m_hasAction = false;
    bool m_hover = false;
};