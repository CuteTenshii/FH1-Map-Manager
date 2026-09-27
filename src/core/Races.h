#pragma once

#include "MapData.h"

#include <QString>

#include <vector>

namespace fh1 {

/// What a transform of a race route file marks, from its name.
enum class RoutePointKind {
    /// start_location_NN: a car's place on the start grid, pole first.
    StartSlot,
    /// start_gantry_cannon_NN
    StartGantry,
    /// route_waypoint_NN: points the route passes, in order.
    Waypoint,
    /// route_checkpoint_NN: the checkpoints of street races, in order.
    Checkpoint,
    /// route_checkpoint_indicator_NN and _NNb, beside the checkpoints.
    CheckpointMarker,
    /// route_boundary_NN
    Boundary,
    /// end_race_cannon_trigger: the finish; its width spans the road.
    Finish,
    /// end_race_cannon_left_NN and _right_NN
    FinishCannon,
    /// post_race_location_NN: where the car is put after the race.
    PostRace,
    /// Photo and stunt missions, crowd animations and anything else.
    Other,
};

RoutePointKind routePointKind(const QString& transformName);

/// The number a transform name ends with ("route_checkpoint_12" -> 12,
/// "route_checkpoint_indicator_12b" -> 12), or -1.
int routePointNumber(const QString& transformName);

/// The transforms of `route` of one kind, ordered by the number their name
/// ends with.
std::vector<const RouteTransform*> routePoints(const RaceRoute& route, RoutePointKind kind);

/// Layer ids of raceOverlay().
inline constexpr QLatin1StringView kRacePathLayer("racepath");
inline constexpr QLatin1StringView kRacePointsLayer("racepoints");

/// What the viewer draws for a selected race: a polyline layer with the
/// route and the finish line, and a point layer with the start grid,
/// checkpoints, waypoints and finish, each in a colour of its own.
///
/// The route is the AI racing line when the game ships one; otherwise it
/// runs straight from the pole position through the checkpoints (or, for
/// races without any, the waypoints) to the finish, which only
/// approximates the roads between them.
std::vector<Layer> raceOverlay(const Race& race, const RaceRoute& route);

} // namespace fh1
