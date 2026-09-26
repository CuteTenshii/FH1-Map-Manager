#include "XboxTexture.h"

#include <QPoint>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace fh1 {

namespace {

constexpr qsizetype kHeaderSize = 52;
constexpr qsizetype kFetchConstantOffset = 0x1C;
constexpr qsizetype kBixHeaderSize = 0x1C;
constexpr int kMaxDimension = 8192;

enum XenosFormat : quint32 {
    k8888 = 6,
    kDxt1 = 18,
    kDxt23 = 19,
    kDxt45 = 20,
    k8888As16 = 50,
    kDxt1As16 = 51,
    kDxt23As16 = 52,
    kDxt45As16 = 53,
};

using dxt::Rgba;

void setError(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
}

Rgba from565(std::uint16_t v)
{
    Rgba c;
    c.r = static_cast<std::uint8_t>(((v >> 11) & 31) * 255 / 31);
    c.g = static_cast<std::uint8_t>(((v >> 5) & 63) * 255 / 63);
    c.b = static_cast<std::uint8_t>((v & 31) * 255 / 31);
    return c;
}

std::uint16_t to565(float r, float g, float b)
{
    auto quantize = [](float v, int levels) {
        return static_cast<std::uint16_t>(
            std::clamp(std::lround(v / 255.0F * static_cast<float>(levels)), 0L, static_cast<long>(levels)));
    };
    return static_cast<std::uint16_t>((quantize(r, 31) << 11) | (quantize(g, 63) << 5) | quantize(b, 31));
}

Rgba mix(const Rgba& a, const Rgba& b, int wa, int wb)
{
    const int total = wa + wb;
    Rgba c;
    c.r = static_cast<std::uint8_t>((a.r * wa + b.r * wb) / total);
    c.g = static_cast<std::uint8_t>((a.g * wa + b.g * wb) / total);
    c.b = static_cast<std::uint8_t>((a.b * wa + b.b * wb) / total);
    return c;
}

std::uint16_t le16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

void putLe16(std::uint8_t* p, std::uint16_t v)
{
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

/// The four colours a DXT colour block can select. DXT1 blocks with
/// c0 <= c1 use three colours and transparent black; DXT3 and DXT5 always
/// use four.
std::array<Rgba, 4> colourPalette(std::uint16_t c0, std::uint16_t c1, bool dxt1)
{
    std::array<Rgba, 4> palette{from565(c0), from565(c1), {}, {}};
    if (!dxt1 || c0 > c1) {
        palette[2] = mix(palette[0], palette[1], 2, 1);
        palette[3] = mix(palette[0], palette[1], 1, 2);
    } else {
        palette[2] = mix(palette[0], palette[1], 1, 1);
        palette[3] = Rgba{0, 0, 0, 0};
    }
    return palette;
}

std::array<int, 8> alphaPalette(int a0, int a1)
{
    std::array<int, 8> palette{a0, a1, 0, 0, 0, 0, 0, 0};
    if (a0 > a1) {
        for (int i = 0; i < 6; ++i) {
            palette[static_cast<std::size_t>(i) + 2] = ((6 - i) * a0 + (1 + i) * a1) / 7;
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            palette[static_cast<std::size_t>(i) + 2] = ((4 - i) * a0 + (1 + i) * a1) / 5;
        }
        palette[6] = 0;
        palette[7] = 255;
    }
    return palette;
}

void decodeColour(const std::uint8_t* block, bool dxt1, Rgba* out)
{
    const auto palette = colourPalette(le16(block), le16(block + 2), dxt1);
    const std::uint32_t bits = static_cast<std::uint32_t>(block[4]) | (static_cast<std::uint32_t>(block[5]) << 8)
        | (static_cast<std::uint32_t>(block[6]) << 16) | (static_cast<std::uint32_t>(block[7]) << 24);
    for (int i = 0; i < 16; ++i) {
        const Rgba& colour = palette[(bits >> (2 * i)) & 3];
        out[i].r = colour.r;
        out[i].g = colour.g;
        out[i].b = colour.b;
        if (dxt1) {
            out[i].a = colour.a;
        }
    }
}

void decodeExplicitAlpha(const std::uint8_t* block, Rgba* out)
{
    for (int i = 0; i < 16; ++i) {
        const int nibble = (block[i / 2] >> ((i % 2) * 4)) & 15;
        out[i].a = static_cast<std::uint8_t>(nibble * 17);
    }
}

void decodeInterpolatedAlpha(const std::uint8_t* block, Rgba* out)
{
    const auto palette = alphaPalette(block[0], block[1]);
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
        bits |= static_cast<std::uint64_t>(block[2 + i]) << (8 * i);
    }
    for (int i = 0; i < 16; ++i) {
        out[i].a = static_cast<std::uint8_t>(palette[(bits >> (3 * i)) & 7]);
    }
}

int colourDistance(const Rgba& a, const Rgba& b)
{
    const int dr = a.r - b.r;
    const int dg = a.g - b.g;
    const int db = a.b - b.b;
    return dr * dr + dg * dg + db * db;
}

/// Two endpoint colours spanning the block's texels along their principal
/// axis, which suits the gradients DXT blocks store far better than the
/// bounding box corners do.
std::pair<std::array<float, 3>, std::array<float, 3>> colourEndpoints(const Rgba* texels, const bool* use)
{
    std::array<float, 3> mean{};
    int count = 0;
    for (int i = 0; i < 16; ++i) {
        if (use[i]) {
            mean[0] += texels[i].r;
            mean[1] += texels[i].g;
            mean[2] += texels[i].b;
            ++count;
        }
    }
    if (count == 0) {
        return {};
    }
    for (float& m : mean) {
        m /= static_cast<float>(count);
    }
    std::array<std::array<float, 3>, 3> covariance{};
    for (int i = 0; i < 16; ++i) {
        if (!use[i]) {
            continue;
        }
        const std::array<float, 3> d{texels[i].r - mean[0], texels[i].g - mean[1], texels[i].b - mean[2]};
        for (std::size_t r = 0; r < 3; ++r) {
            for (std::size_t c = 0; c < 3; ++c) {
                covariance[r][c] += d[r] * d[c];
            }
        }
    }
    // Power iteration toward the principal axis. It starts from the row of
    // the channel that varies most: a fixed start such as (1, 1, 1) can be
    // orthogonal to the true axis (red rising while green falls) and would
    // collapse every texel onto the mean.
    std::size_t widest = 0;
    for (std::size_t c = 1; c < 3; ++c) {
        if (covariance[c][c] > covariance[widest][widest]) {
            widest = c;
        }
    }
    std::array<float, 3> axis = covariance[widest];
    const float startLength = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (startLength < 1e-6F) {
        // Every texel has the same colour.
        const std::array<float, 3> colour{mean[0], mean[1], mean[2]};
        return {colour, colour};
    }
    for (float& a : axis) {
        a /= startLength;
    }
    for (int iteration = 0; iteration < 8; ++iteration) {
        std::array<float, 3> next{};
        for (std::size_t r = 0; r < 3; ++r) {
            next[r] = covariance[r][0] * axis[0] + covariance[r][1] * axis[1] + covariance[r][2] * axis[2];
        }
        const float length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
        if (length < 1e-6F) {
            break;
        }
        axis = {next[0] / length, next[1] / length, next[2] / length};
    }
    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    for (int i = 0; i < 16; ++i) {
        if (!use[i]) {
            continue;
        }
        const float t
            = (texels[i].r - mean[0]) * axis[0] + (texels[i].g - mean[1]) * axis[1] + (texels[i].b - mean[2]) * axis[2];
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    std::array<float, 3> a{};
    std::array<float, 3> b{};
    for (std::size_t c = 0; c < 3; ++c) {
        a[c] = std::clamp(mean[c] + axis[c] * hi, 0.0F, 255.0F);
        b[c] = std::clamp(mean[c] + axis[c] * lo, 0.0F, 255.0F);
    }
    return {a, b};
}

/// Writes the 8-byte colour half of a block. `transparent` selects DXT1's
/// three-colour mode, in which index 3 is transparent black.
void encodeColour(const Rgba* texels, bool dxt1, std::uint8_t* block)
{
    std::array<bool, 16> transparent{};
    std::array<bool, 16> opaque{};
    bool anyTransparent = false;
    for (int i = 0; i < 16; ++i) {
        transparent[static_cast<std::size_t>(i)] = dxt1 && texels[i].a < 128;
        opaque[static_cast<std::size_t>(i)] = !transparent[static_cast<std::size_t>(i)];
        anyTransparent = anyTransparent || transparent[static_cast<std::size_t>(i)];
    }
    const auto [a, b] = colourEndpoints(texels, opaque.data());
    std::uint16_t c0 = to565(a[0], a[1], a[2]);
    std::uint16_t c1 = to565(b[0], b[1], b[2]);
    if (anyTransparent) {
        if (c0 > c1) {
            std::swap(c0, c1);
        }
    } else if (c0 < c1) {
        std::swap(c0, c1);
    }
    const auto palette = colourPalette(c0, c1, dxt1);
    // With equal endpoints a DXT1 block is in three-colour mode; only the
    // first three entries are colours then.
    const int colours = (dxt1 && c0 <= c1) ? 3 : 4;
    std::uint32_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        std::uint32_t index = 3;
        if (!transparent[static_cast<std::size_t>(i)]) {
            int best = std::numeric_limits<int>::max();
            for (int p = 0; p < colours; ++p) {
                const int d = colourDistance(texels[i], palette[static_cast<std::size_t>(p)]);
                if (d < best) {
                    best = d;
                    index = static_cast<std::uint32_t>(p);
                }
            }
        }
        bits |= index << (2 * i);
    }
    putLe16(block, c0);
    putLe16(block + 2, c1);
    for (int i = 0; i < 4; ++i) {
        block[4 + i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF);
    }
}

void encodeAlpha(const Rgba* texels, std::uint8_t* block)
{
    int lo = 255;
    int hi = 0;
    for (int i = 0; i < 16; ++i) {
        lo = std::min<int>(lo, texels[i].a);
        hi = std::max<int>(hi, texels[i].a);
    }
    block[0] = static_cast<std::uint8_t>(hi);
    block[1] = static_cast<std::uint8_t>(lo);
    const auto palette = alphaPalette(hi, lo);
    std::uint64_t bits = 0;
    if (hi != lo) {
        for (int i = 0; i < 16; ++i) {
            std::uint64_t index = 0;
            int best = std::numeric_limits<int>::max();
            for (int p = 0; p < 8; ++p) {
                const int d = std::abs(texels[i].a - palette[static_cast<std::size_t>(p)]);
                if (d < best) {
                    best = d;
                    index = static_cast<std::uint64_t>(p);
                }
            }
            bits |= index << (3 * i);
        }
    }
    for (int i = 0; i < 6; ++i) {
        block[2 + i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF);
    }
}

std::optional<BixHeader> parseBixHeader(const QByteArray& data)
{
    if (data.size() < kBixHeaderSize || !data.startsWith("BIX1")) {
        return std::nullopt;
    }
    const char* p = data.constData();
    BixHeader header;
    header.width = static_cast<int>(qFromBigEndian<quint32>(p + 4));
    header.height = static_cast<int>(qFromBigEndian<quint32>(p + 8));
    header.mipCount = static_cast<int>(qFromBigEndian<quint32>(p + 12));
    header.formatWord = qFromBigEndian<quint32>(p + 16);
    header.totalSize = qFromBigEndian<quint32>(p + 20);
    header.topLevelSize = qFromBigEndian<quint32>(p + 24);
    return header;
}

} // namespace

