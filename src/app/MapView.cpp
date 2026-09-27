#include "MapView.h"

#include "LayerItem.h"

#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {

constexpr double kMaxZoom = 16.0;
constexpr double kWheelStep = 1.0015;
/// A press that moves less than this is a click, anything more is a pan.
constexpr int kClickSlop = 4;
/// Zoom (device pixels per scene unit) from which point and route labels are
/// drawn; region names are drawn at every zoom.
constexpr double kLabelZoom = 0.3;
/// Upper bound on labels per frame, to keep dense views legible and fast.
constexpr std::size_t kMaxLabels = 400;

void drawHaloText(QPainter* painter, const QPointF& baseline, const QFont& font, const QString& text,
    const QColor& fill, const QColor& halo)
{
    QPainterPath path;
    path.addText(baseline, font, text);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(halo, 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->drawPath(path);
    painter->setPen(Qt::NoPen);
    painter->setBrush(fill);
    painter->drawPath(path);
}

} // namespace

MapView::MapView(QWidget* parent)
    : QGraphicsView(parent)
{
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    // Markers are drawn at a fixed screen size and can reach past their items'
    // scene bounds when zoomed out, so partial updates would leave trails.
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    setOptimizationFlag(QGraphicsView::DontSavePainterState, true);
    setBackgroundBrush(Qt::black);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

void MapView::setMetresPerSceneUnit(double metres)
{
    m_metresPerSceneUnit = metres;
    viewport()->update();
}

double MapView::zoom() const
{
    return std::hypot(transform().m11(), transform().m12());
}

double MapView::minimumZoom() const
{
    const QRectF scene = sceneRect();
    if (scene.isEmpty()) {
        return 0.01;
    }
    const QSize view = viewport()->size();
    return 0.5 * std::min(view.width() / scene.width(), view.height() / scene.height());
}

void MapView::zoomBy(double factor)
{
    const double current = zoom();
    const double target = std::clamp(current * factor, minimumZoom(), kMaxZoom);
    if (std::abs(target - current) < 1e-12) {
        return;
    }
    scale(target / current, target / current);
    m_fitted = false;
    emit zoomChanged(zoom());
}

void MapView::fitScene()
{
    if (sceneRect().isEmpty()) {
        return;
    }
    fitInView(sceneRect(), Qt::KeepAspectRatio);
    m_fitted = true;
    emit zoomChanged(zoom());
}

void MapView::focusOn(const QRectF& rect, double maxZoom)
{
    centreAt(rect, std::max(zoomToFit(rect, maxZoom), zoom()));
}

void MapView::fitRect(const QRectF& rect, double maxZoom)
{
    centreAt(rect, zoomToFit(rect, maxZoom));
}

double MapView::zoomToFit(const QRectF& rect, double maxZoom) const
{
    const QSize view = viewport()->size();
    double target = maxZoom;
    if (rect.width() > 0.0 && rect.height() > 0.0) {
        // Leave a margin around lines and zones so their ends stay visible.
        target = std::min(target, 0.8 * std::min(view.width() / rect.width(), view.height() / rect.height()));
    }
    return target;
}

void MapView::centreAt(const QRectF& rect, double zoom)
{
    const double target = std::clamp(zoom, minimumZoom(), kMaxZoom);
    setTransform(QTransform::fromScale(target, target));
    centerOn(rect.center());
    m_fitted = false;
    emit zoomChanged(this->zoom());
}

void MapView::setLabelSources(std::vector<const LayerItem*> sources)
{
    m_labelSources = std::move(sources);
    viewport()->update();
}

void MapView::setLabelsVisible(bool visible)
{
    m_labelsVisible = visible;
    viewport()->update();
}

void MapView::wheelEvent(QWheelEvent* event)
{
    const int delta = event->angleDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }
    zoomBy(std::pow(kWheelStep, delta));
    event->accept();
}

void MapView::setGrabTest(std::function<bool(const QPointF&)> test)
{
    m_grabTest = std::move(test);
    if (!m_grabTest && m_overGrabbable) {
        m_overGrabbable = false;
        viewport()->setCursor(Qt::OpenHandCursor);
    }
}

void MapView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_grabTest && m_grabTest(mapToScene(event->position().toPoint()))) {
        m_grabbing = true;
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        m_pressPos = event->position().toPoint();
        m_pressed = true;
    }
    QGraphicsView::mousePressEvent(event);
}

void MapView::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF scenePos = mapToScene(event->position().toPoint());
    emit cursorMoved(scenePos);
    if (m_grabbing) {
        emit grabMoved(scenePos, event->modifiers());
        event->accept();
        return;
    }
    if (event->buttons() == Qt::NoButton && m_grabTest) {
        const bool over = m_grabTest(scenePos);
        if (over != m_overGrabbable) {
            m_overGrabbable = over;
            viewport()->setCursor(over ? Qt::SizeAllCursor : Qt::OpenHandCursor);
        }
    }
    QGraphicsView::mouseMoveEvent(event);
}

void MapView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_grabbing && event->button() == Qt::LeftButton) {
        m_grabbing = false;
        emit grabReleased(mapToScene(event->position().toPoint()), event->modifiers());
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
    if (event->button() != Qt::LeftButton || !m_pressed) {
        return;
    }
    m_pressed = false;
    const QPoint position = event->position().toPoint();
    if ((position - m_pressPos).manhattanLength() <= kClickSlop) {
        emit clicked(mapToScene(position));
    } else {
        m_fitted = false;
    }
}

