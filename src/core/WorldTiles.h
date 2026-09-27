#pragma once

#include "ForzaZip.h"
#include "RenderMesh.h"
#include "TrackTextures.h"
#include "WorldIndex.h"

#include <QRectF>

#include <cstdint>
#include <vector>

namespace fh1 {

/// The world's chunks bucketed into square tiles, so the 3D view can merge a
/// tile's chunks into one draw and re-merge only when the camera crosses one
/// of the tile's LOD band edges.
class WorldTileGrid {
public:
    struct Tile {
        /// Union of the tile's chunk bounds.
        QVector3D boundsMin;
        QVector3D boundsMax;
        /// Indices into WorldIndex::chunks().
        std::vector<std::uint32_t> chunks;
        /// Sorted, distinct band edges of the tile's chunks.
        std::vector<float> edges;
    };

    /// Leaves out event props (Placement::eventProp) unless `eventProps`, as
    /// the game does outside events.
    WorldTileGrid(const WorldIndex& index, float tileSize, bool eventProps = false);

    const std::vector<Tile>& tiles() const { return m_tiles; }

    /// Horizontal distance from `x`, `z` to the tile's bounds (0 inside).
    float distanceTo(const Tile& tile, float x, float z) const;
    /// Which of the tile's distance intervals `distance` falls in; the chunk
    /// set changes only when this does.
    int stateAt(const Tile& tile, float distance) const;
    /// Chunks drawn when the tile is `distance` metres away.
    std::vector<std::uint32_t> chunksAt(const Tile& tile, float distance) const;
    const WorldIndex& index() const { return m_index; }

private:
    const WorldIndex& m_index;
    std::vector<Tile> m_tiles;
};

/// Geometry of one tile state, ready for upload: interleaved position
/// (x, y, z), normal (x, y, z) and texture coordinate (u, v) floats, and
/// triangle-list indices grouped into one batch per diffuse texture, with
/// backdrop terrain in batches of its own, one per chunk, so each can be
/// shown or hidden by the camera's zone (WorldChunk::zones).
struct TileMesh {
    static constexpr int kFloatsPerVertex = 8;
    /// Batch texture of geometry without a known diffuse texture.
    static constexpr std::uint32_t kNoTexture = 0xFFFFFFFF;
    /// Batch chunk of geometry merged from several chunks.
    static constexpr std::uint32_t kMergedChunks = 0xFFFFFFFF;

    struct Batch {
        std::uint32_t texture = kNoTexture;
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        /// Geometry of backdrop chunks (WorldChunk::backdrop).
        bool backdrop = false;
        /// For backdrop batches, the chunk (index into WorldIndex::chunks())
        /// the geometry comes from; kMergedChunks otherwise.
        std::uint32_t chunk = kMergedChunks;
    };

    /// One model file that went into the mesh, or failed to.
    struct Model {
        /// Index into WorldIndex::chunks().
        std::uint32_t chunk = 0;
        std::uint32_t triangles = 0;
        /// Distinct diffuse textures of its materials, in first-use order.
        std::vector<std::uint32_t> textures;
        /// Empty unless the model could not be read.
        QString error;
    };

    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Batch> batches;
    std::vector<Model> models;
    /// Chunks that failed to decode; their errors are in `errors`.
    int failedChunks = 0;
    QStringList errors;

    std::size_t vertexCount() const { return vertices.size() / kFloatsPerVertex; }
};

/// Materials that are not visible surfaces:
/// - a lighting effect, such as the glow of the town's lights over the night
///   sky ("light_pollution.fx" on a 1 km plane), which drawn as a surface
///   becomes a large opaque wall;
/// - the placeholder material ("Placeholder001") of an abandoned set of
///   terrain pieces ("Area04", "TERR_Zone1_Area1_00"), textured "THIS OBJECT
///   DOES NOT HAVE A FORZA MATERIAL" and partly below the ground. On
///   Colorado 63 models use it and only it;
/// - crowd areas ("CrowdTERR" on "Plane004_LOD00" and the like): flat
///   polygons just above the ground, textured with an orange tile grid,
///   that mark where spectators stand. On Colorado 48 models use it and
///   only it.
/// Such materials are neither drawn nor picked.
bool isNotSurface(const RenderMesh& mesh, const RenderMesh::Material& material);

/// Decodes and merges `chunks`. Safe to call from several threads at once on
/// the same archive. Normals are averaged per part from its triangles.
/// Without `textures`, or for materials whose diffuse texture or texture
/// coordinates are unknown, geometry goes into the kNoTexture batch.
TileMesh buildTileMesh(const ForzaZip& archive, const WorldIndex& index, const std::vector<std::uint32_t>& chunks,
    const TrackTextures* textures = nullptr);

} // namespace fh1
