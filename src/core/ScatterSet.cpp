#include "ScatterSet.h"

#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstring>

namespace fh1 {

namespace {

constexpr std::uint32_t kMagic = 0x4F454750; // "OEGP"
constexpr qsizetype kExtraCountOffset = 0x40;
constexpr qsizetype kFileSizeOffset = 0x48;
constexpr qsizetype kMeshCountOffset = 0x54;
constexpr qsizetype kNameOffset = 0x60;
constexpr qsizetype kNameBytes = 0x1C;
constexpr qsizetype kMeshTableOffset = 0x80;
constexpr qsizetype kMeshBytes = 64;
constexpr qsizetype kMeshPathLength = 32;
constexpr qsizetype kMeshSlots = 36;
constexpr int kSlotCount = 3;
constexpr qsizetype kExtraBytes = 8;
constexpr qsizetype kGroupPreambleBytes = 20;
constexpr qsizetype kGroupCountInPreamble = 12;
constexpr qsizetype kGroupBytes = 92;
constexpr qsizetype kGroupMesh = 44;
constexpr qsizetype kGroupCount = 48;
constexpr qsizetype kGroupBlocks = 56;
constexpr qsizetype kGroupDrawDistance = 60;
constexpr qsizetype kInstanceBytes = 96;
constexpr qsizetype kInstancePosition = 48;
constexpr qsizetype kBlockBytes = 32;
constexpr std::uint32_t kMaxMeshes = 1024;
constexpr std::uint32_t kMaxGroups = 1024;
constexpr std::uint32_t kMaxExtra = 4096;
constexpr std::uint32_t kMaxPathLength = 1024;
constexpr std::uint32_t kMaxBlocks = 16;
/// Used when a group has no block of switch distances: its levels switch at
/// these fractions of its draw distance. A viewer choice, not game data.
constexpr float kFallbackFirstSwitch = 0.25F;
constexpr float kFallbackSecondSwitch = 0.5F;

void setError(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
}

/// Bounds-checked reads at absolute offsets; a read past the end returns 0
/// and marks the reader failed.
class Reader {
public:
    explicit Reader(const QByteArray& data)
        : m_data(data)
    {
    }

