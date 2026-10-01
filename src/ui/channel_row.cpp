#include "channel_row.h"

#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QVBoxLayout>
#include <QtMath>

#include "../models.h"
#include "icons.h"
#include "theme.h"
#include "thumbnail_cache.h"

namespace {

// El recorte circular del avatar vive en nt::circularAvatar (src/ui/icons.*):
// la ficha de canal y esta tarjeta lo comparten, así que son el mismo código.

QPixmap placeholderAvatar(int size)
{
    QPixmap out(size, size);
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addEllipse(0, 0, size, size);
    p.setClipPath(clip);
    p.fillRect(0, 0, size, size, nt::kElevated);
    const QPixmap glyph = nt::icon(nt::Icon::User, nt::kMuted, 20).pixmap(28, 28);
    p.drawPixmap((size - 28) / 2, (size - 28) / 2, glyph);
    return out;
}

} // namespace

ChannelRow::ChannelRow(ThumbnailCache *thumbs, const QString &channelId,
                       const QString &avatarUrl, QWidget *parent)
    : QWidget(parent)
    , m_thumbs(thumbs)
    , m_channelId(channelId)
    , m_avatarKey(QStringLiteral("ch_%1").arg(channelId))
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(kChannelRowHeight);
    setCursor(Qt::PointingHandCursor);

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(14);

    m_avatar = new QLabel(this);
    m_avatar->setFixedSize(kChannelRowAvatar, kChannelRowAvatar);
    m_avatar->setPixmap(placeholderAvatar(kChannelRowAvatar));
    if (!avatarUrl.isEmpty())
        setAvatarUrl(avatarUrl);
    lay->addWidget(m_avatar);

    auto *textLay = new QVBoxLayout;
    textLay->setContentsMargins(0, 0, 0, 0);
    textLay->setSpacing(2);
    textLay->addStretch(1);
    textLay->addStretch(1);
    lay->addLayout(textLay, 1);

    m_title = new QLabel(QString(), this);
    m_title->setWordWrap(true);
    QFont titleFont = m_title->font();
    titleFont.setPixelSize(14);
    titleFont.setWeight(QFont::DemiBold);
    m_title->setFont(titleFont);
    m_title->setObjectName(QStringLiteral("channelRowTitle"));
    m_title->setFixedHeight(qRound(QFontMetricsF(m_title->font()).height() + 1));
    textLay->insertWidget(1, m_title);

    m_meta = new QLabel(QString(), this);
    m_meta->setWordWrap(true);
    QFont metaFont = m_meta->font();
    metaFont.setPixelSize(12);
    m_meta->setFont(metaFont);
    m_meta->setObjectName(QStringLiteral("channelRowMeta"));
    m_meta->setFixedHeight(
        qRound(QFontMetricsF(m_meta->font()).height() * 2 + 3));
    textLay->insertWidget(textLay->count() - 1, m_meta);

    connect(thumbs, &ThumbnailCache::ready, this, &ChannelRow::onThumbReady);
}

void ChannelRow::setTitle(const QString &title)
{
    m_title->setText(title);
}

void ChannelRow::setTitleLines(int lines)
{
    const QFontMetricsF fm(m_title->font());
    m_title->setFixedHeight(lines <= 1 ? qRound(fm.height() + 1)
                                       : qRound(fm.height() * lines + 3));
}

void ChannelRow::setMeta(const QString &meta)
{
    m_meta->setText(meta);
}

void ChannelRow::applyContentWidth()
{
    const QFontMetricsF titleFm(m_title->font());
    const QFontMetricsF metaFm(m_meta->font());
    qreal textW = titleFm.horizontalAdvance(m_title->text());
    const QStringList lines = m_meta->text().split(QLatin1Char('\n'));
    for (const QString &line : lines)
        textW = qMax(textW, metaFm.horizontalAdvance(line));
    const int actionW = m_hasAction ? 14 + 28 : 0;
    setFixedWidth(qCeil(textW) + 6 + kChannelRowAvatar + 14 + actionW + 6);
}

void ChannelRow::setAvatarUrl(const QString &url)
{
    if (url.isEmpty())
        return;
    const QPixmap cached = m_thumbs->pixmap(m_avatarKey);
    if (!cached.isNull()) {
        m_avatar->setPixmap(nt::circularAvatar(cached, kChannelRowAvatar));
        return;
    }
    m_thumbs->requestChannelAvatar(m_channelId, url);
}

void ChannelRow::setActionWidget(QWidget *widget)
{
    if (!widget)
        return;
    if (auto *lay = qobject_cast<QHBoxLayout *>(layout())) {
        lay->addWidget(widget, 0, Qt::AlignVCenter);
        m_hasAction = true;
    }
}

void ChannelRow::setActivate(std::function<void()> fn)
{
    m_activate = std::move(fn);
}

QLabel *ChannelRow::metaLabel() const
{
    return m_meta;
}

void ChannelRow::onThumbReady(const QString &key)
{
    if (key != m_avatarKey)
        return;
    const QPixmap pm = m_thumbs->pixmap(key);
    if (!pm.isNull())
        m_avatar->setPixmap(nt::circularAvatar(pm, kChannelRowAvatar));
}

void ChannelRow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    if (m_hover)
        p.fillRect(rect(), nt::kHover);
}

void ChannelRow::enterEvent(QEvent *event)
{
    Q_UNUSED(event);
    m_hover = true;
    update();
}

void ChannelRow::leaveEvent(QEvent *event)
{
    Q_UNUSED(event);
    m_hover = false;
    update();
}

void ChannelRow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        if (m_activate)
            m_activate();
        else
            emit activated();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}