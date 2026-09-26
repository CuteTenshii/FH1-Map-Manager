#include "PvsFile.h"

#include "BigEndianCursor.h"

#include <QtEndian>

namespace fh1 {

namespace {

constexpr qsizetype kZoneCountOffset = 0x20;
constexpr std::uint32_t kMaxZones = 4096;
/// Bytes between the zone table and the texture table vary; the table is
/// found by its contents within this window.
constexpr qsizetype kTextureTableSearch = 64;
constexpr qsizetype kTextureRecordBytes = 28;
constexpr qsizetype kDrawRecordBytes = 18;
constexpr qsizetype kObjectTrailerBytes = 60;
constexpr std::uint32_t kMaxShaderPathLength = 1024;
constexpr std::uint32_t kMaxObjects = 1'000'000;

void setError(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
}

std::uint32_t u32At(const QByteArray& data, qsizetype offset)
{
    return qFromBigEndian<std::uint32_t>(data.constData() + offset);
}

bool isTextureRecord(const QByteArray& data, qsizetype offset, std::uint32_t number)
{
    constexpr std::uint32_t kOneFloat = 0x3F800000;
    return offset + kTextureRecordBytes <= data.size() && u32At(data, offset + 4) == number
        && u32At(data, offset + 8) == kOneFloat && u32At(data, offset + 12) == kOneFloat;
}

} // namespace

std::optional<PvsTables> readPvs(const QByteArray& pvs, QString* error)
{
    if (!pvs.startsWith("FPVS") || pvs.size() < kZoneCountOffset + 4) {
        setError(error, QStringLiteral("not a PVS file"));
        return std::nullopt;
    }
    const std::uint32_t zones = u32At(pvs, kZoneCountOffset);
    if (zones > kMaxZones) {
        setError(error, QStringLiteral("implausible PVS zone count %1").arg(zones));
        return std::nullopt;
    }
    const qsizetype zoneEnd = kZoneCountOffset + 4 + static_cast<qsizetype>(zones) * 4;

    qsizetype tableStart = -1;
    std::uint32_t recordCount = 0;
    for (qsizetype at = zoneEnd; at < zoneEnd + kTextureTableSearch && at + 4 <= pvs.size(); ++at) {
        const std::uint32_t n = u32At(pvs, at);
        if (n == 0 || at + 4 + static_cast<qsizetype>(n) * kTextureRecordBytes > pvs.size()) {
            continue;
        }
        if (isTextureRecord(pvs, at + 4, 0)
            && isTextureRecord(pvs, at + 4 + static_cast<qsizetype>(n - 1) * kTextureRecordBytes, n - 1)) {
            tableStart = at;
            recordCount = n;
            break;
        }
    }
    if (tableStart < 0) {
        setError(error, QStringLiteral("the PVS file has no texture table where one was expected"));
        return std::nullopt;
    }
    PvsTables tables;
    tables.textureIds.resize(recordCount);
    for (std::uint32_t i = 0; i < recordCount; ++i) {
        tables.textureIds[i] = u32At(pvs, tableStart + 4 + static_cast<qsizetype>(i) * kTextureRecordBytes);
    }

    BigEndianCursor c(pvs, tableStart + 4 + static_cast<qsizetype>(recordCount) * kTextureRecordBytes);
    const std::uint32_t shaderCount = c.u32();
    for (std::uint32_t i = 0; i < shaderCount && c.ok(); ++i) {
        const std::uint32_t length = c.u32();
        if (length > kMaxShaderPathLength) {
            setError(error, QStringLiteral("implausible shader path length in the PVS file"));
            return std::nullopt;
        }
        c.skip(length);
    }
    const std::uint32_t drawCount = c.u32();
    if (!c.ok() || !c.has(static_cast<qsizetype>(drawCount) * kDrawRecordBytes)) {
        setError(error, QStringLiteral("the PVS draw table is truncated"));
        return std::nullopt;
    }
    tables.drawObjects.reserve(drawCount);
    for (std::uint32_t d = 0; d < drawCount; ++d) {
        tables.drawObjects.push_back(c.u16());
        c.skip(kDrawRecordBytes - 2);
    }
    const std::uint32_t objectCount = c.u32();
    if (!c.ok() || objectCount > kMaxObjects) {
        setError(error, QStringLiteral("the PVS object table is truncated"));
        return std::nullopt;
    }
    tables.objectTextures.resize(objectCount);
    for (std::uint32_t o = 0; o < objectCount; ++o) {
        const std::uint32_t textureCount = c.u32();
        if (!c.ok() || !c.has(static_cast<qsizetype>(textureCount) * 4)) {
            setError(error, QStringLiteral("the PVS object table is truncated"));
            return std::nullopt;
        }
        std::vector<std::uint32_t>& textures = tables.objectTextures[o];
        textures.reserve(textureCount);
        for (std::uint32_t t = 0; t < textureCount; ++t) {
            const std::uint32_t record = c.u32();
            if (record >= recordCount) {
                setError(error,
                    QStringLiteral("PVS object %1 refers to texture record %2 of %3")
                        .arg(o)
                        .arg(record)
                        .arg(recordCount));
                return std::nullopt;
            }
            textures.push_back(tables.textureIds[record]);
        }
        const std::uint32_t shaderRefs = c.u32();
        c.skip(static_cast<qsizetype>(shaderRefs) * 4);
        c.skip(kObjectTrailerBytes);
        if (!c.ok()) {
            setError(error, QStringLiteral("the PVS object table is truncated"));
            return std::nullopt;
        }
    }
    return tables;
}

} // namespace fh1
