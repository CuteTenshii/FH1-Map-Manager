#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

#include <cstdint>
#include <optional>

namespace fh1 {

/// One mip level of a texture in the layout PC graphics APIs expect: blocks
/// in row-major order with little-endian fields, no tiling.
struct TextureSurface {
    enum class Format {
        /// 8-byte blocks; colour with optional 1-bit alpha.
        Dxt1,
        /// 16-byte blocks; explicit 4-bit alpha.
        Dxt3,
        /// 16-byte blocks; interpolated alpha.
        Dxt5,
        /// 4 bytes per texel: R, G, B, A. Also what formats without a PC
        /// counterpart here (DXN, DXT3A, DXT5A) are decoded to.
        Rgba8,
    };

    Format format = Format::Rgba8;
    int width = 0;
    int height = 0;
    QByteArray data;

    bool isCompressed() const { return format != Format::Rgba8; }
    /// Bytes per 4x4 block for compressed formats, per texel for Rgba8.
    int blockBytes() const;
    int blocksWide() const;
    int blocksHigh() const;
};

/// Where a texture's top level sits in its tiled data.
enum class MipLayout {
    /// At the origin.
    Unpacked,
    /// In the packed mip tail when the texture is 16 texels or less on its
    /// shorter side; at the origin otherwise.
    Packed,
};

/// Untiles the top mip level of Xbox 360 GPU texture data with the xds
/// library. `formatWord` is the second dword of the GPU fetch constant:
/// format in bits 0-5, byte order in bits 6-7. DXT1, DXT2/3 and DXT4/5 keep
/// their blocks; 8_8_8_8, DXN, DXT3A and DXT5A are decoded to Rgba8. Returns
/// nothing and sets `error` for other formats or truncated data.
std::optional<TextureSurface> untileXboxSurface(const QByteArray& tiled, quint32 formatWord, int width, int height,
    MipLayout layout = MipLayout::Unpacked, QString* error = nullptr);

/// Decodes a surface to an image (ARGB32).
QImage surfaceToImage(const TextureSurface& surface);

/// Decodes the top mip level of an Xbox 360 texture file (.xds) with the xds
/// library, with the swizzle of its fetch constant applied. Returns a null
/// image and sets `error` on failure.
QImage decodeXboxTexture(const QByteArray& data, QString* error = nullptr);

/// Header of a track texture's `_0x<ID>.bix` file ("BIX1", big-endian).
/// The file holds the header and the smaller mip levels; the top level is
/// stored on its own in `_0x<ID>_B.bix` so it can be streamed separately.
struct BixHeader {
    int width = 0;
    int height = 0;
    int mipCount = 0;
    /// Second dword of the GPU fetch constant (format and byte order).
    quint32 formatWord = 0;
    quint32 totalSize = 0;
    quint32 topLevelSize = 0;
};

std::optional<BixHeader> readBixHeader(const QByteArray& data);

/// The top level of a track texture, from its `.bix` header file and its
/// `_B.bix` top-level file.
std::optional<TextureSurface> readBixSurface(
    const QByteArray& header, const QByteArray& topLevel, QString* error = nullptr);

/// The top level of a small track texture stored whole in a CAFF container
/// (`_0x<ID>.bin`).
std::optional<TextureSurface> readCaffSurface(const QByteArray& data, QString* error = nullptr);

/// Image versions of readBixSurface() and readCaffSurface(); null on failure.
QImage decodeBixTexture(const QByteArray& header, const QByteArray& topLevel, QString* error = nullptr);
QImage decodeCaffTexture(const QByteArray& data, QString* error = nullptr);

namespace dxt {

/// One texel, as encoded to a block.
struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

/// Encodes 16 texels (row-major) as a DXT1 block. Texels with alpha below 128
/// become transparent, using DXT1's three-colour mode.
void encodeDxt1(const Rgba* texels, std::uint8_t* block);

/// Encodes 16 texels (row-major) as a DXT5 block.
void encodeDxt5(const Rgba* texels, std::uint8_t* block);

} // namespace dxt
} // namespace fh1
