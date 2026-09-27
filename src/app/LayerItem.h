#pragma once

#include "LayerStyle.h"
#include "MapData.h"

#include <QColor>
#include <QGraphicsItem>
#include <QHash>
#include <QPainterPath>
#include <QPixmap>
#include <QStringList>

#include <optional>
#include <vector>

/// Draws one map layer. Feature indices match the layer's `features` vector.
/// Features are grouped (see fh1::Feature::group); each group has a colour
/// and can be shown or hidden independently.
class LayerItem : public QGraphicsItem {
public:
    struct Hit {
        int feature = -1;
        /// Distance from the query point, in scene units.
        double distance = 0.0;
    };

    /// Text to draw beside a feature. The view gathers these from every layer
    /// each frame and places them so they do not overlap.
    struct Label {
        QPointF scenePos;
        QString text;
        /// Screen pixels between the anchor and the start of the text, so the
        /// text clears the feature's marker.
        double clearance = 0.0;
        /// Region names are centred on their anchor and drawn larger.
        bool region = false;
    };

    /// `layer` must outlive the item.
    LayerItem(const fh1::Layer& layer, const fh1::MapCalibration& calibration, int layerIndex);

    const fh1::Layer& layer() const { return m_layer; }
    int layerIndex() const { return m_layerIndex; }

    const LayerStyle& style() const { return m_style; }
    /// Group names ordered by feature count, largest first.
    const QStringList& groups() const { return m_style.groups; }
    int groupSize(int group) const { return m_style.groupSizes[static_cast<std::size_t>(group)]; }
    QColor groupColor(int group) const { return m_style.groupColors[static_cast<std::size_t>(group)]; }
    int groupOf(int feature) const { return m_style.featureGroup[static_cast<std::size_t>(feature)]; }
    int groupIndex(const QString& name) const { return static_cast<int>(m_style.groups.indexOf(name)); }
    bool isGroupVisible(int group) const { return m_groupVisible[static_cast<std::size_t>(group)]; }
    void setGroupVisible(int group, bool visible);
    bool isFeatureShown(int feature) const { return isVisible() && isGroupVisible(groupOf(feature)); }

    /// Draws `feature` emphasised; -1 clears the highlight.
    void setHighlightedFeature(int feature);
    int highlightedFeature() const { return m_highlighted; }

    /// Nearest shown feature within `tolerance` scene units of `scenePos`.
    virtual std::optional<Hit> hitTest(const QPointF& scenePos, double tolerance) const = 0;
    /// Scene-space bounds of one feature (a zero-size rect for points).
    virtual QRectF featureBounds(int feature) const = 0;
    /// Appends the labels of shown features anchored inside `sceneRect`, for
    /// a view at `scale` device pixels per scene unit.
    virtual void collectLabels(const QRectF& sceneRect, double scale, std::vector<Label>& out) const = 0;
    /// The icon every feature of `group` is drawn with, or a null pixmap when
    /// the group uses coloured markers.
    virtual QPixmap groupIcon(int group) const
    {
        Q_UNUSED(group);
        return {};
    }

protected:
    QPointF toScene(const QVector3D& world) const { return m_calibration.worldToImage(world.x(), world.z()); }

    const fh1::Layer& m_layer;
    fh1::MapCalibration m_calibration;
    int m_layerIndex;
    int m_highlighted = -1;

private:
    LayerStyle m_style;
    std::vector<bool> m_groupVisible;
};

/// Point features drawn as fixed-size screen markers, with a heading tick
/// when zoomed in, or as the game's map icon when the feature has one. A
/// uniform grid keeps painting and picking proportional to the visible area
/// rather than the layer size.
class PointLayerItem : public LayerItem {
public:
    PointLayerItem(
        const fh1::Layer& layer, const fh1::MapCalibration& calibration, int layerIndex, double markerRadius);

    /// Rings `features` as belonging with the highlighted feature, whether
    /// their groups are shown or not; empty for none.
    void setRelatedFeatures(std::vector<int> features);
    const std::vector<int>& relatedFeatures() const { return m_related; }

    /// Draws the features where they are now, after their positions or
    /// headings changed; the features themselves must be the same.
    void refreshPositions();

    /// Icons by fh1::Feature::icon key; features whose key is missing keep a
    /// coloured marker.
    void setIcons(const QHash<QString, QPixmap>& icons);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    std::optional<Hit> hitTest(const QPointF& scenePos, double tolerance) const override;
    QRectF featureBounds(int feature) const override;
    void collectLabels(const QRectF& sceneRect, double scale, std::vector<Label>& out) const override;
    QPixmap groupIcon(int group) const override;

private:
    template <typename Visit> void forEachInRect(const QRectF& rect, Visit&& visit) const;
    /// Scene positions, headings and the lookup grid, from the layer.
    void buildGeometry();
    /// Marker radius and icon size in device pixels, shrinking when zoomed out.
    double markerRadiusAt(double scale) const;
    static double iconSizeAt(double scale);

    std::vector<QPixmap> m_iconPixmaps;
    /// Index into m_iconPixmaps per feature, or -1.
    std::vector<int> m_featureIcon;
    /// Index into m_iconPixmaps per group when all its features share one, or -1.
    std::vector<int> m_groupIcon;

    std::vector<QPointF> m_points;
    /// Unit heading in scene space, or (0, 0) when the feature has none.
    std::vector<QPointF> m_headings;
    QRectF m_bounds;
    double m_cellSize = 64.0;
    int m_columns = 0;
    int m_rows = 0;
    std::vector<std::vector<int>> m_cells;
    double m_radius;
    std::vector<int> m_related;
};

/// Polyline and polygon features drawn with cosmetic (zoom-independent) pens.
class ShapeLayerItem : public LayerItem {
public:
    ShapeLayerItem(const fh1::Layer& layer, const fh1::MapCalibration& calibration, int layerIndex);

    /// Multiplies the width lines are drawn with, so a layer can stand out
    /// from the others.
    void setLineWidthScale(double scale);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;
    std::optional<Hit> hitTest(const QPointF& scenePos, double tolerance) const override;
    QRectF featureBounds(int feature) const override;
    void collectLabels(const QRectF& sceneRect, double scale, std::vector<Label>& out) const override;

private:
    bool isPolygon() const { return m_layer.kind == fh1::FeatureKind::Polygon; }

    std::vector<QPainterPath> m_paths;
    std::vector<std::vector<std::vector<QPointF>>> m_sceneShapes;
    QRectF m_bounds;
    double m_lineWidthScale = 1.0;
};
