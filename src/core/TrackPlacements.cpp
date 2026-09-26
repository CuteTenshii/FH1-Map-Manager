#include "TrackPlacements.h"

#include "BigEndianCursor.h"
#include "PvsFile.h"

#include <QRegularExpression>
#include <QSet>
#include <QtConcurrent/QtConcurrentMap>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fh1 {

namespace {

constexpr std::uint32_t kMaxZoneEntries = 1'000'000;
/// Bytes of a transform record before its variable part: 3 half floats,
/// 3 floats, 9 half floats and 16 zero bytes.
constexpr qsizetype kRecordFixedBytes = 52;
constexpr qsizetype kBlockTrailerBytes = 32;
/// Block id of props that only an event or a script shows.
constexpr std::uint32_t kEventBlock = 0xFFFFFFFF;

void setError(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
}

/// Reads an IEEE 754 half-precision float.
float half(BigEndianCursor& c)
{
    const std::uint16_t bits = c.u16();
    const float sign = (bits & 0x8000U) != 0 ? -1.0F : 1.0F;
    const int exponent = (bits >> 10) & 0x1F;
    const auto mantissa = static_cast<float>(bits & 0x3FFU);
    if (exponent == 0) {
        return sign * std::ldexp(mantissa, -24);
    }
    if (exponent == 0x1F) {
        return mantissa == 0.0F ? sign * std::numeric_limits<float>::infinity()
                                : std::numeric_limits<float>::quiet_NaN();
    }
    return sign * std::ldexp(1024.0F + mantissa, exponent - 25);
}

/// Skips a section: a u32 count and `count * bytesPerEntry` bytes.
void skipSection(BigEndianCursor& c, qsizetype bytesPerEntry)
{
    const std::uint32_t count = c.u32();
    if (count > kMaxZoneEntries) {
        c.skip(-1);
        return;
    }
    c.skip(static_cast<qsizetype>(count) * bytesPerEntry);
}

} // namespace

QVector3D Placement::apply(const QVector3D& parsed) const
{
    // The parser negated the file's Z, so it is negated back here.
    const float x = parsed.x();
    const float y = parsed.y();
    const float z = -parsed.z();
    return {x * rows[0] + y * rows[3] + z * rows[6] + position.x(),
        x * rows[1] + y * rows[4] + z * rows[7] + position.y(), x * rows[2] + y * rows[5] + z * rows[8] + position.z()};
}

std::optional<std::vector<std::pair<std::uint32_t, Placement>>> TrackPlacements::readZone(
    const QByteArray& zone, QString* error)
{
    BigEndianCursor c(zone);
    const std::uint32_t drawCount = c.u32();
    if (!c.ok() || drawCount > kMaxZoneEntries || !c.has(static_cast<qsizetype>(drawCount) * 4)) {
        setError(error, QStringLiteral("not a zone file"));
        return std::nullopt;
    }
    std::vector<std::uint32_t> draws(drawCount);
    for (std::uint32_t& draw : draws) {
        draw = c.u32() & 0xFFFF;
    }
    skipSection(c, 2);
    skipSection(c, 16);
    skipSection(c, 4);
    skipSection(c, 1);
    skipSection(c, 2);
    skipSection(c, 4);
    skipSection(c, 1);
    skipSection(c, 16);
    skipSection(c, 1);
    const std::uint32_t recordCount = c.u32();
    if (!c.ok() || recordCount != drawCount) {
        setError(
            error, QStringLiteral("zone file has %1 transform records for %2 draws").arg(recordCount).arg(drawCount));
        return std::nullopt;
    }
    std::vector<std::pair<std::uint32_t, Placement>> placements;
    placements.reserve(drawCount);
    for (std::uint32_t k = 0; k < recordCount; ++k) {
        if (!c.has(kRecordFixedBytes + 1)) {
            break;
        }
        Placement placement;
        for (float& range : placement.ranges) {
            range = half(c);
        }
        const float px = c.f32();
        const float py = c.f32();
        const float pz = c.f32();
        placement.position = QVector3D(px, py, pz);
        for (float& value : placement.rows) {
            value = half(c);
        }
        c.skip(16);
        const std::uint8_t blocks = c.u8();
        for (std::uint8_t b = 0; b < blocks && c.ok(); ++b) {
            if (c.u32() == kEventBlock) {
                placement.eventProp = true;
            }
            const std::uint8_t bytes = c.u8();
            c.skip(bytes + kBlockTrailerBytes);
        }
        placements.emplace_back(draws[k], placement);
    }
    if (!c.ok() || placements.size() != drawCount) {
        setError(error, QStringLiteral("zone file is truncated"));
        return std::nullopt;
    }
    return placements;
}