    bool ok() const { return m_ok; }
    bool has(qsizetype offset, qsizetype bytes)
    {
        if (offset < 0 || bytes < 0 || offset + bytes > m_data.size()) {
            m_ok = false;
            return false;
        }
        return true;
    }
    std::uint32_t u32(qsizetype offset)
    {
        return has(offset, 4) ? qFromBigEndian<std::uint32_t>(m_data.constData() + offset) : 0;
    }
    float f32(qsizetype offset)
    {
        const std::uint32_t bits = u32(offset);
        float value = 0.0F;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
    QVector3D vec3(qsizetype offset) { return {f32(offset), f32(offset + 4), f32(offset + 8)}; }

private:
    const QByteArray& m_data;
    bool m_ok = true;
};

qsizetype alignUp(qsizetype offset, qsizetype alignment)
{
    return (offset + alignment - 1) / alignment * alignment;
}

/// The distances at which the levels of a `levels`-level mesh end: each
/// level ends where the next begins and the last where drawing stops.
std::array<float, 3> levelRanges(int levels, float firstSwitch, float secondSwitch, float drawDistance)
{
    std::array<float, 3> ranges{-1.0F, -1.0F, -1.0F};
    const std::array<float, 2> switches{firstSwitch, secondSwitch};
    for (int level = 0; level + 1 < levels; ++level) {
        ranges[static_cast<std::size_t>(level)] = switches[static_cast<std::size_t>(level)];
    }
    ranges[static_cast<std::size_t>(std::max(levels, 1) - 1)] = drawDistance;
    return ranges;
}

} // namespace

std::optional<ScatterSet> readScatterSet(const QByteArray& data, QString* error)
{
    Reader r(data);
    if (data.size() < kMeshTableOffset || r.u32(0) != kMagic) {
        setError(error, QStringLiteral("not a procedural placement set"));
        return std::nullopt;
    }
    if (r.u32(kFileSizeOffset) != static_cast<std::uint32_t>(data.size())) {
        setError(error, QStringLiteral("the set's size field does not match its size"));
        return std::nullopt;
    }
    const std::uint32_t meshCount = r.u32(kMeshCountOffset);
    const std::uint32_t extraCount = r.u32(kExtraCountOffset);
    if (meshCount == 0 || meshCount > kMaxMeshes || extraCount > kMaxExtra) {
        setError(error, QStringLiteral("implausible mesh or entry counts"));
        return std::nullopt;
    }
    ScatterSet set;
    set.name = QString::fromLatin1(data.mid(kNameOffset, kNameBytes).constData());

    // Mesh entries: which carry draw records, and how many levels each has.
    std::vector<int> levels(meshCount, 0);
    std::vector<std::uint32_t> pathLengths(meshCount, 0);
    for (std::uint32_t m = 0; m < meshCount; ++m) {
        const qsizetype entry = kMeshTableOffset + static_cast<qsizetype>(m) * kMeshBytes;
        pathLengths[m] = r.u32(entry + kMeshPathLength);
        if (pathLengths[m] > kMaxPathLength) {
            setError(error, QStringLiteral("mesh %1 has an implausible path length").arg(m));
            return std::nullopt;
        }
        if (pathLengths[m] == 0) {
            while (levels[m] < kSlotCount && r.u32(entry + kMeshSlots + 8 * static_cast<qsizetype>(levels[m])) == 1) {
                ++levels[m];
            }
            if (levels[m] == 0) {
                setError(error, QStringLiteral("mesh %1 has neither levels of detail nor a path").arg(m));
                return std::nullopt;
            }
        }
    }
    qsizetype offset = kMeshTableOffset + static_cast<qsizetype>(meshCount) * kMeshBytes;
    set.meshDraws.resize(meshCount);
    for (std::uint32_t m = 0; m < meshCount; ++m) {
        for (int level = 0; level < levels[m]; ++level) {
            set.meshDraws[m].push_back(r.u32(offset));
            offset += 4;
        }
    }
    for (const std::uint32_t length : pathLengths) {
        offset += length;
    }
    offset = alignUp(offset, 4) + static_cast<qsizetype>(extraCount) * kExtraBytes;
    const std::uint32_t groupCount = r.u32(offset + kGroupCountInPreamble);
    offset += kGroupPreambleBytes;
    if (!r.ok() || groupCount > kMaxGroups || !r.has(offset, static_cast<qsizetype>(groupCount) * kGroupBytes)) {
        setError(error, QStringLiteral("the set's group table is truncated"));
        return std::nullopt;
    }

    struct Group {
        std::uint32_t mesh = 0;
        std::uint32_t count = 0;
        std::uint32_t blocks = 0;
        float drawDistance = 0.0F;
    };
    std::vector<Group> groups(groupCount);
    for (std::uint32_t g = 0; g < groupCount; ++g) {
        const qsizetype entry = offset + static_cast<qsizetype>(g) * kGroupBytes;
        groups[g] = {r.u32(entry + kGroupMesh), r.u32(entry + kGroupCount), r.u32(entry + kGroupBlocks),
            r.f32(entry + kGroupDrawDistance)};
        if (groups[g].mesh >= meshCount || groups[g].blocks > kMaxBlocks) {
            setError(error, QStringLiteral("group %1 of the set is not valid").arg(g));
            return std::nullopt;
        }
    }
    offset = alignUp(offset + static_cast<qsizetype>(groupCount) * kGroupBytes, 16);

    for (const Group& group : groups) {
        const qsizetype instances = offset;
        const qsizetype blocks = offset + static_cast<qsizetype>(group.count) * kInstanceBytes;
        if (!r.has(instances, static_cast<qsizetype>(group.count) * kInstanceBytes + group.blocks * kBlockBytes)) {
            setError(error, QStringLiteral("the set's instances are truncated"));
            return std::nullopt;
        }
        offset = blocks + static_cast<qsizetype>(group.blocks) * kBlockBytes;
        const int meshLevels = levels[group.mesh];
        if (meshLevels == 0 || group.count == 0 || !(group.drawDistance > 0.0F)) {
            continue;
        }
        // Blocks that switch no level hold zeros; the first that does gives
        // the switch distances.
        float firstSwitch = group.drawDistance * kFallbackFirstSwitch;
        float secondSwitch = group.drawDistance * kFallbackSecondSwitch;
        for (std::uint32_t b = 0; b < group.blocks; ++b) {
            const float first = r.f32(blocks + b * kBlockBytes);
            const float second = r.f32(blocks + b * kBlockBytes + 4);
            if (first > 0.0F && second >= first && second <= group.drawDistance) {
                firstSwitch = first;
                secondSwitch = second;
                break;
            }
        }
        const std::array<float, 3> ranges = levelRanges(meshLevels, firstSwitch, secondSwitch, group.drawDistance);
        for (std::uint32_t i = 0; i < group.count; ++i) {
            const qsizetype record = instances + static_cast<qsizetype>(i) * kInstanceBytes;
            if (r.f32(record + kInstancePosition + 12) != 1.0F) {
                setError(error, QStringLiteral("instance %1 of the set is not valid").arg(i));
                return std::nullopt;
            }
            ScatterSet::Instance instance;
            instance.mesh = group.mesh;
            instance.placement.position = r.vec3(record + kInstancePosition);
            // The axes form a proper rotation; placements map the model's
            // stored coordinates, whose Z axis points the other way.
            const QVector3D x = r.vec3(record);
            const QVector3D y = r.vec3(record + 16);
            const QVector3D z = -r.vec3(record + 32);
            instance.placement.rows = {x.x(), x.y(), x.z(), y.x(), y.y(), y.z(), z.x(), z.y(), z.z()};
            instance.placement.ranges = ranges;
            set.instances.push_back(instance);
        }
    }
    if (!r.ok()) {
        setError(error, QStringLiteral("the set is truncated"));
        return std::nullopt;
    }
    return set;
}

} // namespace fh1
