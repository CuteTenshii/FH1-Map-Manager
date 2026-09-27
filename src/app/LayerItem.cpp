#include "LayerItem.h"

#include <QHash>
#include <QPainter>
#include <QStyleOptionGraphicsItem>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

double distanceToSegment(const QPointF& p, const QPointF& a, const QPointF& b)
{
    const QPointF ab = b - a;
    const double lengthSquared = QPointF::dotProduct(ab, ab);
    double t = lengthSquared > 0.0 ? QPointF::dotProduct(p - a, ab) / lengthSquared : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF closest = a + ab * t;
    return std::hypot(p.x() - closest.x(), p.y() - closest.y());
}

/// Pixels per scene unit for the painter's current view transform.
double deviceScale(const QPainter* painter)
{
    const QTransform& t = painter->worldTransform();
    return std::hypot(t.m11(), t.m12());
}

} // namespace

LayerItem::LayerItem(const fh1::Layer& layer, const fh1::MapCalibration& calibration, int layerIndex)
    : m_layer(layer)
    , m_calibration(calibration)
    , m_layerIndex(layerIndex)
    , m_style(LayerStyle::of(layer, layerIndex))
    , m_groupVisible(static_cast<std::size_t>(m_style.groups.size()), true)
{
    setFlag(QGraphicsItem::ItemUsesExtendedStyleOption);
}

void LayerItem::setGroupVisible(int group, bool visible)
{
    auto&& flag = m_groupVisible[static_cast<std::size_t>(group)];
    if (flag != visible) {
        flag = visible;
        update();
    }
}

void LayerItem::setHighlightedFeature(int feature)
{
    if (m_highlighted != feature) {
        m_highlighted = feature;
        update();
    }
}

PointLayerItem::PointLayerItem(
    const fh1::Layer& layer, const fh1::MapCalibration& calibration, int layerIndex, double markerRadius)
    : LayerItem(layer, calibration, layerIndex)
    , m_radius(markerRadius)
{
    m_points.reserve(layer.features.size());
    m_headings.reserve(layer.features.size());
    // QRectF::united() ignores zero-size rectangles, so the bounds of a point
    // set are accumulated from explicit minima and maxima.
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    for (const fh1::Feature& feature : layer.features) {
        const QPointF p = toScene(feature.position);
        m_points.push_back(p);
        minX = std::min(minX, p.x());
        minY = std::min(minY, p.y());
        maxX = std::max(maxX, p.x());
        maxY = std::max(maxY, p.y());
        QPointF heading;
        if (!feature.forward.isNull()) {
            const QPointF ahead = toScene(feature.position + feature.forward);
            const QPointF d = ahead - p;
            const double length = std::hypot(d.x(), d.y());
            if (length > 1e-9) {
                heading = d / length;
            }
        }
        m_headings.push_back(heading);
    }
    if (m_points.empty()) {
        return;
    }
    m_bounds = QRectF(QPointF(minX, minY), QPointF(maxX, maxY)).adjusted(-1.0, -1.0, 1.0, 1.0);
    m_columns = std::max(1, static_cast<int>(std::ceil(m_bounds.width() / m_cellSize)));
    m_rows = std::max(1, static_cast<int>(std::ceil(m_bounds.height() / m_cellSize)));
    m_cells.resize(static_cast<std::size_t>(m_columns) * static_cast<std::size_t>(m_rows));
    for (std::size_t i = 0; i < m_points.size(); ++i) {
        const int cx = std::clamp(static_cast<int>((m_points[i].x() - m_bounds.left()) / m_cellSize), 0, m_columns - 1);
        const int cy = std::clamp(static_cast<int>((m_points[i].y() - m_bounds.top()) / m_cellSize), 0, m_rows - 1);
        m_cells[static_cast<std::size_t>(cy) * static_cast<std::size_t>(m_columns) + static_cast<std::size_t>(cx)]
            .push_back(static_cast<int>(i));
    }
}