void MapView::leaveEvent(QEvent* event)
{
    emit cursorLeft();
    QGraphicsView::leaveEvent(event);
}

void MapView::contextMenuEvent(QContextMenuEvent* event)
{
    emit contextMenuRequested(mapToScene(event->pos()), event->globalPos());
    event->accept();
}

void MapView::resizeEvent(QResizeEvent* event)
{
    QGraphicsView::resizeEvent(event);
    if (m_fitted) {
        fitScene();
    }
}

void MapView::drawForeground(QPainter* painter, const QRectF& rect)
{
    Q_UNUSED(rect);
    if (scene() == nullptr || sceneRect().isEmpty()) {
        return;
    }
    painter->save();
    painter->resetTransform();
    if (m_labelsVisible) {
        drawLabels(painter);
    }
    drawScaleBar(painter);
    painter->restore();
}

void MapView::drawLabels(QPainter* painter)
{
    const double scale = zoom();
    const QRectF visible = mapToScene(viewport()->rect()).boundingRect();
    std::vector<LayerItem::Label> labels;
    for (const LayerItem* source : m_labelSources) {
        source->collectLabels(visible, scale, labels);
    }
    // Region names go last so object and route names take precedence.
    std::stable_partition(labels.begin(), labels.end(), [](const LayerItem::Label& l) { return !l.region; });

    QFont labelFont = font();
    QFont regionFont = font();
    regionFont.setBold(true);
    regionFont.setPointSizeF(regionFont.pointSizeF() * 1.3);
    const QFontMetricsF labelMetrics(labelFont);
    const QFontMetricsF regionMetrics(regionFont);
    const QTransform toDevice = viewportTransform();

    painter->setRenderHint(QPainter::Antialiasing, true);
    std::vector<QRectF> placed;
    for (const LayerItem::Label& label : labels) {
        if (placed.size() >= kMaxLabels) {
            break;
        }
        if (!label.region && scale < kLabelZoom) {
            continue;
        }
        const QFontMetricsF& metrics = label.region ? regionMetrics : labelMetrics;
        const QPointF anchor = toDevice.map(label.scenePos);
        const double width = metrics.horizontalAdvance(label.text);
        const double height = metrics.height();
        const QRectF rect = label.region
            ? QRectF(anchor.x() - width / 2.0, anchor.y() - height / 2.0, width, height)
            : QRectF(anchor.x() + label.clearance + 3.0, anchor.y() - height / 2.0, width, height);
        const QRectF padded = rect.adjusted(-3.0, -1.0, 3.0, 1.0);
        const bool overlaps
            = std::any_of(placed.begin(), placed.end(), [&](const QRectF& r) { return r.intersects(padded); });
        if (overlaps) {
            continue;
        }
        placed.push_back(padded);
        const QPointF baseline(rect.left(), rect.top() + metrics.ascent());
        if (label.region) {
            drawHaloText(painter, baseline, regionFont, label.text, QColor(255, 255, 255, 215), QColor(0, 0, 0, 150));
        } else {
            drawHaloText(painter, baseline, labelFont, label.text, Qt::white, QColor(0, 0, 0, 220));
        }
    }
}

void MapView::drawScaleBar(QPainter* painter)
{
    const double metresPerPixel = m_metresPerSceneUnit / zoom();
    if (!std::isfinite(metresPerPixel) || metresPerPixel <= 0.0) {
        return;
    }
    constexpr double kMaxBarPixels = 140.0;
    const double maxMetres = metresPerPixel * kMaxBarPixels;
    const double magnitude = std::pow(10.0, std::floor(std::log10(maxMetres)));
    double metres = magnitude;
    for (double step : std::array{5.0, 2.0, 1.0}) {
        if (step * magnitude <= maxMetres) {
            metres = step * magnitude;
            break;
        }
    }
    const double pixels = metres / metresPerPixel;
    const QString label
        = metres >= 1000.0 ? QStringLiteral("%1 km").arg(metres / 1000.0) : QStringLiteral("%1 m").arg(metres);

    const QFontMetrics metrics(font());
    const int margin = 14;
    const QPointF origin(margin, viewport()->height() - margin);
    const QPointF end = origin + QPointF(pixels, 0.0);
    const QLineF bar(origin, end);
    const std::array ticks{QLineF(origin, origin - QPointF(0.0, 6.0)), QLineF(end, end - QPointF(0.0, 6.0))};

    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QPen(QColor(0, 0, 0, 200), 5.0, Qt::SolidLine, Qt::SquareCap));
    painter->drawLine(bar);
    painter->drawLines(ticks.data(), static_cast<int>(ticks.size()));
    painter->setPen(QPen(Qt::white, 2.0, Qt::SolidLine, Qt::SquareCap));
    painter->drawLine(bar);
    painter->drawLines(ticks.data(), static_cast<int>(ticks.size()));

    const QPointF textPos = origin + QPointF(pixels + 8.0, metrics.ascent() / 2.0 - 3.0);
    drawHaloText(painter, textPos, font(), label, Qt::white, QColor(0, 0, 0, 220));
}
