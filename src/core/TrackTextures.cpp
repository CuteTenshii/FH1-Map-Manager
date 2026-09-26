#include "TrackTextures.h"

#include "BigEndianCursor.h"
#include "PvsFile.h"

#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <QtConcurrent/QtConcurrentMap>
#include <QtEndian>

#include <array>
#include <cstring>
#include <list>

namespace fh1 {

namespace {

constexpr std::uint32_t kLayoutMarker = 0x290;
constexpr int kPositionBytes = 12;
constexpr int kInputBytes = 4;
constexpr int kFirstOutputRegister = 0x30;
constexpr int kUsageTexcoord = 5;
constexpr qsizetype kBundleHeaderBytes = 28;
constexpr std::uint32_t kBundleSeparator = 0xFFFFFFFF;
constexpr int kMaxBundleDimension = 4096;
/// Bundle packs kept decompressed; loading a view's missing textures hits
/// the same few packs many times.
constexpr std::size_t kCachedPacks = 8;

using dxt::Rgba;

void setError(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
}

std::uint32_t u32At(const QByteArray& data, qsizetype offset)
{
    return qFromBigEndian<std::uint32_t>(data.constData() + offset);
}

std::vector<Rgba> decodeTexels(const TextureSurface& surface)
{
    std::vector<Rgba> texels(static_cast<std::size_t>(surface.width) * static_cast<std::size_t>(surface.height));
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(surface.data.constData());
    if (!surface.isCompressed()) {
        for (std::size_t i = 0; i < texels.size(); ++i) {
            texels[i] = Rgba{bytes[i * 4], bytes[i * 4 + 1], bytes[i * 4 + 2], bytes[i * 4 + 3]};
        }
        return texels;
    }
    std::array<Rgba, 16> block{};
    for (int by = 0; by < surface.blocksHigh(); ++by) {
        for (int bx = 0; bx < surface.blocksWide(); ++bx) {
            dxt::decodeBlock(surface.format,
                bytes + (static_cast<qsizetype>(by) * surface.blocksWide() + bx) * surface.blockBytes(), block.data());
            for (int i = 0; i < 16; ++i) {
                const int x = bx * 4 + i % 4;
                const int y = by * 4 + i / 4;
                if (x < surface.width && y < surface.height) {
                    texels[static_cast<std::size_t>(y) * static_cast<std::size_t>(surface.width)
                        + static_cast<std::size_t>(x)] = block[static_cast<std::size_t>(i)];
                }
            }
        }
    }
    return texels;
}

/// Halves an image with a box filter. Colours are weighted by alpha so that
/// transparent texels (black in DXT1) do not darken the edges of cut-outs.
std::vector<Rgba> downsample(const std::vector<Rgba>& texels, int width, int height, int newWidth, int newHeight)
{
    std::vector<Rgba> out(static_cast<std::size_t>(newWidth) * static_cast<std::size_t>(newHeight));
    for (int y = 0; y < newHeight; ++y) {
        for (int x = 0; x < newWidth; ++x) {
            int r = 0;
            int g = 0;
            int b = 0;
            int a = 0;
            int samples = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = std::min(x * 2 + dx, width - 1);
                    const int sy = std::min(y * 2 + dy, height - 1);
                    const Rgba& t = texels[static_cast<std::size_t>(sy) * static_cast<std::size_t>(width)
                        + static_cast<std::size_t>(sx)];
                    r += t.r * t.a;
                    g += t.g * t.a;
                    b += t.b * t.a;
                    a += t.a;
                    ++samples;
                }
            }
            Rgba& o
                = out[static_cast<std::size_t>(y) * static_cast<std::size_t>(newWidth) + static_cast<std::size_t>(x)];
            if (a > 0) {
                o.r = static_cast<std::uint8_t>(r / a);
                o.g = static_cast<std::uint8_t>(g / a);
                o.b = static_cast<std::uint8_t>(b / a);
            }
            o.a = static_cast<std::uint8_t>(a / samples);
        }
    }
    return out;
}

