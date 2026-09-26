#include "MapCalibration.h"

namespace fh1 {

std::optional<BackgroundSource> knownBackground(const QString& trackFolder)
{
    if (trackFolder.compare(QLatin1String("colorado"), Qt::CaseInsensitive) == 0) {
        // The game's world-map texture. The calibration was fitted by
        // projecting the 12,036 road nodes of colorado.nav onto the image and
        // maximising their overlap with high-pass-filtered road pixels; X keeps
        // its sign and Z is flipped, so north is up.
        BackgroundSource source;
        source.archive = QStringLiteral("UI.zip");
        source.entry = QStringLiteral("New Map/MapGameReady.jpg");
        source.expectedSize = QSize(5120, 3072);
        source.calibration = MapCalibration{0.2675, 2559.0, -0.2673, 1227.5};
        return source;
    }
    return std::nullopt;
}

MapCalibration worldCalibration()
{
    return MapCalibration{1.0, 0.0, -1.0, 0.0};
}

} // namespace fh1
