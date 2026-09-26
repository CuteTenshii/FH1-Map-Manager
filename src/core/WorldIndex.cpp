#include "WorldIndex.h"

#include "RenderMesh.h"

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
constexpr quint32 kCacheVersion = 4;
/// Models whose centre lies this close to the origin are in local space.
constexpr float kLocalSpaceRadius = 5.0F;
/// Meshes without LOD levels are drawn up to this distance, or further for
/// big ones (terrain cubes), so small detail such as cables fades out.
constexpr float kNoLodMinRange = 1000.0F;
constexpr float kNoLodRangePerMetre = 15.0F;

/// Meshes that are not visible world surface: boundary walls
/// ("UberLOD23_CAGE_LOD00", "TERR_UberLOD_Patch10_CAGE003_LOD00"), a shell
/// around the whole map ("Underground_NOLOD") and shadow-casting stand-ins
/// ("FT02_ShadowCaster_LOD01", "SHADOWBOX_MOUNTAINS_AREA04_LOD01",
/// "Shadow_Terrain_Area04_LOD02"). Drawn without the game's materials they
/// show up as large grey blocks.
bool isHelperGeometry(const QString& partName)
{
    static const QRegularExpression pattern(
        QStringLiteral("_CAGE\\d*(_|$)|^Underground|ShadowCaster|ShadowBox|^Shadow_"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern.match(partName).hasMatch();
}

} // namespace

std::optional<WorldIndex> WorldIndex::build(
    const ForzaZip& archive, const Progress& progress, const std::atomic<bool>* cancel)
{
    WorldIndex index;
    QHash<QString, std::uint32_t> groups;
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
            ++index.m_localModels;
            continue;
        }
        const QString key = rendermesh::lodGroupKey(header->firstPartName);
        auto group = groups.constFind(key);
        if (group == groups.cend()) {
            group = groups.insert(key, static_cast<std::uint32_t>(index.m_groupKeys.size()));
            index.m_groupKeys.push_back(key);
        }
        WorldChunk chunk;
        chunk.entry = static_cast<std::uint32_t>(i);
        chunk.boundsMin = header->boundsMin;
        chunk.boundsMax = header->boundsMax;
        chunk.lod = static_cast<std::int8_t>(std::clamp(rendermesh::lodLevel(header->firstPartName), -1, 3));
        chunk.group = group.value();
        index.m_chunks.push_back(chunk);
    }
    if (progress) {
        progress(total, total);
    }
    index.assignBands();
    return index;
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
        out << c.entry << c.boundsMin << c.boundsMax << static_cast<qint8>(c.lod) << c.group;
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
        in >> c.entry >> c.boundsMin >> c.boundsMax >> lod >> c.group;
        c.lod = lod;
    }
    if (in.status() != QDataStream::Ok) {
        return fail(QStringLiteral("cache file is truncated"));
    }
    index.assignBands();
    return index;
}

} // namespace fh1
