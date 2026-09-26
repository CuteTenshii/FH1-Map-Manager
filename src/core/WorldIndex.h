#pragma once

#include "ForzaZip.h"
#include "TrackPlacements.h"

#include <QHash>
#include <QRectF>
#include <QString>
#include <QVector3D>

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

namespace fh1 {

/// One render model drawn in the world: a world-space model of a track's
/// bin.zip, or one placement of a prop modelled in its own local space. It
/// carries the range of camera distances at which the 3D view draws it.
struct WorldChunk {
    /// Index into ForzaZip::entries() of the archive the index was built from.
    std::uint32_t entry = 0;
    /// World bounds (for a placed prop, of its transformed model bounds).
    QVector3D boundsMin;
    QVector3D boundsMax;
    /// Level of detail from the part name, or -1 for meshes without levels.
    std::int8_t lod = -1;
    /// Chunks whose names differ only in LOD level and chunk number share a
    /// group; the group's levels decide each chunk's distance band.
    std::uint32_t group = 0;
    float bandStart = 0.0F;
    float bandEnd = std::numeric_limits<float>::infinity();
    /// Part of the map-wide far terrain, which only shows where no other
    /// geometry covers the same ground (see WorldRenderer).
    bool backdrop = false;
    /// A prop drawn where `placement` puts it; world-space models are drawn
    /// as stored.
    bool placed = false;
    Placement placement;
};

/// The render models of a track, found by reading only the header of every
/// `*.rmb.bin` in its archive. Duplicate copies of a model are indexed once.
/// Models centred on the origin are props in their own local space: each of
/// their placements (see TrackPlacements) becomes a chunk of its own, and
/// those never placed are only counted. Boundary shells and shadow-casting
/// stand-ins are left out.
class WorldIndex {
public:
    using Progress = std::function<void(int done, int total)>;

    /// Distance (metres) from which each level replaces the finer one. A
    /// group's finest level starts at 0 and its coarsest level has no end.
    static constexpr float kLodStart[] = {0.0F, 400.0F, 1500.0F, 4000.0F};

    /// Reads every render model header of `archive`, and places props where
    /// `placements` says (without it, props are left out). Returns nothing if
    /// `cancel` was set while it ran.
    static std::optional<WorldIndex> build(const ForzaZip& archive, const Progress& progress = {},
        const std::atomic<bool>* cancel = nullptr, const TrackPlacements* placements = nullptr);

    /// Saves to, or loads from, a cache file. `signature` identifies the
    /// archive (see archiveSignature()); a cache with another signature is
    /// rejected so a changed archive is re-indexed.
    bool save(const QString& path, const QString& signature, QString* error = nullptr) const;
    static std::optional<WorldIndex> load(const QString& path, const QString& signature, QString* error = nullptr);
    static QString archiveSignature(const QString& archivePath);

    const std::vector<WorldChunk>& chunks() const { return m_chunks; }
    /// Props (local-space models) with no placement, which are not drawn.
    int localModelCount() const { return m_localModels; }
    /// Chunks that are placements of props.
    int placedCount() const;
    /// Bounds of all chunks in the XZ plane (x = world X, y = world Z).
    QRectF footprint() const;

    /// Assigns distance bands: a placed prop's from the distances its
    /// placement gives, where it gives them; otherwise from the LOD levels
    /// present in the chunk's group. Exposed for tests; build() and load()
    /// call it.
    void assignBands();

private:
    struct LocalModel;
    void placeProps(const QHash<std::uint32_t, LocalModel>& models, const TrackPlacements& placements,
        const std::function<std::uint32_t(const QString&)>& groupOf);

    std::vector<WorldChunk> m_chunks;
    std::vector<QString> m_groupKeys;
    int m_localModels = 0;
};

} // namespace fh1
