#pragma once

#include "GameObjects.h"
#include "MapCalibration.h"
#include "ScriptReferences.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector3D>

#include <vector>

namespace fh1 {

using Properties = QList<QPair<QString, QString>>;

enum class FeatureKind { Point, Polyline, Polygon };

/// One thing drawn on the map, in game world coordinates (metres, Y up).
struct Feature {
    /// Identifier from the source data, e.g. a GameplayID or file name.
    QString name;
    /// Human-readable name resolved from the game's text (an event, route or
    /// car name), or empty when none is known. Shown on the map and in lists.
    QString label;
    /// Key into MapData::icons for features the in-game map draws with an
    /// icon; empty for plain markers.
    QString icon;
    /// Sub-category within its layer, used for per-group visibility and colour.
    QString group;
    /// Anchor position: the point itself, or a representative vertex.
    QVector3D position;
    /// Heading for point features; null when the source has none.
    QVector3D forward;
    /// Polyline vertices (one shape) or polygon rings (one or more shapes).
    std::vector<std::vector<QVector3D>> shapes;
    Properties properties;
};

struct Layer {
    QString id;
    QString title;
    /// Where the data came from, shown to the user, e.g. "Ribbon_00/CollObjs.xml".
    QString source;
    FeatureKind kind = FeatureKind::Point;
    std::vector<Feature> features;
    /// Colours for groups that should not take the viewer's palette colour.
    QHash<QString, QColor> groupColours;
};

/// One named transform of a race route file.
struct RouteTransform {
    QString name;
    QVector3D position;
    /// Unit heading.
    QVector3D facing;
    /// Metres across, for the transforms that carry a `width` attribute
    /// (checkpoints and the finish trigger); 0 for the others.
    float width = 0.0F;
    /// The `<NamedTransform>` attributes as written in the file, in order;
    /// saving writes `name` and `width` from the fields above.
    Properties attributes;
    /// Where the transform's text lies in RouteFile::text, from the end of
    /// the one before it to the end of its closing line; -1 for a
    /// transform added since.
    qsizetype sourceStart = -1;
    qsizetype sourceEnd = -1;
    /// Changed since it was read, so saving writes it anew.
    bool edited = false;
};

/// A route file as read, so that saving can keep what was not edited
/// byte for byte.
struct RouteFile {
    /// The file's contents; empty for a route built from scratch, or when
    /// the file is not plain ASCII.
    QByteArray text;
    /// Where the first transform's text starts and the last one's ends.
    qsizetype transformsStart = -1;
    qsizetype transformsEnd = -1;
    /// How many transforms the file holds.
    std::size_t transformCount = 0;
    /// The layout of the file's transforms, for writing new ones.
    QByteArray indent = QByteArrayLiteral("\t\t");
    QByteArray innerIndent = QByteArrayLiteral("\t\t\t");
    QByteArray newline = QByteArrayLiteral("\r\n");
};

/// A race route: tracks/<track>/Ribbon_00/TrackRouteNNN.xml, whose NNN is
/// the route's Tracks.RouteId in gamedb. It holds the start grid,
/// checkpoints, waypoints and finish of the races run on the route.
struct RaceRoute {
    int routeId = -1;
    /// Where the transforms came from, e.g. "Ribbon_00/TrackRoute096.xml".
    QString source;
    /// The file's path under the media folder, as the disc spells it, e.g.
    /// "tracks/colorado/Ribbon_00/TrackRoute096.xml".
    QString mediaPath;
    RouteFile file;
    /// In file order.
    std::vector<RouteTransform> transforms;
    /// The AI racing line of the route from aiopenworld.zip, or empty when
    /// the game ships none for it.
    std::vector<QVector3D> racingLine;
};

/// A race event of the track: a gamedb Events row with its Races row.
struct Race {
    /// Events.HorizonEventID, e.g. "FR05"; the game's objects and props of
    /// the event are named after it.
    QString eventId;
    /// From the game's text; the event ID when the text is missing.
    QString name;
    /// CareerEventTypes name, e.g. "Festival Circuit Race".
    QString type;
    /// Name of the car class the event is for, e.g. "B".
    QString carClass;
    /// Display name of the route, e.g. "Beaumont Circuit".
    QString routeName;
    int laps = 1;
    /// Length of one lap or of the whole sprint, in metres (Tracks.Length).
    int length = 0;
    /// Credits for winning.
    int prize = 0;
    int routeId = -1;
    /// Index into MapData::raceRoutes, or -1 when the route file is missing.
    int route = -1;
    /// Its Events and Races rows in gamedb, which edits are written to.
    int eventRow = -1;
    int raceRow = -1;
    /// AI cars racing the player.
    int opponents = 0;
    /// When the race starts, in seconds after midnight.
    int timeOfDay = 0;
    /// The car class the race is for: a CarClasses.Id (see MapData::carClasses).
    int carClassId = -1;
    /// Events.Level: the career level it belongs to; -1 for the opening
    /// race (HORIZON Heats) and the free-roam session.
    int level = 0;
};

/// A car class a race can be for.
struct CarClass {
    int id = -1;
    /// "D", "S", "R1"…
    QString name;
};

/// Everything the viewer shows for one track.
struct MapData {
    QString trackName;
    QImage background;
    QString backgroundSource;
    /// Maps world X/Z to scene (image) pixels. Falls back to
    /// worldCalibration() when the track has no known background image.
    MapCalibration calibration = worldCalibration();
    std::vector<Layer> layers;
    /// The track's race events, in gamedb order.
    std::vector<Race> races;
    /// The routes the races run on.
    std::vector<RaceRoute> raceRoutes;
    /// Every car class, by id.
    std::vector<CarClass> carClasses;
    /// Ribbon_00/GameObjs.xml, whose objects are the "gameobjs" layer's
    /// features in the same order; empty when the track has none or they
    /// do not match up.
    GameObjectsFile gameObjects;
    /// Names the game's scripts in gamemodes.zip use, for this track and
    /// the whole game. The list of every event's activation
    /// (career_event_activations.xml) is left out, as each event has one.
    ScriptReferences scripts;
    /// The in-game map's icons, keyed by its `activity_type` names.
    QHash<QString, QImage> icons;
    /// Non-fatal problems met while loading, e.g. a missing optional file.
    QStringList warnings;
};

} // namespace fh1
