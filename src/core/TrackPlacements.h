#pragma once

#include "ForzaZip.h"

#include <QByteArray>
#include <QString>
#include <QVector3D>

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace fh1 {

/// Where one draw record places its render object.
struct Placement {
    /// World position of the model's origin.
    QVector3D position;
    /// 3x3 matrix, row by row, applied to the model's coordinates as stored
    /// in its file (Z not yet negated): world = x·row0 + y·row1 + z·row2 +
    /// position. The rows can carry scale as well as rotation.
    std::array<float, 9> rows{};
    /// The first three values of the record, in metres; they look like the
    /// distances at which the game switches to the next level of detail and
    /// stops drawing the model. Negative when unused. Inferred from the data.
    std::array<float, 3> ranges{};
    /// True if the game draws the prop only while an event or a script
    /// switches it on: race barriers, chevrons and banners, festival set
    /// dressing, the open and closed states of barn-find barns. Such records
    /// carry a block whose u32 is 0xFFFFFFFF; breakable props (road signs,
    /// cones, bins) carry blocks with other values and are always there.
    /// Inferred from which models carry which blocks.
    bool eventProp = false;

    /// Maps a point of the parsed model (whose Z the parser already negated)
    /// to world coordinates.
    QVector3D apply(const QVector3D& parsed) const;
};

/// Where a track places each draw record's model, read from its zone files
/// (`__R00Z#####.pvsz` in bin.zip, big-endian). A zone file holds, each as a
/// u32 count followed by its entries:
///
///     n u32 draws: the low 16 bits index the PVS file's draw records
///     u16 values, 16-byte entries, u32 values, bytes, u16 values, u32
///     values, bytes, 16-byte entries, n bytes     (not needed here)
///     n transform records, one per draw above, in the same order:
///         3 half floats (ranges), 3 f32 position, 9 half floats (rows),
///         16 zero bytes, u8 c, then c blocks of u32 a, u8 m, m bytes
///         and 32 bytes; a is 0xFFFFFFFF for event props (see
///         Placement::eventProp), the rest is not needed here
///
/// Zones overlap, so a draw appears in several; its transform is the same in
/// each. Each level of detail of a prop is a draw record of its own, with
/// the same transform as its other levels. The layout reads every zone of
/// Colorado to its end and was matched against CollObjs.xml and against the
/// world-space models; it is inferred from the data, not from game code.
class TrackPlacements {
public:
    /// Reads the draw table of `pvs` and every zone file in `archive`.
    /// Returns nothing if the PVS file cannot be read, with `error` set, or if
    /// `cancel` was set while it ran. Zone files that cannot be read are
    /// counted in failedZones().
    static std::optional<TrackPlacements> load(const QByteArray& pvs, const ForzaZip& archive,
        const std::atomic<bool>* cancel = nullptr, QString* error = nullptr);

    /// Parses one zone file into (draw record, placement) pairs.
    static std::optional<std::vector<std::pair<std::uint32_t, Placement>>> readZone(
        const QByteArray& zone, QString* error = nullptr);

    std::size_t drawCount() const { return m_drawObjects.size(); }
    /// The render object that draw record `draw` places.
    std::uint16_t drawObject(std::size_t draw) const { return m_drawObjects[draw]; }
    /// Where draw record `draw` is placed, or nullptr if no zone says.
    const Placement* placement(std::size_t draw) const;
    int zoneCount() const { return m_zones; }
    int failedZones() const { return m_failedZones; }

private:
    std::vector<std::uint16_t> m_drawObjects;
    std::vector<std::optional<Placement>> m_placements;
    int m_zones = 0;
    int m_failedZones = 0;
};

} // namespace fh1