template <typename Visit> void PointLayerItem::forEachInRect(const QRectF& rect, Visit&& visit) const
{
    if (m_cells.empty()) {
        return;
    }
    const QRectF r = rect.intersected(m_bounds);
    if (r.isEmpty()) {
        return;
    }
    const int x0 = std::clamp(static_cast<int>((r.left() - m_bounds.left()) / m_cellSize), 0, m_columns - 1);
    const int x1 = std::clamp(static_cast<int>((r.right() - m_bounds.left()) / m_cellSize), 0, m_columns - 1);
    const int y0 = std::clamp(static_cast<int>((r.top() - m_bounds.top()) / m_cellSize), 0, m_rows - 1);
    const int y1 = std::clamp(static_cast<int>((r.bottom() - m_bounds.top()) / m_cellSize), 0, m_rows - 1);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            for (int index : m_cells[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_columns)
                     + static_cast<std::size_t>(x)]) {
                visit(index);
            }
        }
    }
}

QRectF PointLayerItem::boundingRect() const
{
    // Markers have a fixed on-screen size, so at the furthest zoom-out they
    // cover many scene units; the margin keeps them inside the item's bounds.
    constexpr double kMarkerMargin = 64.0;
    return m_bounds.adjusted(-kMarkerMargin, -kMarkerMargin, kMarkerMargin, kMarkerMargin);
}

double PointLayerItem::markerRadiusAt(double scale) const
{
    // Shrink markers when zoomed out so dense areas stay readable.
    return m_radius * std::clamp(std::sqrt(scale / 0.8), 0.55, 1.0);
}

double PointLayerItem::iconSizeAt(double scale)
{
    return std::clamp(28.0 * std::sqrt(scale / 1.2), 18.0, 30.0);
}

void PointLayerItem::setIcons(const QHash<QString, QPixmap>& icons)
{
    m_iconPixmaps.clear();
    QHash<QString, int> indexByKey;
    m_featureIcon.assign(m_layer.features.size(), -1);
    for (std::size_t i = 0; i < m_layer.features.size(); ++i) {
        const QString& key = m_layer.features[i].icon;
        if (key.isEmpty() || !icons.contains(key)) {
            continue;
        }
        auto it = indexByKey.find(key);
        if (it == indexByKey.end()) {
            it = indexByKey.insert(key, static_cast<int>(m_iconPixmaps.size()));
            m_iconPixmaps.push_back(icons.value(key));
        }
        m_featureIcon[i] = it.value();
    }

    constexpr int kUnset = -2;
    m_groupIcon.assign(static_cast<std::size_t>(groups().size()), kUnset);
    for (std::size_t i = 0; i < m_featureIcon.size(); ++i) {
        int& groupIcon = m_groupIcon[static_cast<std::size_t>(groupOf(static_cast<int>(i)))];
        if (groupIcon == kUnset) {
            groupIcon = m_featureIcon[i];
        } else if (groupIcon != m_featureIcon[i]) {
            groupIcon = -1;
        }
    }
    for (int& groupIcon : m_groupIcon) {
        groupIcon = std::max(groupIcon, -1);
    }
    update();
}

QPixmap PointLayerItem::groupIcon(int group) const
{
    if (group < 0 || static_cast<std::size_t>(group) >= m_groupIcon.size()) {
        return {};
    }
    const int icon = m_groupIcon[static_cast<std::size_t>(group)];
    return icon < 0 ? QPixmap() : m_iconPixmaps[static_cast<std::size_t>(icon)];
}

void PointLayerItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
    Q_UNUSED(widget);
    const double scale = deviceScale(painter);
    if (scale <= 0.0) {
        return;
    }
    const double radius = markerRadiusAt(scale);
    const double iconSize = iconSizeAt(scale);
    const double marginScene = (std::max(radius, iconSize / 2.0) + 12.0) / scale;
    const QRectF exposed = option->exposedRect.adjusted(-marginScene, -marginScene, marginScene, marginScene);
    const QTransform toDevice = painter->worldTransform();
    // Heading ticks only help once markers are far enough apart to read them.
    const bool drawHeadings = scale >= 0.9;
    const bool haveIcons = !m_featureIcon.empty();

    std::vector<std::vector<QPointF>> byGroup(static_cast<std::size_t>(groups().size()));
    std::vector<std::pair<QPointF, int>> iconMarkers;
    std::vector<QLineF> headingLines;
    forEachInRect(exposed, [&](int index) {
        const int group = groupOf(index);
        if (!isGroupVisible(group)) {
            return;
        }
        const QPointF device = toDevice.map(m_points[static_cast<std::size_t>(index)]);
        const int icon = haveIcons ? m_featureIcon[static_cast<std::size_t>(index)] : -1;
        if (icon >= 0) {
            iconMarkers.emplace_back(device, icon);
            return;
        }
        byGroup[static_cast<std::size_t>(group)].push_back(device);
        const QPointF heading = m_headings[static_cast<std::size_t>(index)];
        if (drawHeadings && !heading.isNull()) {
            headingLines.emplace_back(device, device + heading * (radius + 7.0));
        }
    });

    painter->save();
    painter->resetTransform();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    const double diameter = radius * 2.0;
    if (!headingLines.empty()) {
        painter->setPen(QPen(QColor(0, 0, 0, 200), 3.0, Qt::SolidLine, Qt::RoundCap));
        painter->drawLines(headingLines.data(), static_cast<int>(headingLines.size()));
        painter->setPen(QPen(Qt::white, 1.5, Qt::SolidLine, Qt::RoundCap));
        painter->drawLines(headingLines.data(), static_cast<int>(headingLines.size()));
    }
    // A dark halo under each marker keeps light colours readable on bright
    // terrain such as snow and salt flats.
    for (std::size_t group = 0; group < byGroup.size(); ++group) {
        const auto& points = byGroup[group];
        if (points.empty()) {
            continue;
        }
        painter->setPen(QPen(QColor(0, 0, 0, 190), diameter + 2.0, Qt::SolidLine, Qt::RoundCap));
        painter->drawPoints(points.data(), static_cast<int>(points.size()));
    }
    for (std::size_t group = 0; group < byGroup.size(); ++group) {
        const auto& points = byGroup[group];
        if (points.empty()) {
            continue;
        }
        painter->setPen(QPen(groupColor(static_cast<int>(group)), diameter, Qt::SolidLine, Qt::RoundCap));
        painter->drawPoints(points.data(), static_cast<int>(points.size()));
    }
    for (const auto& [device, icon] : iconMarkers) {
        const QRectF target(device.x() - iconSize / 2.0, device.y() - iconSize / 2.0, iconSize, iconSize);
        painter->drawPixmap(target, m_iconPixmaps[static_cast<std::size_t>(icon)], QRectF());
    }

    if (m_highlighted >= 0 && isFeatureShown(m_highlighted)) {
        const QPointF device = toDevice.map(m_points[static_cast<std::size_t>(m_highlighted)]);
        const bool hasIcon = haveIcons && m_featureIcon[static_cast<std::size_t>(m_highlighted)] >= 0;
        const double ring = (hasIcon ? iconSize / 2.0 : radius) + 5.0;
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(Qt::black, 5.0));
        painter->drawEllipse(device, ring, ring);
        painter->setPen(QPen(Qt::white, 2.5));
        painter->drawEllipse(device, ring, ring);
    }
    painter->restore();
}