int TextureSurface::blockBytes() const
{
    switch (format) {
    case Format::Dxt1:
        return 8;
    case Format::Dxt3:
    case Format::Dxt5:
        return 16;
    case Format::Rgba8:
        break;
    }
    return 4;
}

int TextureSurface::blocksWide() const
{
    return isCompressed() ? (width + 3) / 4 : width;
}

int TextureSurface::blocksHigh() const
{
    return isCompressed() ? (height + 3) / 4 : height;
}

namespace xenos {

quint32 tiledOffset(quint32 x, quint32 y, quint32 pitchBlocks, quint32 log2Bytes)
{
    const quint32 macroOuter = ((y >> 5) * (pitchBlocks >> 5)) << (log2Bytes + 7);
    const quint32 microOuter = ((y & 6) << 2) << log2Bytes;
    const quint32 outer = macroOuter + ((microOuter & ~0xFu) << 1) + (microOuter & 0xFu) + ((y & 8) << (3 + log2Bytes))
        + ((y & 1) << 4);
    const quint32 macroInner = (x >> 5) << (log2Bytes + 7);
    const quint32 microInner = (x & 7) << log2Bytes;
    const quint32 offset = outer + macroInner + ((microInner & ~0xFu) << 1) + (microInner & 0xFu);
    return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0u) << 2) + (offset & 0x3Fu) + ((y & 16) << 7)
        + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

