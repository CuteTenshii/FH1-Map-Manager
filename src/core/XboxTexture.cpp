#include "XboxTexture.h"

#include <QtEndian>

#include <xds/xds.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace fh1 {

namespace {

constexpr qsizetype kBixHeaderSize = 0x1C;
constexpr int kMaxDimension = 8192;
/// Tiled surfaces are whole 32x32-block tiles wide.
constexpr int kTileBlocks = 32;

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

std::span<const std::uint8_t> bytesOf(const QByteArray& data)
{
    return {reinterpret_cast<const std::uint8_t*>(data.constData()), static_cast<std::size_t>(data.size())};
}

bool isDxt1(std::uint32_t format)
{
    return format == static_cast<std::uint32_t>(xds::Format::kDxt1)
        || format == static_cast<std::uint32_t>(xds::Format::kDxt1_As16_16_16_16);
}

bool isDxt3(std::uint32_t format)
{
    return format == static_cast<std::uint32_t>(xds::Format::kDxt2_3)
        || format == static_cast<std::uint32_t>(xds::Format::kDxt2_3_As16_16_16_16);
}

bool isDxt5(std::uint32_t format)
{
    return format == static_cast<std::uint32_t>(xds::Format::kDxt4_5)
        || format == static_cast<std::uint32_t>(xds::Format::kDxt4_5_As16_16_16_16);
}

bool is8888(std::uint32_t format)
{
    return format == static_cast<std::uint32_t>(xds::Format::k8_8_8_8)
        || format == static_cast<std::uint32_t>(xds::Format::k8_8_8_8_As16_16_16_16);
}

/// The top level of `texture`: DXT blocks as they are, anything else (8_8_8_8,
/// DXN, the single-channel formats) decoded to RGBA with the texture's
/// swizzle.
TextureSurface topLevelSurface(const xds::Texture& texture)
{
    const std::uint32_t format = texture.info().format;
    TextureSurface surface;
    surface.width = static_cast<int>(texture.info().width);
    surface.height = static_cast<int>(texture.info().height);
    if (isDxt1(format) || isDxt3(format) || isDxt5(format)) {
        surface.format = isDxt1(format) ? TextureSurface::Format::Dxt1
            : isDxt3(format)            ? TextureSurface::Format::Dxt3
                                        : TextureSurface::Format::Dxt5;
        const xds::Surface blocks = texture.extractLevel(0);
        surface.data
            = QByteArray(reinterpret_cast<const char*>(blocks.data.data()), static_cast<qsizetype>(blocks.data.size()));
        return surface;
    }
    surface.format = TextureSurface::Format::Rgba8;
    const xds::Image image = texture.decodeLevel(0);
    surface.data
        = QByteArray(reinterpret_cast<const char*>(image.rgba.data()), static_cast<qsizetype>(image.rgba.size()));
    return surface;
}

QImage imageOf(const xds::Image& decoded)
{
    const QImage view(decoded.rgba.data(), static_cast<int>(decoded.width), static_cast<int>(decoded.height),
        static_cast<qsizetype>(decoded.width) * 4, QImage::Format_RGBA8888);
    // The view borrows the decoded pixels; the conversion copies them.
    return view.convertToFormat(QImage::Format_ARGB32);
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

namespace dxt {

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
    if (width <= 0 || height <= 0 || width > kMaxDimension || height > kMaxDimension) {
        setError(error, QStringLiteral("invalid texture size %1x%2").arg(width).arg(height));
        return std::nullopt;
    }
    xds::TextureInfo info;
    info.format = formatWord & 0x3F;
    if (!xds::isFormatSupported(info.format)) {
        setError(error,
            QStringLiteral("unsupported texture format %1 (%2)")
                .arg(info.format)
                .arg(QString::fromLatin1(xds::formatName(info.format))));
        return std::nullopt;
    }
    info.endian = static_cast<xds::Endian>((formatWord >> 6) & 3);
    info.tiled = true;
    info.packedMips = layout == MipLayout::Packed;
    info.width = static_cast<std::uint32_t>(width);
    info.height = static_cast<std::uint32_t>(height);
    const std::uint32_t edge = xds::blockEdge(info.format);
    const std::uint32_t blocksWide = (info.width + edge - 1) / edge;
    info.pitch = (blocksWide + kTileBlocks - 1) / kTileBlocks * kTileBlocks * edge;
    if (is8888(info.format)) {
        // The containers carry no swizzle; their 8_8_8_8 texels are A, R, G,
        // B in memory once swapped.
        info.swizzle = {xds::Swizzle::Y, xds::Swizzle::Z, xds::Swizzle::W, xds::Swizzle::X};
    }
    try {
        return topLevelSurface(xds::Texture::fromData(info, bytesOf(tiled)));
    } catch (const xds::Error& e) {
        setError(error, QString::fromUtf8(e.what()));
        return std::nullopt;
    }
}

QImage surfaceToImage(const TextureSurface& surface)
{
    if (surface.width <= 0 || surface.height <= 0
        || surface.data.size()
            < static_cast<qsizetype>(surface.blocksWide()) * surface.blocksHigh() * surface.blockBytes()) {
        return {};
    }
    if (!surface.isCompressed()) {
        const QImage view(reinterpret_cast<const uchar*>(surface.data.constData()), surface.width, surface.height,
            static_cast<qsizetype>(surface.width) * 4, QImage::Format_RGBA8888);
        return view.convertToFormat(QImage::Format_ARGB32);
    }
    // The blocks as a linear texture in PC byte order, for xds to decode.
    xds::TextureInfo info;
    info.format = static_cast<std::uint32_t>(surface.format == TextureSurface::Format::Dxt1 ? xds::Format::kDxt1
            : surface.format == TextureSurface::Format::Dxt3                                ? xds::Format::kDxt2_3
                                                                                            : xds::Format::kDxt4_5);
    info.endian = xds::Endian::None;
    info.tiled = false;
    info.width = static_cast<std::uint32_t>(surface.width);
    info.height = static_cast<std::uint32_t>(surface.height);
    info.pitch = static_cast<std::uint32_t>(surface.blocksWide()) * 4;
    try {
        return imageOf(xds::Texture::fromData(info, bytesOf(surface.data)).decodeLevel(0));
    } catch (const xds::Error&) {
        return {};
    }
}

QImage decodeXboxTexture(const QByteArray& data, QString* error)
{
    try {
        return imageOf(xds::Texture::parse(bytesOf(data)).decodeLevel(0));
    } catch (const xds::Error& e) {
        setError(error, QString::fromUtf8(e.what()));
        return {};
    }
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
