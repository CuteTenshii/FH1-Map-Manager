#pragma once

#include <QPointF>
#include <QSize>
#include <QString>

#include <cmath>
#include <optional>

namespace fh1 {

/// Axis-aligned mapping between game world X/Z (metres) and pixels of a map
/// image: imageX = scaleX * worldX + offsetX, imageY = scaleZ * worldZ + offsetY.
struct MapCalibration {
    double scaleX = 1.0;
    double offsetX = 0.0;
    double scaleZ = 1.0;
    double offsetY = 0.0;

    QPointF worldToImage(double worldX, double worldZ) const
    {
        return {scaleX * worldX + offsetX, scaleZ * worldZ + offsetY};
    }

    QPointF imageToWorld(QPointF image) const
    {
        return {(image.x() - offsetX) / scaleX, (image.y() - offsetY) / scaleZ};
    }

    /// Metres per image pixel along X, for scale bars and hit tolerances.
    double metresPerPixel() const { return 1.0 / std::abs(scaleX); }
};

/// A satellite image the game ships for a track, with its calibration.
struct BackgroundSource {
    QString archive;
    QString entry;
    QSize expectedSize;
    MapCalibration calibration;
};

/// Returns the known background image for a track folder name (for example
/// "colorado"), or nothing if the game ships none.
std::optional<BackgroundSource> knownBackground(const QString& trackFolder);

/// The calibration used when a track has no background image: 1 px = 1 m,
/// north (negative Z) up, matching the orientation of the Colorado map.
MapCalibration worldCalibration();

} // namespace fh1