QPoint packedBaseOffset(int width, int height, int blockSize)
{
    constexpr int kTailOffset = 16;
    constexpr int kMaxPackedLog2 = 4;
    const auto log2Ceil = [](int v) {
        int log2 = 0;
        while ((1 << log2) < v) {
            ++log2;
        }
        return log2;
    };
    const int log2Width = log2Ceil(width);
    const int log2Height = log2Ceil(height);
    if (std::min(log2Width, log2Height) > kMaxPackedLog2) {
        return {0, 0};
    }
    // The levels line up along the shorter axis, the first one 16 texels in.
    return log2Width > log2Height ? QPoint(0, kTailOffset / blockSize) : QPoint(kTailOffset / blockSize, 0);
}

} // namespace xenos

namespace dxt {

void decodeBlock(TextureSurface::Format format, const std::uint8_t* block, Rgba* texels)
{
    for (int i = 0; i < 16; ++i) {
        texels[i] = Rgba{};
    }
    switch (format) {
    case TextureSurface::Format::Dxt1:
        decodeColour(block, true, texels);
        break;
    case TextureSurface::Format::Dxt3:
        decodeExplicitAlpha(block, texels);
        decodeColour(block + 8, false, texels);
        break;
    case TextureSurface::Format::Dxt5:
        decodeInterpolatedAlpha(block, texels);
        decodeColour(block + 8, false, texels);
        break;
    case TextureSurface::Format::Rgba8:
        break;
    }
}

void encodeDxt1(const Rgba* texels, std::uint8_t* block)
{
    encodeColour(texels, true, block);
}

void encodeDxt5(const Rgba* texels, std::uint8_t* block)
{
    encodeAlpha(texels, block);
    encodeColour(texels, false, block + 8);
}

} // namespace dxt

