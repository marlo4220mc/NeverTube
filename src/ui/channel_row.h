#pragma once

#include <QWidget>

#include <functional>

#include "../models.h"

class QLabel;
class ThumbnailCache;

// Fixed height and avatar size of the channel card (single source of truth
// for Library and Search).
inline constexpr int kChannelRowHeight = 140;
inline constexpr int kChannelRowAvatar = 36;

// Derives a lowercase @handle from the channel name (same criterion Library
// uses for the "Subscriptions" rows).
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

// Horizontal channel card, same as the Library "Subscriptions" row: circular
// avatar on the left, name + meta (description) on the right, width adaptive
// to the real content, fixed height, hover and click.
//
// Reused by Library (subscriptions row) and by Search (the "Channels"
// section).
class ChannelRow : public QWidget
{
    Q_OBJECT

public:
    explicit ChannelRow(ThumbnailCache *thumbs, const QString &channelId,
                        const QString &avatarUrl, QWidget *parent = nullptr);

    void setTitle(const QString &title);
    // 1 = one text line, 2 = two lines (default 2).
    void setTitleLines(int lines);
    void setMeta(const QString &meta);
    // Applies the minimum width the card needs to show its text at real size
    // (one name line at 14px bold and the widest meta line at 12px). Without
    // it, a QLabel with wordWrap shrinks the card.
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