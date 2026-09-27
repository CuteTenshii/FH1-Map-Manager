#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>

#include <cstdint>
#include <optional>
#include <vector>

namespace fh1 {

/// A render model file (`*.rmb.bin` in a track's bin.zip).
///
/// Layout, big-endian, as far as it is decoded:
///
///     0x00  u32 6
///     0x04  f32 min x, y, z, pad       bounding box
///     0x14  f32 max x, y, z, pad
///     0x24  f32[16]                     transform (identity in every file seen)
///     0x74  u32 part count
///     0xB0  parts
///
///     part      u32 nameLength, name, u32 3, u32 vertexCount, u32 stride,
///               u32 0, vertices (each starts with f32 x, y, z), u32 1,
///               u32 materialCount, u32 1, materials; between two parts a
///               48-byte bounds block and u32 1
///     material  u32 2, u32 nameLength, name, u32 0, u32 ?, u32 table index,
///               u32 1, 48 bytes (f32[4] position offset, f32[4] position
///               scale, f32[4] texture coordinate offset u, v and scale u, v),
///               u32 4, u32 0, u32 indexCount, u32 indexWidth (2 or 4),
///               indices, u32 endTag, u32 1
///     tail      u32 1, 1, 1, u32 count, per table entry: u32 3, u32 shader,
///               u32 0, twice (u32 1, u32 n, n × f32[4] shader constants),
///               u32 1, u32 slotCount, slotCount × s32 texture slot (-2:
///               supplied by the engine); then u32 1, u32 shaderCount, shader
///               paths (u32 length, text), then compiled shader code
///
/// Texture coordinates are pairs of unsigned 16-bit fractions that the
/// vertex shader maps through the material's offset and scale.
///
/// Positions are stored with Z negated relative to the world (and to the
/// placements in CollObjs.xml), so this parser flips Z. Index buffers are
/// triangle strips with all-ones restarts; a buffer without restarts whose
/// length is a multiple of three is a triangle list. That last rule is
/// inferred from the data, not from a format definition.
struct RenderMesh {
    struct Material {
        QString name;
        /// Triangle list indices into the part's positions.
        std::vector<std::uint32_t> triangles;
        /// Index into RenderMesh::materialTable.
        std::uint32_t tableIndex = 0;
        /// Texture coordinate offset (x, y) and scale (z, w).
        QVector4D uvOffsetScale{0.0F, 0.0F, 1.0F, 1.0F};
    };
    struct Part {
        QString name;
        std::uint32_t stride = 0;
        std::vector<QVector3D> positions;
        /// The vertices as stored, `stride` bytes each.
        QByteArray vertexData;
        std::vector<Material> materials;
    };
    /// Shader and textures of a material, from the table after the parts.
    struct MaterialInfo {
        /// Index into RenderMesh::shaders.
        std::uint32_t shader = 0;
        /// Per texture slot, an index into the model's texture list or -1
        /// for textures the engine supplies (lightmaps, shadows). Slot k
        /// feeds the shader's sampler register k.
        std::vector<int> textureSlots;
        /// The second group of shader constants (the first is empty in
        /// every track model). For the blended ground shaders the first two
        /// hold the texture scales of their layers: Blend_A and Blend_B in
        /// the first (x, y and z, w), Blend_C and the splat map in the
        /// second. Inferred from the data.
        std::vector<QVector4D> constants;
    };

    /// World-space bounding box (Z already flipped).
    QVector3D boundsMin;
    QVector3D boundsMax;
    std::vector<Part> parts;
    /// Empty when the file's tail does not have the expected layout; the
    /// geometry is still usable then.
    std::vector<MaterialInfo> materialTable;
    /// Shader paths, e.g. "shaders\track\h_diff_1.fx".
    QStringList shaders;

    std::size_t triangleCount() const;

    /// Texture coordinates of vertex `vertex` of `part`, read from the pair
    /// at byte `offset` and mapped through `material`'s offset and scale.
    /// Returns (0, 0) when the pair lies outside the vertex.
    static QVector2D texcoord(const Part& part, const Material& material, std::uint32_t vertex, int offset);
    /// The texture coordinate pair at byte `offset` of vertex `vertex` as
    /// stored, as 16-bit fractions of 1; (0, 0) outside the vertex.
    static QVector2D rawTexcoord(const Part& part, std::uint32_t vertex, int offset);
    /// The four bytes at byte `offset` of vertex `vertex` (a vertex colour),
    /// or 0 outside the vertex.
    static std::uint32_t colour(const Part& part, std::uint32_t vertex, int offset);
};

/// What the first bytes of a render model say about it, enough to index a
/// world without decoding whole files.
struct RenderMeshHeader {
    QVector3D boundsMin;
    QVector3D boundsMax;
    std::uint32_t partCount = 0;
    /// Name of the first part, e.g. "Redstone_Area02_Road_LOD02_01".
    QString firstPartName;
};

namespace rendermesh {

/// Bytes from the start of a file that readHeader() needs.
constexpr qsizetype kHeaderBytes = 0x200;

/// Parses a whole file; throws LoadError on any structural mismatch.
RenderMesh parse(const QByteArray& data, const QString& source);

/// Parses the header and first part name from the start of a file; returns
/// nothing if the data is not a render model.
std::optional<RenderMeshHeader> readHeader(const QByteArray& prefix);

/// Level of detail from a part name ("..._LOD01_..." gives 1), or -1 when the
/// name carries none (e.g. "NOLOD" meshes).
int lodLevel(const QString& partName);

/// The part name without its LOD token and trailing chunk numbers:
/// "Redstone_Area02_Road_LOD01_02" and "Redstone_Area02_Road_LOD02_11" both
/// give "redstone_area02_road". Each LOD level of such a group covers the same
/// area, though its chunks are cut differently.
QString lodGroupKey(const QString& partName);

} // namespace rendermesh
} // namespace fh1
