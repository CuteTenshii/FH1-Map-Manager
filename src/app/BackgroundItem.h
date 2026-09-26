#pragma once

#include <QGraphicsItem>
#include <QImage>
#include <QPixmap>

#include <vector>

/// The satellite map image. Keeps a mip chain so zoomed-out views sample a
/// downscaled copy instead of filtering the full 5120x3072 image every frame.
class BackgroundItem : public QGraphicsItem {
public:
    explicit BackgroundItem(const QImage& image);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

private:
    std::vector<QPixmap> m_levels;
    QSizeF m_size;
};