std::optional<TextureSurface> untileXboxSurface(
    const QByteArray& tiled, quint32 formatWord, int width, int height, MipLayout layout, QString* error)
{
    const quint32 format = formatWord & 0x3F;
    const quint32 endian = (formatWord >> 6) & 3;
    if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension) {
        setError(error, QStringLiteral("invalid texture size %1x%2").arg(width).arg(height));
        return std::nullopt;
    }
    TextureSurface surface;
    surface.width = width;
    surface.height = height;
    switch (format) {
    case kDxt1:
    case kDxt1As16:
        surface.format = TextureSurface::Format::Dxt1;
        break;
    case kDxt23:
    case kDxt23As16:
        surface.format = TextureSurface::Format::Dxt3;
        break;
    case kDxt45:
    case kDxt45As16:
        surface.format = TextureSurface::Format::Dxt5;
        break;
    case k8888:
    case k8888As16:
        surface.format = TextureSurface::Format::Rgba8;
        break;
    default:
        setError(error, QStringLiteral("unsupported texture format %1").arg(format));
        return std::nullopt;
    }

    QByteArray swapped = tiled;
    auto* bytes = reinterpret_cast<std::uint8_t*>(swapped.data());
    const qsizetype size = swapped.size();
    if (endian == 1 || endian == 3) {
        for (qsizetype i = 0; i + 1 < size; i += 2) {
            std::swap(bytes[i], bytes[i + 1]);
        }
    }
    if (endian == 2 || endian == 3) {
        for (qsizetype i = 0; i + 3 < size; i += 4) {
            std::swap(bytes[i], bytes[i + 3]);
            std::swap(bytes[i + 1], bytes[i + 2]);
        }
    }

    const int blockBytes = surface.blockBytes();
    const int blocksWide = surface.blocksWide();
    const int blocksHigh = surface.blocksHigh();
    const auto pitch = static_cast<quint32>((blocksWide + 31) & ~31);
    const quint32 log2Bytes = blockBytes == 16 ? 4 : (blockBytes == 8 ? 3 : 2);
    const QPoint start = layout == MipLayout::Packed
        ? xenos::packedBaseOffset(width, height, surface.isCompressed() ? 4 : 1)
        : QPoint(0, 0);
    surface.data.resize(static_cast<qsizetype>(blocksWide) * blocksHigh * blockBytes);
    auto* out = reinterpret_cast<std::uint8_t*>(surface.data.data());
    for (int by = 0; by < blocksHigh; ++by) {
        for (int bx = 0; bx < blocksWide; ++bx) {
            const auto offset = static_cast<qsizetype>(xenos::tiledOffset(
                static_cast<quint32>(bx + start.x()), static_cast<quint32>(by + start.y()), pitch, log2Bytes));
            if (offset + blockBytes > size) {
                setError(error, QStringLiteral("texture data is truncated"));
                return std::nullopt;
            }
            std::uint8_t* target = out + (static_cast<qsizetype>(by) * blocksWide + bx) * blockBytes;
            const std::uint8_t* source = bytes + offset;
            if (surface.format == TextureSurface::Format::Rgba8) {
                // 8_8_8_8 texels are A, R, G, B in memory once swapped.
                target[0] = source[1];
                target[1] = source[2];
                target[2] = source[3];
                target[3] = source[0];
            } else {
                std::memcpy(target, source, static_cast<std::size_t>(blockBytes));
            }
        }
    }
    return surface;
}

