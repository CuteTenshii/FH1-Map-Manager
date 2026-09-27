#pragma once

#include "ForzaZip.h"
#include "WorldIndex.h"

#include <QByteArray>
#include <QHash>
#include <QString>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace fh1 {

/// Where the zone files of a track's archive place each draw record: every
/// transform record, in every zone file entry, copies included.
class ZoneRecordIndex {
public:
    /// Reads every `.pvsz` entry of `archive`. Returns nothing, with `error`
    /// set, if one cannot be read.
    static std::optional<ZoneRecordIndex> build(const ForzaZip& archive, QString* error = nullptr);

    /// The (archive entry, record offset) pairs placing draw record `draw`.
    const std::vector<std::pair<std::uint32_t, qsizetype>>& recordsOf(std::uint32_t draw) const;

private:
    QHash<std::uint32_t, std::vector<std::pair<std::uint32_t, qsizetype>>> m_records;
};

/// The chunks of `index` that are the same model placed once, as chunk
/// `chunk` is: its levels of detail, placed alike. Chunk `chunk` comes
/// first; empty if it is not a placed model.
std::vector<std::uint32_t> placedModelChunks(const WorldIndex& index, std::uint32_t chunk);

/// Removes the placed model that chunk `chunk` belongs to from the map:
/// zeroes the matrix of each zone record placing it, or the axes of its
/// procedural copy, in every copy of the files involved. `edited` holds the
/// archive entries already changed, by entry index, and receives the new
/// contents; entries not in it are read from `archive`. Returns false, with
/// `error` set, if the chunk is not a placed model or a file cannot be read.
bool removePlacedModel(const ForzaZip& archive, const WorldIndex& index, const ZoneRecordIndex& zones,
    std::uint32_t chunk, QHash<std::uint32_t, QByteArray>& edited, QString* error = nullptr);

} // namespace fh1