QByteArray encodeLevel(const std::vector<Rgba>& texels, int width, int height, TextureSurface::Format format)
{
    if (format == TextureSurface::Format::Rgba8) {
        QByteArray out(static_cast<qsizetype>(texels.size()) * 4, Qt::Uninitialized);
        std::memcpy(out.data(), texels.data(), texels.size() * sizeof(Rgba));
        return out;
    }
    const int blocksWide = (width + 3) / 4;
    const int blocksHigh = (height + 3) / 4;
    const int blockBytes = format == TextureSurface::Format::Dxt1 ? 8 : 16;
    QByteArray out(static_cast<qsizetype>(blocksWide) * blocksHigh * blockBytes, Qt::Uninitialized);
    auto* bytes = reinterpret_cast<std::uint8_t*>(out.data());
    std::array<Rgba, 16> block{};
    for (int by = 0; by < blocksHigh; ++by) {
        for (int bx = 0; bx < blocksWide; ++bx) {
            for (int i = 0; i < 16; ++i) {
                // Levels smaller than a block repeat their edge texels.
                const int x = std::min(bx * 4 + i % 4, width - 1);
                const int y = std::min(by * 4 + i / 4, height - 1);
                block[static_cast<std::size_t>(i)]
                    = texels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                        + static_cast<std::size_t>(x)];
            }
            std::uint8_t* target = bytes + (static_cast<qsizetype>(by) * blocksWide + bx) * blockBytes;
            if (format == TextureSurface::Format::Dxt1) {
                dxt::encodeDxt1(block.data(), target);
            } else {
                dxt::encodeDxt5(block.data(), target);
            }
        }
    }
    return out;
}

} // namespace

TextureSurface TextureMipChain::level(int level) const
{
    TextureSurface surface;
    surface.format = format;
    surface.width = levelWidth(level);
    surface.height = levelHeight(level);
    if (level >= 0 && static_cast<std::size_t>(level) < levels.size()) {
        surface.data = levels[static_cast<std::size_t>(level)];
    }
    return surface;
}

qsizetype TextureMipChain::byteSize() const
{
    qsizetype total = 0;
    for (const QByteArray& level : levels) {
        total += level.size();
    }
    return total;
}

TextureMipChain buildMipChain(const TextureSurface& top)
{
    TextureMipChain chain;
    chain.width = top.width;
    chain.height = top.height;
    chain.format = top.format == TextureSurface::Format::Dxt3 ? TextureSurface::Format::Dxt5 : top.format;
    std::vector<Rgba> texels = decodeTexels(top);
    chain.levels.push_back(
        top.format == chain.format ? top.data : encodeLevel(texels, top.width, top.height, chain.format));
    int width = top.width;
    int height = top.height;
    while (width > 1 || height > 1) {
        const int newWidth = std::max(1, width / 2);
        const int newHeight = std::max(1, height / 2);
        texels = downsample(texels, width, height, newWidth, newHeight);
        width = newWidth;
        height = newHeight;
        chain.levels.push_back(encodeLevel(texels, width, height, chain.format));
    }
    return chain;
}

std::optional<ShaderLayout> readShaderLayout(const QByteArray& fxobj)
{
    static const QByteArray kVersion("vs_3_0", 7);
    const qsizetype version = fxobj.indexOf(kVersion);
    if (version < 0) {
        return std::nullopt;
    }
    const QByteArray marker = QByteArray::fromHex("00000290");
    const qsizetype table = fxobj.indexOf(marker, version);
    constexpr qsizetype kMarkerSearch = 256;
    if (table < 0 || table - version > kMarkerSearch || table + 8 > fxobj.size()) {
        return std::nullopt;
    }
    if (u32At(fxobj, table) != kLayoutMarker) {
        return std::nullopt;
    }
    const std::uint32_t count = u32At(fxobj, table + 4) & 0xFFFF;
    ShaderLayout layout;
    int inputs = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const qsizetype at = table + 8 + static_cast<qsizetype>(i) * 4;
        if (at + 4 > fxobj.size()) {
            return std::nullopt;
        }
        const std::uint32_t entry = u32At(fxobj, at);
        const auto reg = static_cast<int>(entry & 0xFF);
        if (reg >= kFirstOutputRegister) {
            break;
        }
        const auto usage = static_cast<int>((entry >> 12) & 0xF);
        const auto usageIndex = static_cast<int>((entry >> 16) & 0xF);
        if (usage == kUsageTexcoord && usageIndex == 0 && layout.texcoord0Offset < 0) {
            layout.texcoord0Offset = kPositionBytes + inputs * kInputBytes;
        }
        ++inputs;
    }
    layout.vertexBytes = kPositionBytes + inputs * kInputBytes;
    return layout;
}

