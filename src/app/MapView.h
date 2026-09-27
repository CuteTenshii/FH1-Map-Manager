#pragma once

#include <QGraphicsView>
#include <QPoint>

#include <vector>

class LayerItem;

/// Pan-and-zoom view of the map scene. Scene units are map-image pixels;
/// setMetresPerSceneUnit() tells the view how to label its scale bar.
class MapView : public QGraphicsView {
    Q_OBJECT

public:
    explicit MapView(QWidget* parent = nullptr);

    void setMetresPerSceneUnit(double metres);
    /// Device pixels per scene unit.
    double zoom() const;

    void zoomBy(double factor);
    void fitScene();
    /// Centres on `rect`, zooming in no further than `maxZoom`.
    void focusOn(const QRectF& rect, double maxZoom);
    /// Centres on `rect` and zooms in or out to fit it, zooming in no
    /// further than `maxZoom`.
    void fitRect(const QRectF& rect, double maxZoom);

    /// Layers whose labels are drawn over the map, top-most first; earlier
    /// layers win when labels would overlap. The items must outlive their use
    /// here, so clear the sources before deleting them.
    void setLabelSources(std::vector<const LayerItem*> sources);
    void setLabelsVisible(bool visible);
    bool labelsVisible() const { return m_labelsVisible; }

signals:
    void cursorMoved(const QPointF& scenePos);
    void cursorLeft();
    void clicked(const QPointF& scenePos);
    void contextMenuRequested(const QPointF& scenePos, const QPoint& globalPos);
    void zoomChanged(double zoom);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void drawForeground(QPainter* painter, const QRectF& rect) override;

private:
    double minimumZoom() const;
    /// Zoom that fits `rect` with a margin, or `maxZoom` if that is closer.
    double zoomToFit(const QRectF& rect, double maxZoom) const;
    void centreAt(const QRectF& rect, double zoom);
    void drawScaleBar(QPainter* painter);
    void drawLabels(QPainter* painter);

    double m_metresPerSceneUnit = 1.0;
    std::vector<const LayerItem*> m_labelSources;
    bool m_labelsVisible = true;
    QPoint m_pressPos;
    bool m_pressed = false;
    bool m_fitted = true;
};
