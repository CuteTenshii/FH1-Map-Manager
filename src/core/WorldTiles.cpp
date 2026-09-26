#include "WorldTiles.h"

#include "Loaders.h"
#include "RenderMesh.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>

namespace fh1 {

WorldTileGrid::WorldTileGrid(const WorldIndex& index, float tileSize, bool eventProps)
    : m_index(index)
{
    const QRectF footprint = index.footprint();
    const auto& chunks = index.chunks();
    std::map<std::pair<int, int>, std::size_t> tileByCell;
    for (std::uint32_t i = 0; i < chunks.size(); ++i) {
        const WorldChunk& chunk = chunks[i];
        if (chunk.placed && chunk.placement.eventProp && !eventProps) {
            continue;
        }
        const QVector3D centre = (chunk.boundsMin + chunk.boundsMax) / 2.0F;
        const int cx = static_cast<int>(std::floor((centre.x() - footprint.left()) / tileSize));
        const int cz = static_cast<int>(std::floor((centre.z() - footprint.top()) / tileSize));
        auto [it, inserted] = tileByCell.try_emplace({cx, cz}, m_tiles.size());
        if (inserted) {
            Tile tile;
            tile.boundsMin = chunk.boundsMin;
            tile.boundsMax = chunk.boundsMax;
            m_tiles.push_back(std::move(tile));
        }
        Tile& tile = m_tiles[it->second];
        tile.chunks.push_back(i);
        tile.boundsMin = QVector3D(std::min(tile.boundsMin.x(), chunk.boundsMin.x()),
            std::min(tile.boundsMin.y(), chunk.boundsMin.y()), std::min(tile.boundsMin.z(), chunk.boundsMin.z()));
        tile.boundsMax = QVector3D(std::max(tile.boundsMax.x(), chunk.boundsMax.x()),
            std::max(tile.boundsMax.y(), chunk.boundsMax.y()), std::max(tile.boundsMax.z(), chunk.boundsMax.z()));
    }
    for (Tile& tile : m_tiles) {
        for (std::uint32_t c : tile.chunks) {
            tile.edges.push_back(chunks[c].bandStart);
            tile.edges.push_back(chunks[c].bandEnd);
        }
        std::sort(tile.edges.begin(), tile.edges.end());
        tile.edges.erase(std::unique(tile.edges.begin(), tile.edges.end()), tile.edges.end());
    }
}

float WorldTileGrid::distanceTo(const Tile& tile, float x, float z) const
{
    const float dx = std::max({tile.boundsMin.x() - x, 0.0F, x - tile.boundsMax.x()});
    const float dz = std::max({tile.boundsMin.z() - z, 0.0F, z - tile.boundsMax.z()});
    return std::hypot(dx, dz);
}

int WorldTileGrid::stateAt(const Tile& tile, float distance) const
{
    return static_cast<int>(std::upper_bound(tile.edges.begin(), tile.edges.end(), distance) - tile.edges.begin());
}

std::vector<std::uint32_t> WorldTileGrid::chunksAt(const Tile& tile, float distance) const
{
    std::vector<std::uint32_t> result;
    const auto& chunks = m_index.chunks();
    for (std::uint32_t c : tile.chunks) {
        if (distance >= chunks[c].bandStart && distance < chunks[c].bandEnd) {
            result.push_back(c);
        }
    }
    return result;
}

namespace {

/// The diffuse texture of `material` and where its texture coordinates sit
/// in a vertex, if both are known.
struct DiffuseSource {
    std::uint32_t texture = TileMesh::kNoTexture;
    int texcoordOffset = -1;
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
bool isNotSurface(const RenderMesh& mesh, const RenderMesh::Material& material)
{
    if (material.name.startsWith(QLatin1String("Placeholder"), Qt::CaseInsensitive)
        || material.name.compare(QLatin1String("CrowdTERR"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    if (material.tableIndex >= mesh.materialTable.size()) {
        return false;
    }
    const std::uint32_t shader = mesh.materialTable[material.tableIndex].shader;
    return shader < static_cast<std::uint32_t>(mesh.shaders.size())
        && mesh.shaders[static_cast<qsizetype>(shader)].contains(QLatin1String("light_pollution"), Qt::CaseInsensitive);
}

DiffuseSource diffuseSource(const RenderMesh& mesh, const RenderMesh::Part& part, const RenderMesh::Material& material,
    const std::vector<std::uint32_t>* objectTextures, const TrackTextures& textures)
{
    if (objectTextures == nullptr || material.tableIndex >= mesh.materialTable.size()) {
        return {};
    }
    const RenderMesh::MaterialInfo& info = mesh.materialTable[material.tableIndex];
    // Slot 0 is sampler register 0, the diffuse (or first blend layer)
    // texture of every track shader.
    if (info.textureSlots.empty() || info.textureSlots.front() < 0
        || static_cast<std::size_t>(info.textureSlots.front()) >= objectTextures->size()
        || info.shader >= static_cast<std::uint32_t>(mesh.shaders.size())) {
        return {};
    }
    const ShaderLayout* layout = textures.shader(mesh.shaders[static_cast<qsizetype>(info.shader)]);
    if (layout == nullptr || layout->texcoord0Offset < 0
        || static_cast<std::uint32_t>(layout->texcoord0Offset) + 4 > part.stride) {
        return {};
    }
    return {(*objectTextures)[static_cast<std::size_t>(info.textureSlots.front())], layout->texcoord0Offset};
}

} // namespace

TileMesh buildTileMesh(const ForzaZip& archive, const WorldIndex& index, const std::vector<std::uint32_t>& chunks,
    const TrackTextures* textures)
{
    TileMesh out;
    // Indices per (backdrop, chunk, texture), concatenated into out.indices
    // at the end. Only backdrop geometry is kept apart by chunk.
    std::map<std::tuple<bool, std::uint32_t, std::uint32_t>, std::vector<std::uint32_t>> batchIndices;
    const auto& entries = archive.entries();
    const auto fail = [&out](std::uint32_t chunk, const QString& error) {
        ++out.failedChunks;
        out.errors.append(error);
        out.models.push_back({chunk, 0, {}, error});
    };
    // A prop placed many times in the tile is read and parsed once.
    std::unordered_map<std::uint32_t, std::shared_ptr<const RenderMesh>> meshes;
    std::unordered_map<std::uint32_t, QString> failures;
    for (std::uint32_t c : chunks) {
        const WorldChunk& chunk = index.chunks()[c];
        if (chunk.entry >= entries.size()) {
            fail(c, QStringLiteral("chunk %1 refers to a missing archive entry").arg(c));
            continue;
        }
        const ZipEntry& entry = entries[chunk.entry];
        if (const auto failed = failures.find(chunk.entry); failed != failures.end()) {
            fail(c, failed->second);
            continue;
        }
        std::shared_ptr<const RenderMesh>& cached = meshes[chunk.entry];
        if (!cached) {
            QString error;
            const QByteArray data = archive.read(entry, &error);
            try {
                if (data.isNull()) {
                    throw LoadError(error.toStdString());
                }
                cached = std::make_shared<const RenderMesh>(rendermesh::parse(data, entry.name));
            } catch (const LoadError& e) {
                meshes.erase(chunk.entry);
                failures.emplace(chunk.entry, QString::fromStdString(e.what()));
                fail(c, failures.at(chunk.entry));
                continue;
            }
        }
        const RenderMesh& mesh = *cached;
        // A placement's matrix can mirror the model; its triangles then
        // turn inside out, and the normals computed from them with it.
        float handedness = 1.0F;
        if (chunk.placed) {
            const auto& r = chunk.placement.rows;
            const float determinant = r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6])
                + r[2] * (r[3] * r[7] - r[4] * r[6]);
            // The parsed model already has Z negated, which mirrors once more.
            handedness = determinant > 0.0F ? -1.0F : 1.0F;
        }
        TileMesh::Model model;
        model.chunk = c;
        model.triangles = static_cast<std::uint32_t>(mesh.triangleCount());
        const std::vector<std::uint32_t>* objectTextures = nullptr;
        if (textures != nullptr) {
            if (const std::optional<std::uint32_t> object = TrackTextures::objectNumber(entry.name)) {
                objectTextures = textures->objectTextures(*object);
            }
        }
        for (const RenderMesh::Part& part : mesh.parts) {
            std::vector<QVector3D> placedPositions;
            if (chunk.placed) {
                placedPositions.reserve(part.positions.size());
                for (const QVector3D& p : part.positions) {
                    placedPositions.push_back(chunk.placement.apply(p));
                }
            }
            const std::vector<QVector3D>& positions = chunk.placed ? placedPositions : part.positions;
            std::vector<QVector3D> normals(positions.size());
            for (const RenderMesh::Material& material : part.materials) {
                if (isNotSurface(mesh, material)) {
                    continue;
                }
                for (std::size_t t = 0; t + 2 < material.triangles.size(); t += 3) {
                    const std::uint32_t a = material.triangles[t];
                    const std::uint32_t b = material.triangles[t + 1];
                    const std::uint32_t d = material.triangles[t + 2];
                    // Unnormalized, so larger triangles weigh more in the
                    // average. Flipping Z mirrored the geometry and reversed
                    // its winding, hence the swapped operands.
                    const QVector3D n = handedness
                        * QVector3D::crossProduct(positions[d] - positions[a], positions[b] - positions[a]);
                    normals[a] += n;
                    normals[b] += n;
                    normals[d] += n;
                }
            }
            // Materials sharing a vertex can map it through different
            // texture transforms, so every material gets its own copies.
            std::vector<std::uint32_t> remap(positions.size());
            for (const RenderMesh::Material& material : part.materials) {
                if (isNotSurface(mesh, material)) {
                    continue;
                }
                const DiffuseSource source = textures != nullptr
                    ? diffuseSource(mesh, part, material, objectTextures, *textures)
                    : DiffuseSource{};
                std::fill(remap.begin(), remap.end(), TileMesh::kNoTexture);
                if (source.texture != TileMesh::kNoTexture
                    && std::find(model.textures.begin(), model.textures.end(), source.texture)
                        == model.textures.end()) {
                    model.textures.push_back(source.texture);
                }
                std::vector<std::uint32_t>& target
                    = batchIndices[{chunk.backdrop, chunk.backdrop ? c : TileMesh::kMergedChunks, source.texture}];
                for (std::uint32_t v : material.triangles) {
                    if (remap[v] == TileMesh::kNoTexture) {
                        remap[v] = static_cast<std::uint32_t>(out.vertexCount());
                        const QVector3D& p = positions[v];
                        QVector3D n = normals[v].normalized();
                        if (n.isNull()) {
                            n = QVector3D(0.0F, 1.0F, 0.0F);
                        }
                        const QVector2D uv = source.texcoordOffset >= 0
                            ? RenderMesh::texcoord(part, material, v, source.texcoordOffset)
                            : QVector2D();
                        out.vertices.insert(
                            out.vertices.end(), {p.x(), p.y(), p.z(), n.x(), n.y(), n.z(), uv.x(), uv.y()});
                    }
                    target.push_back(remap[v]);
                }
            }
        }
        out.models.push_back(std::move(model));
    }
    for (auto& [key, indices] : batchIndices) {
        if (indices.empty()) {
            continue;
        }
        const auto& [backdrop, chunk, texture] = key;
        out.batches.push_back({texture, static_cast<std::uint32_t>(out.indices.size()),
            static_cast<std::uint32_t>(indices.size()), backdrop, chunk});
        out.indices.insert(out.indices.end(), indices.begin(), indices.end());
    }
    return out;
}

} // namespace fh1