std::optional<TrackPlacements> TrackPlacements::load(
    const QByteArray& pvs, const ForzaZip& archive, const std::atomic<bool>* cancel, QString* error)
{
    std::optional<PvsTables> tables = readPvs(pvs, error);
    if (!tables) {
        return std::nullopt;
    }
    TrackPlacements result;
    result.m_drawObjects = std::move(tables->drawObjects);
    result.m_placements.resize(result.m_drawObjects.size());

    result.m_zonesListing.resize(result.m_drawObjects.size());

    // Zone files are stored several times, some under names differing only
    // in case; one copy of each is enough.
    static const QRegularExpression zoneNumber(
        QStringLiteral("z(\\d+)\\.pvsz$"), QRegularExpression::CaseInsensitiveOption);
    std::vector<std::size_t> zones;
    std::vector<int> zoneNumbers;
    QSet<QString> names;
    const auto& entries = archive.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].name.endsWith(QLatin1String(".pvsz"), Qt::CaseInsensitive)) {
            continue;
        }
        const QString name = ForzaZip::normalizeName(entries[i].name);
        if (!names.contains(name)) {
            names.insert(name);
            zones.push_back(i);
            const QRegularExpressionMatch match = zoneNumber.match(name);
            bool ok = false;
            const int number = match.hasMatch() ? match.captured(1).toInt(&ok) : -1;
            zoneNumbers.push_back(ok && number <= std::numeric_limits<std::uint16_t>::max() ? number : -1);
        }
    }
    using ZonePlacements = std::optional<std::vector<std::pair<std::uint32_t, Placement>>>;
    const std::vector<ZonePlacements> parsed
        = QtConcurrent::blockingMapped<std::vector<ZonePlacements>>(zones, [&archive, cancel](std::size_t entry) {
              if (cancel != nullptr && cancel->load()) {
                  return ZonePlacements();
              }
              const QByteArray data = archive.read(archive.entries()[entry]);
              return data.isNull() ? ZonePlacements() : readZone(data);
          });
    if (cancel != nullptr && cancel->load()) {
        return std::nullopt;
    }
    result.m_zones = static_cast<int>(zones.size());
    for (std::size_t z = 0; z < parsed.size(); ++z) {
        const ZonePlacements& zone = parsed[z];
        if (!zone) {
            ++result.m_failedZones;
            continue;
        }
        for (const auto& [draw, placement] : *zone) {
            if (draw >= result.m_placements.size()) {
                continue;
            }
            if (!result.m_placements[draw]) {
                result.m_placements[draw] = placement;
            }
            if (zoneNumbers[z] >= 0) {
                result.m_zonesListing[draw].push_back(static_cast<std::uint16_t>(zoneNumbers[z]));
            }
        }
    }
    for (std::vector<std::uint16_t>& listing : result.m_zonesListing) {
        std::sort(listing.begin(), listing.end());
        listing.erase(std::unique(listing.begin(), listing.end()), listing.end());
    }
    return result;
}

const std::vector<std::uint16_t>& TrackPlacements::zonesListing(std::size_t draw) const
{
    static const std::vector<std::uint16_t> kNone;
    return draw < m_zonesListing.size() ? m_zonesListing[draw] : kNone;
}

const Placement* TrackPlacements::placement(std::size_t draw) const
{
    if (draw >= m_placements.size()) {
        return nullptr;
    }
    const std::optional<Placement>& placement = m_placements[draw];
    return placement.has_value() ? &placement.value() : nullptr;
}

} // namespace fh1
