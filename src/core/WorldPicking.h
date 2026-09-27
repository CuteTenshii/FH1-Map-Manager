#pragma once

#include "ForzaZip.h"
#include "WorldIndex.h"

#include <QVector3D>

#include <cstdint>
#include <optional>
#include <vector>

namespace fh1 {

/// Where a ray first meets a model of the world.
struct PickHit {
    /// Index into WorldIndex::chunks().
    std::uint32_t chunk = 0;
    /// Distance along the ray, in metres.
    float distance = 0.0F;
    QVector3D point;
    /// Triangles of the model that was hit.
    std::size_t triangles = 0;
};

/// Distance along the ray from `origin` in unit direction `direction` at
/// which it enters the box `lo`..`hi`, or nothing if it misses it or the box
/// lies behind the origin. An origin inside the box gives 0.
std::optional<float> rayBoxDistance(
    const QVector3D& origin, const QVector3D& direction, const QVector3D& lo, const QVector3D& hi);

/// Distance along the ray at which it crosses triangle `a`, `b`, `c` from
/// either side, or nothing.
std::optional<float> rayTriangleDistance(
    const QVector3D& origin, const QVector3D& direction, const QVector3D& a, const QVector3D& b, const QVector3D& c);

/// The model among `candidates` (indices into `index`'s chunks) whose
/// triangles the ray from `origin` along unit `direction` meets first,
/// within `maxDistance`. Models are placed and their materials skipped as
/// for drawing (see buildTileMesh); backdrop terrain, drawn behind
/// everything else, is never picked. Reads the candidates' model files from
/// `archive`, nearest bounds first, and stops once no remaining model can
/// be nearer than the best hit.
std::optional<PickHit> pickModel(const ForzaZip& archive, const WorldIndex& index,
    const std::vector<std::uint32_t>& candidates, const QVector3D& origin, const QVector3D& direction,
    float maxDistance);

} // namespace fh1
