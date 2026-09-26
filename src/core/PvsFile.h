#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <optional>
#include <vector>

namespace fh1 {

/// The tables of a track's PVS file (`Ribbon_00/<Track>_00.pvs`, magic
/// "FPVS", big-endian) that the viewer uses. After its zone table the file
/// holds:
///
///     u32 N, N texture records of 28 bytes:
///         u32 texture id, u32 record number, f32 1, f32 1, u32 0, u32 0,
///         u32 flags
///     u32 count, shader paths (u32 length, text)
///     u32 count, 18-byte draw records, one per placed model: u16 render
///         object, then eight u16 not needed here
///     u32 count, one record per render object:
///         u32 n, n texture record numbers, u32 m, m shader numbers,
///         15 floats (orientation and bounding box)
///
/// Render object i is the file `<track>.<i>.rmb.bin`. A draw record is one
/// placement of a render object.
struct PvsTables {
    /// Texture id of each texture record.
    std::vector<std::uint32_t> textureIds;
    /// Texture ids used by each render object.
    std::vector<std::vector<std::uint32_t>> objectTextures;
    /// Render object of each draw record.
    std::vector<std::uint16_t> drawObjects;
};

/// Parses a PVS file. Returns nothing, with `error` set, if it does not have
/// the expected layout.
std::optional<PvsTables> readPvs(const QByteArray& pvs, QString* error = nullptr);

} // namespace fh1
