#include "ZoneGrid.h"

#include "BigEndianCursor.h"

#include <QFile>

#include <cmath>
#include <limits>
#include <numbers>

namespace fh1 {

namespace {

constexpr std::uint32_t kMagic = 0x48455859; // "HEXY"
constexpr std::uint32_t kNoZone = 0xFFFFFFFF;
constexpr std::uint32_t kMaxCells = 1'000'000;
constexpr float kSqrt3 = std::numbers::sqrt3_v<float>;

void setError(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

std::optional<ZoneGrid> ZoneGrid::read(const QByteArray& data, QString* error)
{
    BigEndianCursor c(data);
    if (c.u32() != kMagic) {
        setError(error, QStringLiteral("not a zone grid file"));
        return std::nullopt;
    }
    c.skip(4);
    ZoneGrid grid;
    grid.m_radius = c.f32();
    grid.m_originX = c.f32();
    grid.m_originZ = c.f32();
    const std::uint32_t zones = c.u32();
    const std::uint32_t columns = c.u32();
    const std::uint32_t rows = c.u32();
    if (!c.ok() || !(grid.m_radius > 0.0F) || !std::isfinite(grid.m_originX) || !std::isfinite(grid.m_originZ)
        || columns == 0 || rows == 0 || columns > kMaxCells / rows || zones > kMaxCells) {
        setError(error, QStringLiteral("the zone grid header is not valid"));
        return std::nullopt;
    }
    if (!c.has(static_cast<qsizetype>(columns) * rows * 4)) {
        setError(error, QStringLiteral("the zone grid is truncated"));
        return std::nullopt;
    }
    grid.m_zoneCount = static_cast<int>(zones);
    grid.m_columns = static_cast<int>(columns);
    grid.m_rows = static_cast<int>(rows);
    grid.m_cells.reserve(static_cast<std::size_t>(columns) * rows);
    for (std::uint32_t i = 0; i < columns * rows; ++i) {
        const std::uint32_t zone = c.u32();
        if (zone != kNoZone && zone >= zones) {
            setError(error, QStringLiteral("zone grid cell %1 names zone %2 of %3").arg(i).arg(zone).arg(zones));
            return std::nullopt;
        }
        grid.m_cells.push_back(zone == kNoZone ? -1 : static_cast<std::int32_t>(zone));
    }
    return grid;
}

std::optional<ZoneGrid> ZoneGrid::readFile(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, file.errorString());
        return std::nullopt;
    }
    return read(file.readAll(), error);
}

int ZoneGrid::zoneOfCell(int column, int row) const
{
    return m_cells[static_cast<std::size_t>(row) * static_cast<std::size_t>(m_columns)
        + static_cast<std::size_t>(column)];
}

int ZoneGrid::zoneAt(float x, float z) const
{
    if (m_cells.empty()) {
        return -1;
    }
    const float rowPitch = kSqrt3 * m_radius;
    const int nearColumn = static_cast<int>(std::floor((x - m_originX - m_radius) / (1.5F * m_radius)));
    float best = std::numeric_limits<float>::infinity();
    int zone = -1;
    // The nearest centre is always in one of the columns next to the one
    // the point falls in, and in the row nearest to it within that column.
    for (int column = nearColumn - 1; column <= nearColumn + 2; ++column) {
        if (column < 0 || column >= m_columns) {
            continue;
        }
        const float centreX = m_originX + m_radius * (1.0F + 1.5F * static_cast<float>(column));
        const float firstCentreZ = m_originZ + rowPitch * ((column & 1) != 0 ? 1.0F : 0.5F);
        const int nearRow = static_cast<int>(std::lround((z - firstCentreZ) / rowPitch));
        for (int row = nearRow - 1; row <= nearRow + 1; ++row) {
            if (row < 0 || row >= m_rows) {
                continue;
            }
            const float distance = std::hypot(x - centreX, z - (firstCentreZ + rowPitch * static_cast<float>(row)));
            if (distance < best) {
                best = distance;
                zone = zoneOfCell(column, row);
            }
        }
    }
    // Past the grid's edge the nearest cell is further than its own corners.
    return best <= m_radius ? zone : -1;
}

QDataStream& operator<<(QDataStream& out, const ZoneGrid& grid)
{
    out << grid.m_radius << grid.m_originX << grid.m_originZ << static_cast<qint32>(grid.m_zoneCount)
        << static_cast<qint32>(grid.m_columns) << static_cast<qint32>(grid.m_rows);
    for (const std::int32_t zone : grid.m_cells) {
        out << static_cast<qint32>(zone);
    }
    return out;
}

QDataStream& operator>>(QDataStream& in, ZoneGrid& grid)
{
    qint32 zones = 0;
    qint32 columns = 0;
    qint32 rows = 0;
    in >> grid.m_radius >> grid.m_originX >> grid.m_originZ >> zones >> columns >> rows;
    if (in.status() != QDataStream::Ok || columns <= 0 || rows <= 0
        || columns > static_cast<qint32>(kMaxCells) / rows) {
        in.setStatus(QDataStream::ReadCorruptData);
        return in;
    }
    grid.m_zoneCount = zones;
    grid.m_columns = columns;
    grid.m_rows = rows;
    grid.m_cells.resize(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows));
    for (std::int32_t& zone : grid.m_cells) {
        qint32 value = 0;
        in >> value;
        zone = value;
    }
    return in;
}

} // namespace fh1
