#include "WorldPicking.h"

#include "Loaders.h"
#include "RenderMesh.h"
#include "WorldTiles.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace fh1 {

namespace {

constexpr float kParallelEpsilon = 1e-9F;

} // namespace

std::optional<float> rayBoxDistance(
    const QVector3D& origin, const QVector3D& direction, const QVector3D& lo, const QVector3D& hi)
{
    float enter = 0.0F;
    float leave = std::numeric_limits<float>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        const float o = origin[axis];
        const float d = direction[axis];
        if (std::abs(d) < kParallelEpsilon) {
            if (o < lo[axis] || o > hi[axis]) {
                return std::nullopt;
            }
            continue;
        }
        float t1 = (lo[axis] - o) / d;
        float t2 = (hi[axis] - o) / d;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        enter = std::max(enter, t1);
        leave = std::min(leave, t2);
        if (enter > leave) {
            return std::nullopt;
        }
    }
    return enter;
}

std::optional<float> rayTriangleDistance(
    const QVector3D& origin, const QVector3D& direction, const QVector3D& a, const QVector3D& b, const QVector3D& c)
{
    // Möller-Trumbore, accepting either side of the triangle.
    const QVector3D edge1 = b - a;
    const QVector3D edge2 = c - a;
    const QVector3D p = QVector3D::crossProduct(direction, edge2);
    const float determinant = QVector3D::dotProduct(edge1, p);
    if (std::abs(determinant) < kParallelEpsilon) {
        return std::nullopt;
    }
    const float inverse = 1.0F / determinant;
    const QVector3D s = origin - a;
    const float u = QVector3D::dotProduct(s, p) * inverse;
    if (u < 0.0F || u > 1.0F) {
        return std::nullopt;
    }
    const QVector3D q = QVector3D::crossProduct(s, edge1);
    const float v = QVector3D::dotProduct(direction, q) * inverse;
    if (v < 0.0F || u + v > 1.0F) {
        return std::nullopt;
    }
    const float t = QVector3D::dotProduct(edge2, q) * inverse;
    if (t < 0.0F) {
        return std::nullopt;
    }
    return t;
}

std::optional<PickHit> pickModel(const ForzaZip& archive, const WorldIndex& index,
    const std::vector<std::uint32_t>& candidates, const QVector3D& origin, const QVector3D& direction,
    float maxDistance)
{
    const auto& chunks = index.chunks();
    const auto& entries = archive.entries();
    std::vector<std::pair<float, std::uint32_t>> boxes;
    for (const std::uint32_t c : candidates) {
        if (c >= chunks.size() || chunks[c].backdrop) {
            continue;
        }
        const std::optional<float> enter = rayBoxDistance(origin, direction, chunks[c].boundsMin, chunks[c].boundsMax);
        if (enter && *enter <= maxDistance) {
            boxes.emplace_back(*enter, c);
        }
    }
    std::sort(boxes.begin(), boxes.end());
    boxes.erase(std::unique(boxes.begin(), boxes.end()), boxes.end());

    std::optional<PickHit> best;
    for (const auto& [enter, c] : boxes) {
        if (best && enter > best->distance) {
            break;
        }
        const WorldChunk& chunk = chunks[c];
        if (chunk.entry >= entries.size()) {
            continue;
        }
        RenderMesh mesh;
        try {
            const QByteArray data = archive.read(entries[chunk.entry]);
            if (data.isNull()) {
                continue;
            }
            mesh = rendermesh::parse(data, entries[chunk.entry].name);
        } catch (const LoadError&) {
            // A model that cannot be read is not drawn either.
            continue;
        }
        float nearest = best ? best->distance : maxDistance;
        bool hit = false;
        for (const RenderMesh::Part& part : mesh.parts) {
            std::vector<QVector3D> placed;
            if (chunk.placed) {
                placed.reserve(part.positions.size());
                for (const QVector3D& p : part.positions) {
                    placed.push_back(chunk.placement.apply(p));
                }
            }
            const std::vector<QVector3D>& positions = chunk.placed ? placed : part.positions;
            for (const RenderMesh::Material& material : part.materials) {
                if (isNotSurface(mesh, material)) {
                    continue;
                }
                for (std::size_t t = 0; t + 2 < material.triangles.size(); t += 3) {
                    const std::optional<float> distance
                        = rayTriangleDistance(origin, direction, positions[material.triangles[t]],
                            positions[material.triangles[t + 1]], positions[material.triangles[t + 2]]);
                    if (distance && *distance < nearest) {
                        nearest = *distance;
                        hit = true;
                    }
                }
            }
        }
        if (hit) {
            best = PickHit{c, nearest, origin + direction * nearest, mesh.triangleCount()};
        }
    }
    return best;
}

} // namespace fh1
