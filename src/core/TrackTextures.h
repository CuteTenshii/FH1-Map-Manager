#pragma once

#include "ForzaZip.h"
#include "XboxTexture.h"

#include <QByteArray>
#include <QHash>
#include <QString>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace fh1 {

/// A texture ready for upload: every mip level from the full size down to
/// 1x1, all in one format. Level 0 keeps the game's own compressed blocks;
/// smaller levels are filtered from it and compressed again.
struct TextureMipChain {
    /// Where a texture was read from.
    enum class Origin {
        /// `_0x<ID>.bix` and `_0x<ID>_B.bix`.
        Bix,
        /// `_0x<ID>.bin`.
        Caff,
        /// A small copy in a `.bundle` pack.
        Bundle,
    };

    TextureSurface::Format format = TextureSurface::Format::Rgba8;
    int width = 0;
    int height = 0;
    std::vector<QByteArray> levels;
    Origin origin = Origin::Bix;
    /// The archive files the texture was read from, for display.
    QString files;

    int levelWidth(int level) const { return std::max(1, width >> level); }
    int levelHeight(int level) const { return std::max(1, height >> level); }
    /// The level as a surface, e.g. to decode it where the GPU cannot.
    TextureSurface level(int level) const;
    qsizetype byteSize() const;
};

/// Builds the mip chain of `top`. DXT3 textures are converted to DXT5, the
/// only other alpha format the chain uses.
TextureMipChain buildMipChain(const TextureSurface& top);

/// What the 3D view needs to know about one of the track's shaders.
struct ShaderLayout {
    /// Byte offset of the first texture coordinate pair in a vertex, or -1
    /// when the shader reads none.
    int texcoord0Offset = -1;
    /// Vertex size implied by the shader's inputs.
    int vertexBytes = 0;
};

/// Reads the vertex inputs of a compiled track shader (`.fxobj`).
///
/// After the first vertex shader's version string ("vs_3_0", compiler
/// version) the file holds a table: u32 0x290, u32 whose low 16 bits count
/// the entries, then one u32 per input and output. Inputs carry the D3D
/// declaration usage in bits 12-15, the usage index in bits 16-19 and the
/// register in bits 0-7; outputs use registers from 0x30 up. Every input is
/// four bytes in the vertex, after the 12-byte position, in table order.
/// That layout is inferred from matching the tables against vertex strides.
std::optional<ShaderLayout> readShaderLayout(const QByteArray& fxobj);

/// Which of the track's textures each render model uses, and how to read
/// them.
///
/// The track's PVS file (see PvsTables) lists the textures of each render
/// object; render object i is the file `<track>.<i>.rmb.bin`. A material's
/// texture slots index the object's texture list, and slot 0 holds the
/// diffuse texture in every shader the track uses (its sampler register 0).
///
/// A texture id names either `_0x<ID>.bix` (header and small mips) with
/// `_0x<ID>_B.bix` (top level), or `_0x<ID>.bin` (a CAFF container).
///
/// The `_0x1000xxxx.bundle` packs hold small copies (4x4 to 16x16 texels) of
/// nearly every texture, keyed by texture record number. For about a third
/// of the textures the models use, those copies are all there is; they are
/// mostly flat colours. A pack is u32 count, then per texture: u32 record
/// number, u32 width, u32 height, u32 mip count, u32 format word (as in the
/// GPU fetch constant), u32 0xFFFFFFFF, u32 size, and `size` bytes of tiled
/// data with packed mips. The same texture can appear in several packs.
class TrackTextures {
public:
    /// One texture in a bundle pack.
    struct BundleEntry {
        std::uint32_t record = 0;
        int width = 0;
        int height = 0;
        quint32 formatWord = 0;
        /// Where the tiled data starts in the pack, and its size.
        qsizetype offset = 0;
        qsizetype size = 0;
    };

    TrackTextures();
    ~TrackTextures();
    TrackTextures(TrackTextures&& other) noexcept;
    TrackTextures& operator=(TrackTextures&& other) noexcept;
    TrackTextures(const TrackTextures&) = delete;
    TrackTextures& operator=(const TrackTextures&) = delete;

    /// Reads the PVS file, every shader in `archive` and the index of every
    /// bundle pack. Returns nothing, with `error` set, if the PVS file does
    /// not have the expected layout.
    static std::optional<TrackTextures> load(const QByteArray& pvs, const ForzaZip& archive, QString* error = nullptr);

    /// Parses just the per-object texture ids of a PVS file.
    static std::optional<std::vector<std::vector<std::uint32_t>>> readObjectTextures(
        const QByteArray& pvs, QString* error = nullptr);

    /// Texture ids used by render object `object`, or nullptr if the PVS file
    /// has no such object.
    const std::vector<std::uint32_t>* objectTextures(std::uint32_t object) const;
    std::size_t objectCount() const { return m_objects.size(); }

    /// Layout of the shader at `path` as written in a render model, such as
    /// "shaders\track\h_diff_1.fx"; nullptr if the archive lacks it.
    const ShaderLayout* shader(const QString& path) const;
    int shaderCount() const { return static_cast<int>(m_shaders.size()); }

    /// The file name stem of texture `id`, e.g. "_0x00002B40".
    static QString textureName(std::uint32_t id);

    /// The number of the render object in an archive entry name such as
    /// "coloradoout.06454.rmb.bin", or nothing for other names.
    static std::optional<std::uint32_t> objectNumber(const QString& entryName);

    /// Lists the textures in a bundle pack; nothing if it is malformed.
    static std::optional<std::vector<BundleEntry>> readBundle(const QByteArray& pack, QString* error = nullptr);

    /// Texture records with a copy in a bundle pack.
    int bundledTextureCount() const { return static_cast<int>(m_bundled.size()); }

    /// Loads and decodes texture `id` from its full-size files, or from a
    /// bundle pack when the archive has none. `archive` must be the one
    /// load() indexed. Safe to call from several threads.
    std::optional<TextureMipChain> loadTexture(
        const ForzaZip& archive, std::uint32_t id, QString* error = nullptr) const;

private:
    struct BundleLocation {
        /// Index into ForzaZip::entries().
        std::size_t archiveEntry = 0;
        BundleEntry entry;
    };
    class PackCache;

    static QString shaderKey(const QString& path);
    std::optional<TextureMipChain> loadBundled(const ForzaZip& archive, std::uint32_t id, QString* error) const;

    std::vector<std::vector<std::uint32_t>> m_objects;
    /// Texture id to texture record number.
    QHash<std::uint32_t, std::uint32_t> m_records;
    QHash<QString, ShaderLayout> m_shaders;
    /// Texture record number to its copy in a bundle pack.
    QHash<std::uint32_t, BundleLocation> m_bundled;
    std::unique_ptr<PackCache> m_packs;
};

} // namespace fh1