QImage surfaceToImage(const TextureSurface& surface)
{
    if (surface.width <= 0 || surface.height <= 0) {
        return {};
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(surface.data.constData());
    const int blockBytes = surface.blockBytes();
    if (surface.data.size() < static_cast<qsizetype>(surface.blocksWide()) * surface.blocksHigh() * blockBytes) {
        return {};
    }
    QImage image(surface.width, surface.height, QImage::Format_ARGB32);
    if (!surface.isCompressed()) {
        for (int y = 0; y < surface.height; ++y) {
            auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
            const std::uint8_t* texel = bytes + static_cast<qsizetype>(y) * surface.width * 4;
            for (int x = 0; x < surface.width; ++x, texel += 4) {
                line[x] = qRgba(texel[0], texel[1], texel[2], texel[3]);
            }
        }
        return image;
    }
    std::array<Rgba, 16> texels{};
    for (int by = 0; by < surface.blocksHigh(); ++by) {
        for (int bx = 0; bx < surface.blocksWide(); ++bx) {
            dxt::decodeBlock(surface.format,
                bytes + (static_cast<qsizetype>(by) * surface.blocksWide() + bx) * blockBytes, texels.data());
            for (int i = 0; i < 16; ++i) {
                const int x = bx * 4 + i % 4;
                const int y = by * 4 + i / 4;
                if (x < surface.width && y < surface.height) {
                    const Rgba& c = texels[static_cast<std::size_t>(i)];
                    image.setPixel(x, y, qRgba(c.r, c.g, c.b, c.a));
                }
            }
        }
    }
    return image;
}

QImage decodeXboxTexture(const QByteArray& data, QString* error)
{
    if (data.size() < kHeaderSize) {
        setError(error, QStringLiteral("texture file is too small"));
        return {};
    }
    const char* fetch = data.constData() + kFetchConstantOffset;
    const auto word0 = qFromBigEndian<quint32>(fetch);
    const auto word1 = qFromBigEndian<quint32>(fetch + 4);
    const auto word2 = qFromBigEndian<quint32>(fetch + 8);
    if ((word0 >> 31) == 0) {
        // Every texture seen on the disc is tiled; the row alignment of linear
        // textures has not been confirmed, so they are rejected, not guessed.
        setError(error, QStringLiteral("linear (untiled) textures are not supported"));
        return {};
    }
    const int width = static_cast<int>((word2 & 0x1FFF) + 1);
    const int height = static_cast<int>(((word2 >> 13) & 0x1FFF) + 1);
    const std::optional<TextureSurface> surface
        = untileXboxSurface(data.mid(kHeaderSize), word1, width, height, MipLayout::Unpacked, error);
    return surface ? surfaceToImage(*surface) : QImage();
}

std::optional<BixHeader> readBixHeader(const QByteArray& data)
{
    return parseBixHeader(data);
}

std::optional<TextureSurface> readBixSurface(const QByteArray& header, const QByteArray& topLevel, QString* error)
{
    const std::optional<BixHeader> bix = parseBixHeader(header);
    if (!bix) {
        setError(error, QStringLiteral("not a BIX1 texture header"));
        return std::nullopt;
    }
    if (static_cast<quint32>(topLevel.size()) < bix->topLevelSize) {
        setError(error, QStringLiteral("texture top level is truncated"));
        return std::nullopt;
    }
    return untileXboxSurface(topLevel, bix->formatWord, bix->width, bix->height, MipLayout::Packed, error);
}

std::optional<TextureSurface> readCaffSurface(const QByteArray& data, QString* error)
{
    // The container holds a .data section (the texture description) and a
    // .gpu section (the texels) stored back to back after the header.
    constexpr qsizetype kHeaderSizeOffset = 0x48;
    constexpr qsizetype kDataSizeOffset = 0x60;
    constexpr qsizetype kGpuSizeOffset = 0x9C;
    constexpr qsizetype kFormatOffset = 0x18;
    constexpr qsizetype kSizeOffset = 0x24;
    if (data.size() < kGpuSizeOffset + 4 || !data.startsWith("CAFF")) {
        setError(error, QStringLiteral("not a CAFF container"));
        return std::nullopt;
    }
    const char* p = data.constData();
    const auto headerSize = static_cast<qsizetype>(qFromBigEndian<quint32>(p + kHeaderSizeOffset));
    const auto dataSize = static_cast<qsizetype>(qFromBigEndian<quint32>(p + kDataSizeOffset));
    const auto gpuSize = static_cast<qsizetype>(qFromBigEndian<quint32>(p + kGpuSizeOffset));
    if (dataSize < kSizeOffset + 4 || gpuSize <= 0 || headerSize + dataSize + gpuSize > data.size()) {
        setError(error, QStringLiteral("CAFF sections do not fit the file"));
        return std::nullopt;
    }
    const char* description = p + headerSize;
    if (std::memcmp(description, "texture", 8) != 0) {
        setError(error, QStringLiteral("CAFF container does not hold a texture"));
        return std::nullopt;
    }
    const auto formatWord = qFromBigEndian<quint32>(description + kFormatOffset);
    const int width = qFromBigEndian<quint16>(description + kSizeOffset);
    const int height = qFromBigEndian<quint16>(description + kSizeOffset + 2);
    return untileXboxSurface(
        data.mid(headerSize + dataSize, gpuSize), formatWord, width, height, MipLayout::Packed, error);
}

QImage decodeBixTexture(const QByteArray& header, const QByteArray& topLevel, QString* error)
{
    const std::optional<TextureSurface> surface = readBixSurface(header, topLevel, error);
    return surface ? surfaceToImage(*surface) : QImage();
}

QImage decodeCaffTexture(const QByteArray& data, QString* error)
{
    const std::optional<TextureSurface> surface = readCaffSurface(data, error);
    return surface ? surfaceToImage(*surface) : QImage();
}

} // namespace fh1