/// The most recently read bundle packs, shared by the loading threads.
class TrackTextures::PackCache {
public:
    QByteArray get(const ForzaZip& archive, std::size_t entry, QString* error)
    {
        {
            const QMutexLocker lock(&m_mutex);
            for (auto it = m_packs.begin(); it != m_packs.end(); ++it) {
                if (it->first == entry) {
                    m_packs.splice(m_packs.begin(), m_packs, it);
                    return m_packs.front().second;
                }
            }
        }
        // Decompressed outside the lock so other threads are not held up; two
        // threads may occasionally read the same pack.
        QByteArray data = archive.read(archive.entries()[entry], error);
        if (data.isNull()) {
            return data;
        }
        const QMutexLocker lock(&m_mutex);
        m_packs.emplace_front(entry, data);
        if (m_packs.size() > kCachedPacks) {
            m_packs.pop_back();
        }
        return data;
    }

private:
    QMutex m_mutex;
    std::list<std::pair<std::size_t, QByteArray>> m_packs;
};

TrackTextures::TrackTextures()
    : m_packs(std::make_unique<PackCache>())
{
}

TrackTextures::~TrackTextures() = default;
TrackTextures::TrackTextures(TrackTextures&& other) noexcept = default;
TrackTextures& TrackTextures::operator=(TrackTextures&& other) noexcept = default;

std::optional<std::vector<std::vector<std::uint32_t>>> TrackTextures::readObjectTextures(
    const QByteArray& pvs, QString* error)
{
    std::optional<PvsTables> tables = readPvs(pvs, error);
    if (!tables) {
        return std::nullopt;
    }
    return std::move(tables->objectTextures);
}

std::optional<std::vector<TrackTextures::BundleEntry>> TrackTextures::readBundle(const QByteArray& pack, QString* error)
{
    BigEndianCursor c(pack);
    const std::uint32_t count = c.u32();
    if (!c.ok() || count > static_cast<std::uint32_t>(pack.size() / kBundleHeaderBytes)) {
        setError(error, QStringLiteral("not a bundle pack"));
        return std::nullopt;
    }
    std::vector<BundleEntry> entries;
    entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        BundleEntry entry;
        entry.record = c.u32();
        entry.width = static_cast<int>(c.u32());
        entry.height = static_cast<int>(c.u32());
        c.u32(); // mip count; the packed levels are rebuilt from the top one
        entry.formatWord = c.u32();
        const std::uint32_t separator = c.u32();
        const std::uint32_t size = c.u32();
        entry.offset = c.pos();
        entry.size = static_cast<qsizetype>(size);
        c.skip(entry.size);
        if (!c.ok() || separator != kBundleSeparator || entry.width <= 0 || entry.height <= 0
            || entry.width > kMaxBundleDimension || entry.height > kMaxBundleDimension) {
            setError(error, QStringLiteral("bundle entry %1 is malformed").arg(i));
            return std::nullopt;
        }
        entries.push_back(entry);
    }
    return entries;
}

std::optional<TrackTextures> TrackTextures::load(const QByteArray& pvs, const ForzaZip& archive, QString* error)
{
    std::optional<PvsTables> tables = readPvs(pvs, error);
    if (!tables) {
        return std::nullopt;
    }
    TrackTextures textures;
    textures.m_objects = std::move(tables->objectTextures);
    for (std::size_t record = 0; record < tables->textureIds.size(); ++record) {
        textures.m_records.insert(tables->textureIds[record], static_cast<std::uint32_t>(record));
    }
    for (const ZipEntry& entry : archive.entries()) {
        if (!entry.name.endsWith(QLatin1String(".fxobj"), Qt::CaseInsensitive)) {
            continue;
        }
        const QString key = shaderKey(entry.name);
        if (textures.m_shaders.contains(key)) {
            continue;
        }
        const QByteArray data = archive.read(entry);
        if (const std::optional<ShaderLayout> layout = readShaderLayout(data)) {
            textures.m_shaders.insert(key, *layout);
        }
    }

    // Each pack is stored several times; one copy of each is indexed.
    std::vector<std::size_t> packs;
    QSet<QString> packNames;
    const auto& entries = archive.entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name.endsWith(QLatin1String(".bundle"), Qt::CaseInsensitive)) {
            const QString name = ForzaZip::normalizeName(entries[i].name);
            if (!packNames.contains(name)) {
                packNames.insert(name);
                packs.push_back(i);
            }
        }
    }
    const std::vector<std::vector<BundleEntry>> packEntries
        = QtConcurrent::blockingMapped<std::vector<std::vector<BundleEntry>>>(packs, [&archive](std::size_t entry) {
              const QByteArray data = archive.read(archive.entries()[entry]);
              std::optional<std::vector<BundleEntry>> parsed = readBundle(data);
              return parsed ? std::move(*parsed) : std::vector<BundleEntry>();
          });
    for (std::size_t p = 0; p < packs.size(); ++p) {
        for (const BundleEntry& entry : packEntries[p]) {
            if (!textures.m_bundled.contains(entry.record)) {
                textures.m_bundled.insert(entry.record, BundleLocation{packs[p], entry});
            }
        }
    }
    return textures;
}