void PointLayerItem::collectLabels(const QRectF& sceneRect, double scale, std::vector<Label>& out) const
{
    if (!isVisible()) {
        return;
    }
    const double radius = markerRadiusAt(scale);
    const double iconSize = iconSizeAt(scale);
    forEachInRect(sceneRect, [&](int index) {
        const fh1::Feature& feature = m_layer.features[static_cast<std::size_t>(index)];
        if (feature.label.isEmpty() || !isGroupVisible(groupOf(index))) {
            return;
        }
        const bool hasIcon = !m_featureIcon.empty() && m_featureIcon[static_cast<std::size_t>(index)] >= 0;
        out.push_back(
            {m_points[static_cast<std::size_t>(index)], feature.label, hasIcon ? iconSize / 2.0 : radius, false});
    });
}

std::optional<LayerItem::Hit> PointLayerItem::hitTest(const QPointF& scenePos, double tolerance) const
{
    if (!isVisible()) {
        return std::nullopt;
    }
    std::optional<Hit> best;
    const QRectF area(scenePos.x() - tolerance, scenePos.y() - tolerance, tolerance * 2.0, tolerance * 2.0);
    forEachInRect(area, [&](int index) {
        if (!isGroupVisible(groupOf(index))) {
            return;
        }
        const QPointF p = m_points[static_cast<std::size_t>(index)];
        const double distance = std::hypot(p.x() - scenePos.x(), p.y() - scenePos.y());
        if (distance <= tolerance && (!best || distance < best->distance)) {
            best = Hit{index, distance};
        }
    });
    return best;
}

QRectF PointLayerItem::featureBounds(int feature) const
{
    return {m_points[static_cast<std::size_t>(feature)], QSizeF(0.0, 0.0)};
}

ShapeLayerItem::ShapeLayerItem(const fh1::Layer& layer, const fh1::MapCalibration& calibration, int layerIndex)
    : LayerItem(layer, calibration, layerIndex)
{
    m_paths.reserve(layer.features.size());
    m_sceneShapes.reserve(layer.features.size());
    for (const fh1::Feature& feature : layer.features) {
        QPainterPath path;
        path.setFillRule(Qt::WindingFill);
        std::vector<std::vector<QPointF>> sceneShapes;
        for (const auto& shape : feature.shapes) {
            std::vector<QPointF> scenePoints;
            scenePoints.reserve(shape.size());
            for (const QVector3D& point : shape) {
                scenePoints.push_back(toScene(point));
            }
            if (scenePoints.empty()) {
                continue;
            }
            path.moveTo(scenePoints.front());
            for (std::size_t i = 1; i < scenePoints.size(); ++i) {
                path.lineTo(scenePoints[i]);
            }
            if (isPolygon()) {
                path.closeSubpath();
            }
            sceneShapes.push_back(std::move(scenePoints));
        }
        if (isPolygon()) {
            // Zones are stored as triangle soups; merging them leaves only the
            // zone's outline, both for drawing and for edge hit tests.
            path = path.simplified();
            sceneShapes.clear();
            for (const QPolygonF& outline : path.toSubpathPolygons()) {
                sceneShapes.emplace_back(outline.begin(), outline.end());
            }
        }
        m_bounds = m_bounds.united(path.boundingRect());
        m_paths.push_back(std::move(path));
        m_sceneShapes.push_back(std::move(sceneShapes));
    }
}

QRectF ShapeLayerItem::boundingRect() const
{
    constexpr double kPenMargin = 16.0;
    return m_bounds.adjusted(-kPenMargin, -kPenMargin, kPenMargin, kPenMargin);
}

void ShapeLayerItem::setLineWidthScale(double scale)
{
    if (m_lineWidthScale != scale) {
        m_lineWidthScale = scale;
        update();
    }
}

