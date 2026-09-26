#pragma once

#include "GameInstall.h"
#include "MapData.h"

#include <functional>

namespace fh1 {

/// Assembles every map layer for one track from an extracted disc.
class MapLoader {
public:
    using Progress = std::function<void(const QString& step)>;

    /// Loads `trackFolder` (a folder name under media/tracks, such as
    /// "colorado"). Missing or malformed optional files become warnings in
    /// the result; a missing track folder throws LoadError.
    static MapData load(const GameInstall& install, const QString& trackFolder, const Progress& progress = {});
};

} // namespace fh1
