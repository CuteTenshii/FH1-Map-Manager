#include "Races.h"

#include <algorithm>
#include <array>
#include <utility>

namespace fh1 {

namespace {

const QColor kRouteColour(0xFF, 0xB0, 0x00);
const QColor kStartColour(0x3D, 0xDC, 0x97);
const QColor kCheckpointColour(0x4E, 0x9B, 0xFF);
const QColor kWaypointColour(0xB5, 0xB5, 0xFF);
const QColor kFinishColour(0xFF, 0x5A, 0x5F);

/// The number a transform name ends with ("route_checkpoint_12" -> 12,
/// "route_checkpoint_indicator_12b" -> 12), or -1.
int trailingNumber(const QString& name)
{
    const qsizetype underscore = name.lastIndexOf(QLatin1Char('_'));
    qsizetype end = underscore + 1;
    while (end < name.size() && name.at(end).isDigit()) {
        ++end;
    }
    if (end == underscore + 1) {
        return -1;
    }
    return name.mid(underscore + 1, end - underscore - 1).toInt();
}

QString formatPoint(const QVector3D& v)
{
    return QStringLiteral("%1, %2, %3")
        .arg(static_cast<double>(v.x()), 0, 'f', 2)
        .arg(static_cast<double>(v.y()), 0, 'f', 2)
        .arg(static_cast<double>(v.z()), 0, 'f', 2);
}

Feature pointFeature(const RouteTransform& transform, const QString& group, const QString& label)
{
    Feature feature;
    feature.name = transform.name;
    feature.label = label;
    feature.group = group;
    feature.position = transform.position;
    feature.forward = transform.facing;
    feature.properties = {
        {QStringLiteral("Position"), formatPoint(transform.position)},
        {QStringLiteral("Facing"), formatPoint(transform.facing)},
    };
    return feature;
}

} // namespace

RoutePointKind routePointKind(const QString& transformName)
{
    // Longer names first where one starts with another.
    static const std::array<std::pair<QLatin1StringView, RoutePointKind>, 9> prefixes{{
        {QLatin1StringView("start_location"), RoutePointKind::StartSlot},
        {QLatin1StringView("start_gantry"), RoutePointKind::StartGantry},
        {QLatin1StringView("route_waypoint"), RoutePointKind::Waypoint},
        {QLatin1StringView("route_checkpoint_indicator"), RoutePointKind::CheckpointMarker},
        {QLatin1StringView("route_checkpoint"), RoutePointKind::Checkpoint},
        {QLatin1StringView("route_boundary"), RoutePointKind::Boundary},
        {QLatin1StringView("end_race_cannon_trigger"), RoutePointKind::Finish},
        {QLatin1StringView("end_race_cannon"), RoutePointKind::FinishCannon},
        {QLatin1StringView("post_race_location"), RoutePointKind::PostRace},
    }};
    for (const auto& [prefix, kind] : prefixes) {
        if (transformName.startsWith(prefix, Qt::CaseInsensitive)) {
            return kind;
        }
    }
    return RoutePointKind::Other;
}

std::vector<const RouteTransform*> routePoints(const RaceRoute& route, RoutePointKind kind)
{
    std::vector<const RouteTransform*> points;
    for (const RouteTransform& transform : route.transforms) {
        if (routePointKind(transform.name) == kind) {
            points.push_back(&transform);
        }
    }
    std::stable_sort(points.begin(), points.end(), [](const RouteTransform* a, const RouteTransform* b) {
        return std::pair(trailingNumber(a->name), a->name) < std::pair(trailingNumber(b->name), b->name);
    });
    return points;
}

std::vector<Layer> raceOverlay(const Race& race, const RaceRoute& route)
{
    const std::vector<const RouteTransform*> grid = routePoints(route, RoutePointKind::StartSlot);
    const std::vector<const RouteTransform*> checkpoints = routePoints(route, RoutePointKind::Checkpoint);
    const std::vector<const RouteTransform*> waypoints = routePoints(route, RoutePointKind::Waypoint);
    const std::vector<const RouteTransform*> finish = routePoints(route, RoutePointKind::Finish);

    const QString routeGroup = QStringLiteral("Route");
    const QString finishGroup = QStringLiteral("Finish");
    Layer path;
    path.id = kRacePathLayer;
    path.title = QStringLiteral("Route of %1").arg(race.name);
    path.source = route.source;
    path.kind = FeatureKind::Polyline;
    path.groupColours = {{routeGroup, kRouteColour}, {finishGroup, kFinishColour}};

    Feature line;
    line.name = race.eventId;
    line.group = routeGroup;
    if (!route.racingLine.empty()) {
        line.shapes.push_back(route.racingLine);
        line.properties.append({QStringLiteral("Drawn from"), QStringLiteral("The AI racing line (aiopenworld.zip)")});
    } else {
        std::vector<QVector3D> points;
        if (!grid.empty()) {
            points.push_back(grid.front()->position);
        }
        for (const RouteTransform* point : checkpoints.empty() ? waypoints : checkpoints) {
            points.push_back(point->position);
        }
        if (!finish.empty()) {
            points.push_back(finish.front()->position);
        }
        if (points.size() >= 2) {
            line.shapes.push_back(std::move(points));
        }
        line.properties.append({QStringLiteral("Drawn from"),
            QStringLiteral("Straight lines between the checkpoints; the game ships no racing line for this route")});
    }
    if (!line.shapes.empty()) {
        line.position = line.shapes.front().front();
        path.features.push_back(std::move(line));
    }
    for (const RouteTransform* trigger : finish) {
        if (trigger->width <= 0.0F) {
            continue;
        }
        // Across the road: the heading turned a quarter clockwise, seen from
        // above with X east and Z north.
        const QVector3D across = QVector3D(trigger->facing.z(), 0.0F, -trigger->facing.x()).normalized();
        const QVector3D half = across * (trigger->width / 2.0F);
        Feature finishLine;
        finishLine.name = trigger->name;
        finishLine.group = finishGroup;
        finishLine.position = trigger->position;
        finishLine.shapes.push_back({trigger->position - half, trigger->position + half});
        finishLine.properties.append(
            {QStringLiteral("Width"), QStringLiteral("%1 m").arg(static_cast<double>(trigger->width))});
        path.features.push_back(std::move(finishLine));
    }

    const QString gridGroup = QStringLiteral("Start grid");
    const QString checkpointGroup = QStringLiteral("Checkpoints");
    const QString waypointGroup = QStringLiteral("Waypoints");
    Layer points;
    points.id = kRacePointsLayer;
    points.title = QStringLiteral("Start, checkpoints and finish of %1").arg(race.name);
    points.source = route.source;
    points.kind = FeatureKind::Point;
    points.groupColours = {{gridGroup, kStartColour}, {checkpointGroup, kCheckpointColour},
        {waypointGroup, kWaypointColour}, {finishGroup, kFinishColour}};
    for (std::size_t i = 0; i < grid.size(); ++i) {
        Feature slot = pointFeature(*grid[i], gridGroup, i == 0 ? QStringLiteral("Start") : QString());
        slot.properties.prepend({QStringLiteral("Grid position"), QString::number(i + 1)});
        points.features.push_back(std::move(slot));
    }
    for (std::size_t i = 0; i < checkpoints.size(); ++i) {
        points.features.push_back(
            pointFeature(*checkpoints[i], checkpointGroup, QStringLiteral("Checkpoint %1").arg(i + 1)));
    }
    for (const RouteTransform* waypoint : waypoints) {
        points.features.push_back(pointFeature(*waypoint, waypointGroup, {}));
    }
    for (const RouteTransform* trigger : finish) {
        points.features.push_back(pointFeature(*trigger, finishGroup, QStringLiteral("Finish")));
    }

    std::vector<Layer> layers;
    if (!path.features.empty()) {
        layers.push_back(std::move(path));
    }
    if (!points.features.empty()) {
        layers.push_back(std::move(points));
    }
    return layers;
}

} // namespace fh1
