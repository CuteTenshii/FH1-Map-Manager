#pragma once

#include "MapData.h"

#include <QByteArray>
#include <QHash>
#include <QString>

#include <stdexcept>

namespace fh1 {

class LoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Parsers for the track data formats. Each takes the file contents and throws
/// LoadError with a description when the data is malformed.
namespace loaders {

/// Ribbon_NN/CollObjs.xml and GameObjs.xml: `<ObjN>` elements with a type
/// attribute, a `<Pos>` and an `<Orientation>` basis. `typeAttribute` names the
/// attribute that identifies the object ("PhysicsType" or "GameplayID").
Layer placements(const QByteArray& data, const QString& layerId, const QString& title, const QString& source,
    const QString& typeAttribute);

/// Ribbon_NN/TrackRouteNNN.xml: `<NamedTransform name>` elements, some with
/// a `width`, each holding a `<Transform>` with pos.x/y/z and facing.x/y/z
/// attributes. The result's routeId is left for the caller to set.
RaceRoute raceRoute(const QByteArray& data, const QString& source);

/// Appends one point feature per transform of `route`, grouped by
/// transformKind() of its name; `routeLabel` (the file's base name) is kept
/// as a property.
void appendTrackRoute(const RaceRoute& route, const QString& routeLabel, Layer& layer);

/// Ribbon_NN/ParticleEmitters.xml.
Layer particleEmitters(const QByteArray& data, const QString& source);

/// Ribbon_NN/PostProcessingZones_Safe.xml: named zones made of XZ triangles.
Layer postProcessingZones(const QByteArray& data, const QString& source);

/// <track>.nav: the road graph; this reads its node positions.
Layer navNodes(const QByteArray& data, const QString& source);

/// aiopenworld.zip route_NNN.owt: an AI racing line as a polyline.
Feature aiRoute(const QByteArray& data, const QString& name);

/// A transform name without its trailing number parts:
/// "start_location_03" -> "start_location", "anim_couple_01_01" -> "anim_couple".
QString transformKind(const QString& name);

/// Category shown for a GameplayID or PhysicsType, derived from its name.
QString gameplayGroup(const QString& gameplayId);
QString physicsGroup(const QString& physicsType);

} // namespace loaders
} // namespace fh1
