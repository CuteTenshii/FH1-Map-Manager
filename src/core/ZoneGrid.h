#pragma once

#include <QByteArray>
#include <QDataStream>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace fh1 {

/// Which visibility zone (see TrackPlacements) covers each point of a track,
/// read from `Ribbon_00/<Track>_00.hex` (big-endian):
///
///     "HEXY", u32 version (101), f32 cell radius (100),
///     f32 origin X, f32 origin Z, u32 zone count, u32 columns, u32 rows,
///     columns × rows u32 zone numbers, row by row, 0xFFFFFFFF where no zone
///     is; then 9 bytes per zone, not needed here
///
/// The cells are flat-topped hexagons: column c, row r has its centre at
/// X = originX + radius·(1 + 1.5c), Z = originZ + radius·√3·(r + ½), and odd
/// columns sit half a row further along Z. Zone n is the zone file
/// `__R00Z<n>.pvsz`. The grid shape comes from the header; the placement of
/// the cells was matched against the props each zone lists, not taken from
/// game code.
class ZoneGrid {
public:
    /// Parses a `.hex` file. Returns nothing, with `error` set, if it does
    /// not have the expected layout.
    static std::optional<ZoneGrid> read(const QByteArray& data, QString* error = nullptr);
    /// Reads and parses the file at `path`.
    static std::optional<ZoneGrid> readFile(const QString& path, QString* error = nullptr);

    /// The zone of the cell whose centre is nearest to X, Z, or -1 outside
    /// the grid or in a cell without a zone.
    int zoneAt(float x, float z) const;
    int zoneCount() const { return m_zoneCount; }

    friend QDataStream& operator<<(QDataStream& out, const ZoneGrid& grid);
    friend QDataStream& operator>>(QDataStream& in, ZoneGrid& grid);

private:
    int zoneOfCell(int column, int row) const;

    float m_radius = 0.0F;
    float m_originX = 0.0F;
    float m_originZ = 0.0F;
    int m_zoneCount = 0;
    int m_columns = 0;
    int m_rows = 0;
    /// Zone per cell, row by row; -1 for none.
    std::vector<std::int32_t> m_cells;
};

} // namespace fh1