const std::vector<std::uint32_t>* TrackTextures::objectTextures(std::uint32_t object) const
{
    return object < m_objects.size() ? &m_objects[object] : nullptr;
}

const ShaderLayout* TrackTextures::shader(const QString& path) const
{
    const auto it = m_shaders.constFind(shaderKey(path));
    return it == m_shaders.constEnd() ? nullptr : &it.value();
}

QString TrackTextures::shaderKey(const QString& path)
{
    QString name = path.section(QRegularExpression(QStringLiteral("[/\\\\]")), -1).toLower();
    const qsizetype dot = name.indexOf(QLatin1Char('.'));
    if (dot >= 0) {
        name.truncate(dot);
    }
    return name;
}

std::optional<std::uint32_t> TrackTextures::objectNumber(const QString& entryName)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\.(\\d+)\\.rmb\\.bin$"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(entryName);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    bool ok = false;
    const std::uint32_t number = match.captured(1).toUInt(&ok);
    return ok ? std::optional<std::uint32_t>(number) : std::nullopt;
}

std::optional<TextureMipChain> TrackTextures::loadTexture(
    const ForzaZip& archive, std::uint32_t id, QString* error) const
{
    const QString stem = textureName(id);
    std::optional<TextureSurface> surface;
    TextureMipChain::Origin origin = TextureMipChain::Origin::Bix;
    QString files;
    if (const ZipEntry* header = archive.find(stem + QLatin1String(".bix"))) {
        const ZipEntry* top = archive.find(stem + QLatin1String("_B.bix"));
        if (top == nullptr) {
            setError(error, QStringLiteral("%1_B.bix is missing").arg(stem));
            return std::nullopt;
        }
        QString readError;
        const QByteArray headerData = archive.read(*header, &readError);
        const QByteArray topData = headerData.isNull() ? QByteArray() : archive.read(*top, &readError);
        if (topData.isNull()) {
            setError(error, readError);
            return std::nullopt;
        }
        surface = readBixSurface(headerData, topData, error);
        files = QStringLiteral("%1, %2").arg(header->name, top->name);
    } else if (const ZipEntry* container = archive.find(stem + QLatin1String(".bin"))) {
        QString readError;
        const QByteArray data = archive.read(*container, &readError);
        if (data.isNull()) {
            setError(error, readError);
            return std::nullopt;
        }
        surface = readCaffSurface(data, error);
        origin = TextureMipChain::Origin::Caff;
        files = container->name;
    } else {
        return loadBundled(archive, id, error);
    }
    if (!surface) {
        return std::nullopt;
    }
    TextureMipChain chain = buildMipChain(*surface);
    chain.origin = origin;
    chain.files = files;
    return chain;
}

QString TrackTextures::textureName(std::uint32_t id)
{
    return QStringLiteral("_0x%1").arg(QString::number(id, 16).toUpper().rightJustified(8, QLatin1Char('0')));
}

std::optional<TextureMipChain> TrackTextures::loadBundled(
    const ForzaZip& archive, std::uint32_t id, QString* error) const
{
    const QString name = textureName(id);
    const auto record = m_records.constFind(id);
    const auto location = record == m_records.constEnd() ? m_bundled.constEnd() : m_bundled.constFind(record.value());
    if (location == m_bundled.constEnd() || location->archiveEntry >= archive.entries().size()) {
        setError(error, QStringLiteral("texture %1 is not in the archive").arg(name));
        return std::nullopt;
    }
    QString readError;
    const QByteArray pack = m_packs->get(archive, location->archiveEntry, &readError);
    const BundleEntry& entry = location->entry;
    if (pack.isNull() || entry.offset + entry.size > pack.size()) {
        setError(error,
            QStringLiteral("the bundle copy of texture %1 cannot be read: %2")
                .arg(name, readError.isEmpty() ? QStringLiteral("the pack changed") : readError));
        return std::nullopt;
    }
    const std::optional<TextureSurface> surface = untileXboxSurface(
        pack.mid(entry.offset, entry.size), entry.formatWord, entry.width, entry.height, MipLayout::Packed, error);
    if (!surface) {
        return std::nullopt;
    }
    TextureMipChain chain = buildMipChain(*surface);
    chain.origin = TextureMipChain::Origin::Bundle;
    chain.files = QStringLiteral("%1 (record %2)")
                      .arg(archive.entries()[location->archiveEntry].name)
                      .arg(location->entry.record);
    return chain;
}

} // namespace fh1
