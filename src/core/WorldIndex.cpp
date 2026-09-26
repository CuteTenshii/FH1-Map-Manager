#include "WorldIndex.h"

#include "RenderMesh.h"
#include "TrackTextures.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <map>

namespace fh1 {

namespace {

constexpr quint32 kCacheMagic = 0x46483157; // "FH1W"
constexpr quint32 kCacheVersion = 7;
/// Models whose centre lies this close to the origin are in local space.
constexpr float kLocalSpaceRadius = 5.0F;
/// Meshes without LOD levels are drawn up to this distance, or further for
/// big ones (terrain cubes), so small detail such as cables fades out.
constexpr float kNoLodMinRange = 1000.0F;
constexpr float kNoLodRangePerMetre = 15.0F;

/// Meshes that are not visible world surface: boundary walls
/// ("UberLOD23_CAGE_LOD00", "TERR_UberLOD_Patch10_CAGE003_LOD00"), a shell
/// around the whole map ("Underground_NOLOD"), shadow-casting stand-ins
/// ("FT02_ShadowCaster_LOD01", "SHADOWBOX_MOUNTAINS_AREA04_LOD01",
/// "Shadow_Terrain_Area04_LOD02"), and a crude copy of the scene
/// ("TERR_CUBE_MainTown_Area3_12", "TERR_CUBE_Plains_Area04_4"): ground,
/// roads and buildings as plain blocks, most likely what the game renders
/// into its reflection cube maps. Every surface of that copy exists in full
/// detail elsewhere; drawn, its buildings stick out as grey blocks.
bool isHelperGeometry(const QString& partName)
{
    static const QRegularExpression pattern(
        QStringLiteral("_CAGE\\d*(_|$)|^Underground|ShadowCaster|ShadowBox|^Shadow_|^TERR_CUBE_"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern.match(partName).hasMatch();
}

/// The far terrain of the whole map ("TERR_UberLOD_Patch07_LOD00",
/// "TERR_UberLOD_Patch18"). It lies within a few metres of the detailed
/// ground where both exist, and alone beyond the drivable area.
bool isBackdrop(const QString& partName)
{
    return partName.contains(QLatin1String("UberLOD"), Qt::CaseInsensitive);
}

/// The distance band a placement's ranges give level `lod`, if they give
/// one: a level ends where the next begins, and the last where the prop
/// stops being drawn. Meshes without levels are drawn up to the furthest.
std::optional<std::pair<float, float>> placementBand(const Placement& placement, int lod)
{
    const auto& ranges = placement.ranges;
    if (lod < 0) {
        const float furthest = *std::max_element(ranges.begin(), ranges.end());
        return furthest > 0.0F ? std::optional<std::pair<float, float>>({0.0F, furthest}) : std::nullopt;
    }
    if (lod >= static_cast<int>(ranges.size())) {
        return std::nullopt;
    }
    const float start = lod == 0 ? 0.0F : ranges[static_cast<std::size_t>(lod) - 1];
    const float end = ranges[static_cast<std::size_t>(lod)];
    if (start < 0.0F || end <= start) {
        return std::nullopt;
    }
    return std::pair<float, float>{start, end};
}

} // namespace

/// A prop model found in the archive, to be placed by the draw records.
struct WorldIndex::LocalModel {
    std::uint32_t entry = 0;
    RenderMeshHeader header;
};

std::optional<WorldIndex> WorldIndex::build(const ForzaZip& archive, const Progress& progress,
    const std::atomic<bool>* cancel, const TrackPlacements* placements)
{
    WorldIndex index;
    QHash<QString, std::uint32_t> groups;
    const auto groupOf = [&index, &groups](const QString& partName) {
        const QString key = rendermesh::lodGroupKey(partName);
        auto group = groups.constFind(key);
        if (group == groups.cend()) {
            group = groups.insert(key, static_cast<std::uint32_t>(index.m_groupKeys.size()));
            index.m_groupKeys.push_back(key);
        }
        return group.value();
    };
    // Props by render object number, placed once every header is read.
    QHash<std::uint32_t, LocalModel> localModels;
    // bin.zip stores most models several times under the same name, with
    // identical contents (same CRC) at different offsets; one copy is enough.
    QSet<QString> seen;
    const auto& entries = archive.entries();
    const int total = static_cast<int>(entries.size());
    for (int i = 0; i < total; ++i) {
        if (cancel != nullptr && cancel->load()) {
            return std::nullopt;
        }
        if (progress && i % 2048 == 0) {
            progress(i, total);
        }
        const ZipEntry& entry = entries[static_cast<std::size_t>(i)];
        if (!entry.name.endsWith(QLatin1String(".rmb.bin"), Qt::CaseInsensitive)) {
            continue;
        }
        const QString name = ForzaZip::normalizeName(entry.name);
        if (seen.contains(name)) {
            continue;
        }
        seen.insert(name);
        const QByteArray prefix = archive.readPrefix(entry, rendermesh::kHeaderBytes);
        const std::optional<RenderMeshHeader> header = rendermesh::readHeader(prefix);
        if (!header || isHelperGeometry(header->firstPartName)) {
            continue;
        }
        const QVector3D centre = (header->boundsMin + header->boundsMax) / 2.0F;
        if (std::hypot(centre.x(), centre.z()) < kLocalSpaceRadius) {
            if (const std::optional<std::uint32_t> object = TrackTextures::objectNumber(entry.name)) {
                localModels.insert(*object, LocalModel{static_cast<std::uint32_t>(i), *header});
            } else {
                ++index.m_localModels;
            }
            continue;
        }
        WorldChunk chunk;
        chunk.entry = static_cast<std::uint32_t>(i);
        chunk.boundsMin = header->boundsMin;
        chunk.boundsMax = header->boundsMax;
        chunk.lod = static_cast<std::int8_t>(std::clamp(rendermesh::lodLevel(header->firstPartName), -1, 3));
        chunk.group = groupOf(header->firstPartName);
        chunk.backdrop = isBackdrop(header->firstPartName);
        index.m_chunks.push_back(chunk);
    }
    if (placements != nullptr) {
        index.placeProps(localModels, *placements, groupOf);
    } else {
        index.m_localModels += static_cast<int>(localModels.size());
    }
    if (progress) {
        progress(total, total);
    }
    index.assignBands();
    return index;
}

void WorldIndex::placeProps(const QHash<std::uint32_t, LocalModel>& models, const TrackPlacements& placements,
    const std::function<std::uint32_t(const QString&)>& groupOf)
{
    // Each level of detail of a prop is a draw record of its own, next to the
    // prop's other levels and with the same transform. Some levels (mostly
    // LOD00) are in no zone file; they take a neighbouring level's
    // transform, so the prop does not vanish up close.
    constexpr int kNeighbourReach = 3;
    const auto modelOf = [&](std::size_t draw) -> const LocalModel* {
        const auto it = models.constFind(placements.drawObject(draw));
        return it == models.cend() ? nullptr : &it.value();
    };
    QSet<std::uint32_t> placedObjects;
    const std::size_t draws = placements.drawCount();
    for (std::size_t d = 0; d < draws; ++d) {
        const LocalModel* model = modelOf(d);
        if (model == nullptr) {
            continue;
        }
        const Placement* placement = placements.placement(d);
        if (placement == nullptr) {
            const QString key = rendermesh::lodGroupKey(model->header.firstPartName);
            for (int step = 1; step <= kNeighbourReach && placement == nullptr; ++step) {
                for (const std::ptrdiff_t n :
                    {static_cast<std::ptrdiff_t>(d) - step, static_cast<std::ptrdiff_t>(d) + step}) {
                    if (n < 0 || static_cast<std::size_t>(n) >= draws) {
                        continue;
                    }
                    const LocalModel* other = modelOf(static_cast<std::size_t>(n));
                    const Placement* candidate = placements.placement(static_cast<std::size_t>(n));
                    if (other != nullptr && candidate != nullptr
                        && rendermesh::lodGroupKey(other->header.firstPartName) == key) {
                        placement = candidate;
                        break;
                    }
                }
            }
        }
        if (placement == nullptr) {
            continue;
        }
        WorldChunk chunk;
        chunk.entry = model->entry;
        chunk.lod = static_cast<std::int8_t>(std::clamp(rendermesh::lodLevel(model->header.firstPartName), -1, 3));
        chunk.group = groupOf(model->header.firstPartName);
        chunk.placed = true;
        chunk.placement = *placement;
        const QVector3D& lo = model->header.boundsMin;
        const QVector3D& hi = model->header.boundsMax;
        for (int corner = 0; corner < 8; ++corner) {
            const QVector3D p = placement->apply(QVector3D((corner & 1) != 0 ? hi.x() : lo.x(),
                (corner & 2) != 0 ? hi.y() : lo.y(), (corner & 4) != 0 ? hi.z() : lo.z()));
            if (corner == 0) {
                chunk.boundsMin = p;
                chunk.boundsMax = p;
            } else {
                chunk.boundsMin = QVector3D(std::min(chunk.boundsMin.x(), p.x()), std::min(chunk.boundsMin.y(), p.y()),
                    std::min(chunk.boundsMin.z(), p.z()));
                chunk.boundsMax = QVector3D(std::max(chunk.boundsMax.x(), p.x()), std::max(chunk.boundsMax.y(), p.y()),
                    std::max(chunk.boundsMax.z(), p.z()));
            }
        }
        m_chunks.push_back(chunk);
        placedObjects.insert(placements.drawObject(d));
    }
    for (auto it = models.cbegin(); it != models.cend(); ++it) {
        if (!placedObjects.contains(it.key())) {
            ++m_localModels;
        }
    }
}

int WorldIndex::placedCount() const
{
    return static_cast<int>(
        std::count_if(m_chunks.begin(), m_chunks.end(), [](const WorldChunk& c) { return c.placed; }));
}

void WorldIndex::assignBands()
{
    std::map<std::uint32_t, std::vector<int>> levelsByGroup;
    for (const WorldChunk& chunk : m_chunks) {
        if (chunk.lod >= 0) {
            levelsByGroup[chunk.group].push_back(chunk.lod);
        }
    }
    for (auto& [group, levels] : levelsByGroup) {
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
    }
    constexpr float kInfinity = std::numeric_limits<float>::infinity();
    for (WorldChunk& chunk : m_chunks) {
        if (chunk.placed) {
            if (const auto band = placementBand(chunk.placement, chunk.lod)) {
                chunk.bandStart = band->first;
                chunk.bandEnd = band->second;
                continue;
            }
        }
        if (chunk.lod < 0) {
            const float diagonal = (chunk.boundsMax - chunk.boundsMin).length();
            chunk.bandStart = 0.0F;
            chunk.bandEnd = std::max(kNoLodMinRange, diagonal * kNoLodRangePerMetre);
            continue;
        }
        const std::vector<int>& levels = levelsByGroup[chunk.group];
        const auto it = std::find(levels.begin(), levels.end(), static_cast<int>(chunk.lod));
        const bool finest = it == levels.begin();
        const bool coarsest = std::next(it) == levels.end();
        chunk.bandStart = finest ? 0.0F : kLodStart[chunk.lod];
        chunk.bandEnd = coarsest ? kInfinity : kLodStart[*std::next(it)];
    }
}

QRectF WorldIndex::footprint() const
{
    if (m_chunks.empty()) {
        return {};
    }
    float minX = m_chunks.front().boundsMin.x();
    float minZ = m_chunks.front().boundsMin.z();
    float maxX = m_chunks.front().boundsMax.x();
    float maxZ = m_chunks.front().boundsMax.z();
    for (const WorldChunk& chunk : m_chunks) {
        minX = std::min(minX, chunk.boundsMin.x());
        minZ = std::min(minZ, chunk.boundsMin.z());
        maxX = std::max(maxX, chunk.boundsMax.x());
        maxZ = std::max(maxZ, chunk.boundsMax.z());
    }
    return QRectF(QPointF(minX, minZ), QPointF(maxX, maxZ));
}

QString WorldIndex::archiveSignature(const QString& archivePath)
{
    const QFileInfo info(archivePath);
    return QStringLiteral("%1|%2|%3")
        .arg(info.canonicalFilePath())
        .arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch());
}

bool WorldIndex::save(const QString& path, const QString& signature, QString* error) const
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    QDataStream out(&file);
    out.setVersion(QDataStream::Qt_6_5);
    out << kCacheMagic << kCacheVersion << signature << static_cast<qint32>(m_localModels);
    out << static_cast<quint32>(m_groupKeys.size());
    for (const QString& key : m_groupKeys) {
        out << key;
    }
    out << static_cast<quint32>(m_chunks.size());
    for (const WorldChunk& c : m_chunks) {
        out << c.entry << c.boundsMin << c.boundsMax << static_cast<qint8>(c.lod) << c.group << c.backdrop << c.placed;
        if (c.placed) {
            out << c.placement.position;
            for (const float value : c.placement.rows) {
                out << value;
            }
            for (const float value : c.placement.ranges) {
                out << value;
            }
        }
    }
    if (!file.commit()) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

std::optional<WorldIndex> WorldIndex::load(const QString& path, const QString& signature, QString* error)
{
    auto fail = [error](const QString& message) -> std::optional<WorldIndex> {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(file.errorString());
    }
    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_6_5);
    quint32 magic = 0;
    quint32 version = 0;
    QString storedSignature;
    qint32 localModels = 0;
    in >> magic >> version >> storedSignature >> localModels;
    if (magic != kCacheMagic || version != kCacheVersion) {
        return fail(QStringLiteral("not a world index cache of this version"));
    }
    if (storedSignature != signature) {
        return fail(QStringLiteral("cache was built from a different archive"));
    }
    WorldIndex index;
    index.m_localModels = localModels;
    quint32 groupCount = 0;
    in >> groupCount;
    index.m_groupKeys.resize(groupCount);
    for (QString& key : index.m_groupKeys) {
        in >> key;
    }
    quint32 chunkCount = 0;
    in >> chunkCount;
    index.m_chunks.resize(chunkCount);
    for (WorldChunk& c : index.m_chunks) {
        qint8 lod = 0;
        in >> c.entry >> c.boundsMin >> c.boundsMax >> lod >> c.group >> c.backdrop >> c.placed;
        c.lod = lod;
        if (c.placed) {
            in >> c.placement.position;
            for (float& value : c.placement.rows) {
                in >> value;
            }
            for (float& value : c.placement.ranges) {
                in >> value;
            }
        }
    }
    if (in.status() != QDataStream::Ok) {
        return fail(QStringLiteral("cache file is truncated"));
    }
    index.assignBands();
    return index;
}

} // namespace fh1
