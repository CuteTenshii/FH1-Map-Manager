#include "BackgroundItem.h"

#include <QPainter>
#include <QStyleOptionGraphicsItem>

#include <algorithm>
#include <cmath>

BackgroundItem::BackgroundItem(const QImage& image)
    : m_size(image.size())
{
    constexpr int kSmallestLevelWidth = 512;
    QImage level = image.convertToFormat(QImage::Format_RGB32);
    m_levels.push_back(QPixmap::fromImage(level));
    while (level.width() / 2 >= kSmallestLevelWidth) {
        level = level.scaled(level.width() / 2, level.height() / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        m_levels.push_back(QPixmap::fromImage(level));
    }
    setFlag(QGraphicsItem::ItemUsesExtendedStyleOption);
}

QRectF BackgroundItem::boundingRect() const
{
    return {QPointF(0.0, 0.0), m_size};
}

void BackgroundItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
    Q_UNUSED(widget);
    const QTransform& t = painter->worldTransform();
    const double scale = std::hypot(t.m11(), t.m12());
    // Level k is 2^-k of full size; pick the smallest one that still has at
    // least one texel per device pixel.
    int level = 0;
    if (scale > 0.0 && scale < 1.0) {
        level = static_cast<int>(std::floor(std::log2(1.0 / scale)));
    }
    level = std::clamp(level, 0, static_cast<int>(m_levels.size()) - 1);

    const QPixmap& pixmap = m_levels[static_cast<std::size_t>(level)];
    const QRectF target = option->exposedRect.intersected(boundingRect());
    if (target.isEmpty()) {
        return;
    }
    const double sx = pixmap.width() / m_size.width();
    const double sy = pixmap.height() / m_size.height();
    const QRectF source(target.left() * sx, target.top() * sy, target.width() * sx, target.height() * sy);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->drawPixmap(target, pixmap, source);
}
