#pragma once

#include "TrackPlacements.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace fh1 {

/// A procedural placement set of a track (`__R00G#####.pgeo` in bin.zip,
/// magic "OEGP", big-endian): many copies of a few models, such as the trees,
/// bushes, rocks and fences of an area. Offsets are from the file start:
///
///     0x40 u32 e, 0x48 u32 file size, 0x54 u32 m,
///     0x60 set name, NUL-terminated (28 bytes)
///     0x80 m mesh entries of 64 bytes: bounds (2 x 4 f32), u32 p, three
///          level-of-detail slots (u32 1 when used, u32), u32
///     one u32 draw record per used slot of each mesh with p = 0, in order
///     for each mesh with p > 0: p bytes of source path; then 4-byte aligned
///     e entries of 8 bytes, then 20 bytes whose fourth u32 is g
///     g groups of 92 bytes: +44 u32 mesh, +48 u32 n, +56 u32 b,
///          +60 f32 draw distance
///     16-byte aligned, for each group: n instances of 96 bytes (X, Y and Z
///          axes as 4 f32, scaled; position as 4 f32 with w = 1; tint;
///          ground normal), then b blocks of 8 f32 whose first two values
///          are the level-of-detail switch distances
///
/// A mesh's draw records are template placements at the world origin whose
/// models the set copies; meshes named by a source path instead have no
/// draw record and are left out. The layout reads every "Models_Ungrouped"
/// set of Colorado; it is inferred from the data, not from game code.
struct ScatterSet {
    struct Instance {
        /// Index into `meshDraws`.
        std::uint32_t mesh = 0;
        /// Where the instance goes, with the distances at which the game
        /// switches between the mesh's levels and stops drawing it.
        Placement placement;
    };

    QString name;
    /// Draw record of each level of detail of each mesh, finest first; empty
    /// for meshes named by a source path.
    std::vector<std::vector<std::uint32_t>> meshDraws;
    std::vector<Instance> instances;
};

/// Parses a procedural placement set. Returns nothing, with `error` set, if
/// the data does not have the layout ScatterSet documents.
std::optional<ScatterSet> readScatterSet(const QByteArray& data, QString* error = nullptr);

} // namespace fh1
