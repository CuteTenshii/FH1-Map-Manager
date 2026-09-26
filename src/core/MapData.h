#pragma once

#include "MapCalibration.h"

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
    /// The in-game map's icons, keyed by its `activity_type` names.
    QHash<QString, QImage> icons;
    /// Non-fatal problems met while loading, e.g. a missing optional file.
    QStringList warnings;
};

} // namespace fh1
