#include "WorldTiles.h"

#include "Loaders.h"
#include "RenderMesh.h"

#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>

namespace fh1 {

EventPropFilter EventPropFilter::all()
{
    EventPropFilter filter;
    filter.m_all = true;
    return filter;
}

EventPropFilter EventPropFilter::race(const QString& eventId, int routeId)
{
    EventPropFilter filter;
    filter.m_eventId = eventId;
    filter.m_routeId = routeId;
    return filter;
}

bool EventPropFilter::includes(const WorldChunk& chunk) const
{
    if (!chunk.placed || !chunk.placement.eventProp || m_all) {
        return true;
    }
    return chunk.placement.belongsToRace(m_eventId, m_routeId);
}

WorldTileGrid::WorldTileGrid(const WorldIndex& index, float tileSize, const EventPropFilter& eventProps)
    : m_index(index)
{
    const QRectF footprint = index.footprint();
    const auto& chunks = index.chunks();
    std::map<std::pair<int, int>, std::size_t> tileByCell;
    for (std::uint32_t i = 0; i < chunks.size(); ++i) {
        const WorldChunk& chunk = chunks[i];
        if (!eventProps.includes(chunk)) {
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

/// What a material draws with: its surface (the batch it goes into, less
/// the index range) and where its vertices keep the inputs that surface
/// reads.
struct SurfaceSource {
    TileMesh::Batch surface;
    int texcoord0Offset = -1;
    int texcoord1Offset = -1;
    int colourOffset = -1;
};

/// Orders batches that draw alike, so identical surfaces share one.
using BatchKey = std::tuple<bool, std::uint32_t, TileMesh::Shading, std::uint32_t,
    std::array<std::uint32_t, TileMesh::LayerCount>, std::array<float, 6>>;

BatchKey batchKey(const TileMesh::Batch& b)
{
    return {b.backdrop, b.chunk, b.shading, b.texture, b.layers, b.layerScales};
}

/// A layer's texture scale from a material constant, where it gives one.
float layerScale(float value)
{
    return value > 0.0F ? value : 1.0F;
}

SurfaceSource surfaceSource(const RenderMesh& mesh, const RenderMesh::Part& part, const RenderMesh::Material& material,
    const std::vector<std::uint32_t>* objectTextures, const TrackTextures& textures)
{
    if (objectTextures == nullptr || material.tableIndex >= mesh.materialTable.size()) {
        return {};
    }
    const RenderMesh::MaterialInfo& info = mesh.materialTable[material.tableIndex];
    if (info.shader >= static_cast<std::uint32_t>(mesh.shaders.size())) {
        return {};
    }
    const ShaderLayout* layout = textures.shader(mesh.shaders[static_cast<qsizetype>(info.shader)]);
    const auto fits
        = [&part](int offset) { return offset >= 0 && static_cast<std::uint32_t>(offset) + 4 <= part.stride; };
    if (layout == nullptr || !fits(layout->texcoord0Offset)) {
        return {};
    }
    // Material slot k feeds sampler register k.
    const auto textureOf = [&](const QString& sampler) -> std::uint32_t {
        const int reg = layout->samplerRegister(sampler);
        if (reg < 0 || static_cast<std::size_t>(reg) >= info.textureSlots.size()) {
            return TileMesh::kNoTexture;
        }
        const int slot = info.textureSlots[static_cast<std::size_t>(reg)];
        return slot >= 0 && static_cast<std::size_t>(slot) < objectTextures->size()
            ? (*objectTextures)[static_cast<std::size_t>(slot)]
            : TileMesh::kNoTexture;
    };
    SurfaceSource source;
    source.texcoord0Offset = layout->texcoord0Offset;
    TileMesh::Batch& surface = source.surface;

    const std::uint32_t normalMap = textureOf(QStringLiteral("NormalMapASampler"));
    const std::uint32_t blendA = textureOf(QStringLiteral("Blend_ASampler"));
    const std::uint32_t blendB = textureOf(QStringLiteral("Blend_BSampler"));
    const std::uint32_t splat = textureOf(QStringLiteral("Splat_Sampler"));
    if (normalMap != TileMesh::kNoTexture && layout->samplerRegister(QStringLiteral("CubeMapSampler")) >= 0) {
        surface.shading = TileMesh::Shading::Water;
        surface.texture = normalMap;
        return source;
    }
    // The splat and occlusion maps span a ground patch once, on the last
    // coordinate pair the shader reads: the second for splat-blended
    // ground, the third for vertex-blended ground.
    const int patchOffset = fits(layout->texcoord2Offset) ? layout->texcoord2Offset
        : fits(layout->texcoord1Offset)                   ? layout->texcoord1Offset
                                                          : -1;
    const bool splatGround = blendA != TileMesh::kNoTexture && blendB != TileMesh::kNoTexture
        && splat != TileMesh::kNoTexture && patchOffset >= 0;
    // Roads also name two Blend layers, but mix them with noise and modulate
    // maps rather than a vertex colour; they stay plain.
    const bool roadLayers = layout->samplerRegister(QStringLiteral("Noise_Sampler")) >= 0
        || layout->samplerRegister(QStringLiteral("Modulate_Sampler")) >= 0;
    const bool vertexGround = blendA != TileMesh::kNoTexture && blendB != TileMesh::kNoTexture
        && splat == TileMesh::kNoTexture && !roadLayers && fits(layout->colourOffset);
    if (splatGround || vertexGround) {
        surface.shading = splatGround ? TileMesh::Shading::Splat : TileMesh::Shading::VertexBlend;
        surface.texture = blendA;
        surface.layers[TileMesh::LayerB] = blendB;
        surface.layers[TileMesh::LayerC]
            = splatGround ? textureOf(QStringLiteral("Blend_CSampler")) : TileMesh::kNoTexture;
        surface.layers[TileMesh::SplatMap] = splatGround ? splat : TileMesh::kNoTexture;
        if (patchOffset >= 0) {
            source.texcoord1Offset = patchOffset;
            surface.layers[TileMesh::OcclusionMap] = textureOf(QStringLiteral("AO_Sampler"));
        }
        if (vertexGround) {
            source.colourOffset = layout->colourOffset;
        }
        // The first constant holds Blend_A's and Blend_B's scales, the
        // second Blend_C's.
        if (!info.constants.empty()) {
            const QVector4D& ab = info.constants[0];
            surface.layerScales[0] = layerScale(ab.x());
            surface.layerScales[1] = layerScale(ab.y());
            surface.layerScales[2] = layerScale(ab.z());
            surface.layerScales[3] = layerScale(ab.w());
        }
        if (info.constants.size() > 1) {
            surface.layerScales[4] = layerScale(info.constants[1].x());
            surface.layerScales[5] = layerScale(info.constants[1].y());
        }
        return source;
    }
    // Register 0 holds the diffuse texture of every other track shader.
    if (!info.textureSlots.empty() && info.textureSlots.front() >= 0
        && static_cast<std::size_t>(info.textureSlots.front()) < objectTextures->size()) {
        surface.texture = (*objectTextures)[static_cast<std::size_t>(info.textureSlots.front())];
    }
    return source;
}

} // namespace

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

TileMesh buildTileMesh(const ForzaZip& archive, const WorldIndex& index, const std::vector<std::uint32_t>& chunks,
    const TrackTextures* textures)
{
    TileMesh out;
    // Indices per surface, concatenated into out.indices at the end. Only
    // backdrop geometry is kept apart by chunk.
    std::map<BatchKey, std::pair<TileMesh::Batch, std::vector<std::uint32_t>>> batchIndices;
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
                SurfaceSource source = textures != nullptr
                    ? surfaceSource(mesh, part, material, objectTextures, *textures)
                    : SurfaceSource{};
                source.surface.backdrop = chunk.backdrop;
                source.surface.chunk = chunk.backdrop ? c : TileMesh::kMergedChunks;
                std::fill(remap.begin(), remap.end(), TileMesh::kNoTexture);
                for (const std::uint32_t id : source.surface.textures()) {
                    if (std::find(model.textures.begin(), model.textures.end(), id) == model.textures.end()) {
                        model.textures.push_back(id);
                    }
                }
                auto& [batch, target] = batchIndices[batchKey(source.surface)];
                batch = source.surface;
                for (std::uint32_t v : material.triangles) {
                    if (remap[v] == TileMesh::kNoTexture) {
                        remap[v] = static_cast<std::uint32_t>(out.vertexCount());
                        const QVector3D& p = positions[v];
                        QVector3D n = normals[v].normalized();
                        if (n.isNull()) {
                            n = QVector3D(0.0F, 1.0F, 0.0F);
                        }
                        // The material's offset and scale decode both
                        // coordinate pairs.
                        const QVector2D uv = source.texcoord0Offset >= 0
                            ? RenderMesh::texcoord(part, material, v, source.texcoord0Offset)
                            : QVector2D();
                        const QVector2D uv1 = source.texcoord1Offset >= 0
                            ? RenderMesh::texcoord(part, material, v, source.texcoord1Offset)
                            : QVector2D();
                        // The colour's bytes travel in a float's space, in the
                        // file's order; the renderer reads them back as four
                        // bytes.
                        const std::uint32_t colour = source.colourOffset >= 0
                            ? qToBigEndian(RenderMesh::colour(part, v, source.colourOffset))
                            : 0xFFFFFFFFU;
                        float packed = 0.0F;
                        std::memcpy(&packed, &colour, sizeof packed);
                        out.vertices.insert(out.vertices.end(),
                            {p.x(), p.y(), p.z(), n.x(), n.y(), n.z(), uv.x(), uv.y(), uv1.x(), uv1.y(), packed});
                    }
                    target.push_back(remap[v]);
                }
            }
        }
        out.models.push_back(std::move(model));
    }
    for (auto& [key, entry] : batchIndices) {
        auto& [batch, indices] = entry;
        if (indices.empty()) {
            continue;
        }
        batch.firstIndex = static_cast<std::uint32_t>(out.indices.size());
        batch.indexCount = static_cast<std::uint32_t>(indices.size());
        out.batches.push_back(batch);
        out.indices.insert(out.indices.end(), indices.begin(), indices.end());
    }
    return out;
}

std::vector<std::uint32_t> TileMesh::Batch::textures() const
{
    std::vector<std::uint32_t> ids;
    if (texture != kNoTexture) {
        ids.push_back(texture);
    }
    for (const std::uint32_t id : layers) {
        if (id != kNoTexture && std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids.push_back(id);
        }
    }
    return ids;
}

} // namespace fh1
