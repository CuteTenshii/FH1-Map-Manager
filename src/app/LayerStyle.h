#pragma once

#include "MapData.h"

#include <QColor>
#include <QString>
#include <QStringList>

#include <vector>

/// How a map layer's features are grouped and coloured. The 2D map and the
/// 3D view both use it, so a group has the same index and colour in both.
struct LayerStyle {
    /// Group names ordered by feature count, largest first.
    QStringList groups;
    /// Index into `groups` for each feature of the layer.
    std::vector<int> featureGroup;
    std::vector<int> groupSizes;
    /// The layer's own colour for a group (fh1::Layer::groupColours), or
    /// one from a palette.
    std::vector<QColor> groupColors;

    static LayerStyle of(const fh1::Layer& layer, int layerIndex);
};

/// Radius, in device pixels, of the marker drawn for a point of layer
/// `layerId` that has no icon.
double markerRadiusFor(const QString& layerId);