void ShapeLayerItem::paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget)
{
    Q_UNUSED(widget);
    const QRectF exposed = option->exposedRect;
    painter->setRenderHint(QPainter::Antialiasing, true);

    auto drawFeature = [&](std::size_t i, bool highlighted) {
        const QColor color = groupColor(groupOf(static_cast<int>(i)));
        if (isPolygon()) {
            QColor fill = color;
            fill.setAlpha(highlighted ? 110 : 55);
            painter->setBrush(fill);
            QPen pen(highlighted ? QColor(Qt::white) : color, highlighted ? 3.0 : 1.5);
            pen.setCosmetic(true);
            painter->setPen(pen);
            painter->drawPath(m_paths[i]);
            return;
        }
        painter->setBrush(Qt::NoBrush);
        QPen halo(QColor(0, 0, 0, 170), (highlighted ? 7.0 : 4.5) * m_lineWidthScale, Qt::SolidLine, Qt::RoundCap,
            Qt::RoundJoin);
        halo.setCosmetic(true);
        painter->setPen(halo);
        painter->drawPath(m_paths[i]);
        QPen pen(highlighted ? QColor(Qt::white) : color, (highlighted ? 4.0 : 2.5) * m_lineWidthScale, Qt::SolidLine,
            Qt::RoundCap, Qt::RoundJoin);
        pen.setCosmetic(true);
        painter->setPen(pen);
        painter->drawPath(m_paths[i]);
    };

    for (std::size_t i = 0; i < m_paths.size(); ++i) {
        if (static_cast<int>(i) == m_highlighted || !isGroupVisible(groupOf(static_cast<int>(i)))) {
            continue;
        }
        if (m_paths[i].boundingRect().intersects(exposed)) {
            drawFeature(i, false);
        }
    }
    if (m_highlighted >= 0 && isFeatureShown(m_highlighted)) {
        drawFeature(static_cast<std::size_t>(m_highlighted), true);
    }
}

std::optional<LayerItem::Hit> ShapeLayerItem::hitTest(const QPointF& scenePos, double tolerance) const
{
    if (!isVisible()) {
        return std::nullopt;
    }
    std::optional<Hit> best;
    for (std::size_t i = 0; i < m_paths.size(); ++i) {
        if (!isGroupVisible(groupOf(static_cast<int>(i)))) {
            continue;
        }
        if (!m_paths[i].boundingRect().adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(scenePos)) {
            continue;
        }
        double distance = std::numeric_limits<double>::max();
        if (isPolygon() && m_paths[i].contains(scenePos)) {
            // Zones cover large areas; ranking a click inside one at the edge of
            // the tolerance lets any nearby marker or line win over it.
            distance = tolerance;
        } else {
            for (const auto& shape : m_sceneShapes[i]) {
                const std::size_t n = shape.size();
                for (std::size_t k = 1; k < n; ++k) {
                    distance = std::min(distance, distanceToSegment(scenePos, shape[k - 1], shape[k]));
                }
                if (isPolygon() && n > 2) {
                    distance = std::min(distance, distanceToSegment(scenePos, shape[n - 1], shape[0]));
                }
            }
        }
        if (distance <= tolerance && (!best || distance < best->distance)) {
            best = Hit{static_cast<int>(i), distance};
        }
    }
    return best;
}

QRectF ShapeLayerItem::featureBounds(int feature) const
{
    return m_paths[static_cast<std::size_t>(feature)].boundingRect();
}

void ShapeLayerItem::collectLabels(const QRectF& sceneRect, double scale, std::vector<Label>& out) const
{
    Q_UNUSED(scale);
    if (!isVisible()) {
        return;
    }
    for (std::size_t i = 0; i < m_layer.features.size(); ++i) {
        const fh1::Feature& feature = m_layer.features[i];
        if (feature.label.isEmpty() || !isGroupVisible(groupOf(static_cast<int>(i))) || m_sceneShapes[i].empty()) {
            continue;
        }
        // Routes are labelled where they start; zones at their centre.
        const QPointF anchor = isPolygon() ? toScene(feature.position) : m_sceneShapes[i].front().front();
        if (sceneRect.contains(anchor)) {
            out.push_back({anchor, feature.label, isPolygon() ? 0.0 : 6.0, isPolygon()});
        }
    }
}
