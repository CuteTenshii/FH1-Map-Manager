#include "ForzaZip.h"

#include "Lzx.h"

#include <zlib.h>

#include <algorithm>
#include <span>

namespace fh1 {

namespace {

constexpr std::uint32_t kEndOfCentralDirSig = 0x06054b50;
constexpr std::uint32_t kCentralDirSig = 0x02014b50;
constexpr std::uint32_t kLocalHeaderSig = 0x04034b50;
constexpr std::uint16_t kForzaOffsetExtra = 0x1123;
constexpr std::uint16_t kMethodStored = 0;
constexpr std::uint16_t kMethodXMemLzx = 21;

std::uint16_t le16(const uchar* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t le32(const uchar* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
        | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

} // namespace

QString ForzaZip::normalizeName(const QString& name)
{
    QString result = name.toLower();
    result.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (result.startsWith(QLatin1Char('/'))) {
        result.remove(0, 1);
    }
    return result;
}

bool ForzaZip::fail(const QString& message)
{
    m_error = message;
    if (m_file.isOpen()) {
        if (m_map != nullptr) {
            m_file.unmap(const_cast<uchar*>(m_map));
        }
        m_file.close();
    }
    m_map = nullptr;
    m_size = 0;
    m_entries.clear();
    m_index.clear();
    return false;
}

bool ForzaZip::open(const QString& path)
{
    if (m_file.isOpen()) {
        fail({});
    }
    m_error.clear();
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("cannot open %1: %2").arg(path, m_file.errorString()));
    }
    m_size = m_file.size();
    m_map = m_file.map(0, m_size);
    if (m_map == nullptr) {
        return fail(QStringLiteral("cannot map %1: %2").arg(path, m_file.errorString()));
    }
    return readCentralDirectory(m_map, m_size);
}

bool ForzaZip::readCentralDirectory(const uchar* base, qint64 size)
{
    constexpr qint64 kEocdSize = 22;
    if (size < kEocdSize) {
        return fail(QStringLiteral("%1 is too small to be a zip archive").arg(m_file.fileName()));
    }
    const qint64 searchStart = std::max<qint64>(0, size - kEocdSize - 0xFFFF);
    qint64 eocd = -1;
    for (qint64 pos = size - kEocdSize; pos >= searchStart; --pos) {
        if (le32(base + pos) == kEndOfCentralDirSig) {
            eocd = pos;
            break;
        }
    }
    if (eocd < 0) {
        return fail(QStringLiteral("%1 has no end-of-central-directory record").arg(m_file.fileName()));
    }

    const std::uint16_t entryCount = le16(base + eocd + 10);
    const std::uint32_t dirSize = le32(base + eocd + 12);
    const std::uint32_t dirOffset = le32(base + eocd + 16);
    if (static_cast<qint64>(dirOffset) + dirSize > size) {
        return fail(QStringLiteral("%1 has a central directory outside the file").arg(m_file.fileName()));
    }

    // The 16-bit count in the end record wraps for archives such as
    // tracks/colorado/bin.zip (230k entries), so walk the directory by size.
    Q_UNUSED(entryCount);
    m_entries.clear();
    m_index.clear();
    qint64 pos = dirOffset;
    const qint64 dirEnd = static_cast<qint64>(dirOffset) + dirSize;
    while (pos + 46 <= dirEnd) {
        const uchar* rec = base + pos;
        if (le32(rec) != kCentralDirSig) {
            return fail(
                QStringLiteral("%1: bad central directory record at offset %2").arg(m_file.fileName()).arg(pos));
        }
        const std::uint16_t nameLen = le16(rec + 28);
        const std::uint16_t extraLen = le16(rec + 30);
        const std::uint16_t commentLen = le16(rec + 32);
        if (pos + 46 + nameLen + extraLen + commentLen > dirEnd) {
            return fail(QStringLiteral("%1: truncated central directory").arg(m_file.fileName()));
        }

        ZipEntry entry;
        entry.method = le16(rec + 10);
        entry.crc32 = le32(rec + 16);
        entry.compressedSize = le32(rec + 20);
        entry.uncompressedSize = le32(rec + 24);
        entry.headerOffset = le32(rec + 42);
        entry.name = QString::fromUtf8(reinterpret_cast<const char*>(rec + 46), nameLen);

        const uchar* extra = rec + 46 + nameLen;
        const uchar* extraEnd = extra + extraLen;
        while (extra + 4 <= extraEnd) {
            const std::uint16_t id = le16(extra);
            const std::uint16_t len = le16(extra + 2);
            if (extra + 4 + len > extraEnd) {
                break;
            }
            if (id == kForzaOffsetExtra && len >= 4) {
                entry.dataOffset = le32(extra + 4);
                entry.dataOffsetKnown = true;
            }
            extra += 4 + len;
        }

        m_index.insert(normalizeName(entry.name), m_entries.size());
        m_entries.push_back(std::move(entry));
        pos += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

const ZipEntry* ForzaZip::find(const QString& name) const
{
    const auto it = m_index.constFind(normalizeName(name));
    return it == m_index.cend() ? nullptr : &m_entries[it.value()];
}

QStringList ForzaZip::list(const QString& prefix) const
{
    const QString normalizedPrefix = normalizeName(prefix);
    QStringList names;
    for (const ZipEntry& entry : m_entries) {
        if (normalizedPrefix.isEmpty() || normalizeName(entry.name).startsWith(normalizedPrefix)) {
            names.append(entry.name);
        }
    }
    return names;
}

std::uint64_t ForzaZip::resolveDataOffset(const ZipEntry& entry, QString* error) const
{
    if (entry.dataOffsetKnown) {
        return entry.dataOffset;
    }
    const qint64 header = entry.headerOffset;
    if (header + 30 > m_size || le32(m_map + header) != kLocalHeaderSig) {
        if (error != nullptr) {
            *error = QStringLiteral("%1 has neither a data offset field nor a local header").arg(entry.name);
        }
        return 0;
    }
    return static_cast<std::uint64_t>(header) + 30 + le16(m_map + header + 26) + le16(m_map + header + 28);
}

QByteArray ForzaZip::read(const QString& name, QString* error) const
{
    const ZipEntry* entry = find(name);
    if (entry == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("%1 is not in %2").arg(name, m_file.fileName());
        }
        return {};
    }
    return read(*entry, error);
}

QByteArray ForzaZip::readPrefix(const ZipEntry& entry, qsizetype size, QString* error) const
{
    auto failWith = [error](const QString& message) {
        if (error != nullptr) {
            *error = message;
        }
        return QByteArray();
    };
    if (m_map == nullptr) {
        return failWith(QStringLiteral("archive is not open"));
    }
    QString offsetError;
    const std::uint64_t offset = resolveDataOffset(entry, &offsetError);
    if (!offsetError.isEmpty()) {
        return failWith(offsetError);
    }
    if (offset + entry.compressedSize > static_cast<std::uint64_t>(m_size)) {
        return failWith(QStringLiteral("%1: data runs past end of archive").arg(entry.name));
    }
    const std::span<const std::uint8_t> raw(m_map + offset, entry.compressedSize);
    const auto wanted = static_cast<std::size_t>(std::max<qsizetype>(size, 0));
    switch (entry.method) {
    case kMethodStored: {
        const std::size_t n = std::min<std::size_t>(wanted, raw.size());
        return QByteArray(reinterpret_cast<const char*>(raw.data()), static_cast<qsizetype>(n));
    }
    case kMethodXMemLzx:
        try {
            const std::vector<std::uint8_t> out = LzxDecoder::decompress(raw, entry.uncompressedSize, 17, wanted);
            return QByteArray(reinterpret_cast<const char*>(out.data()), static_cast<qsizetype>(out.size()));
        } catch (const LzxError& e) {
            return failWith(QStringLiteral("%1: LZX decode failed: %2").arg(entry.name, QString::fromUtf8(e.what())));
        }
    default:
        return failWith(QStringLiteral("%1: unsupported compression method %2").arg(entry.name).arg(entry.method));
    }
}

QByteArray ForzaZip::read(const ZipEntry& entry, QString* error) const
{
    auto failWith = [error](const QString& message) {
        if (error != nullptr) {
            *error = message;
        }
        return QByteArray();
    };

    if (m_map == nullptr) {
        return failWith(QStringLiteral("archive is not open"));
    }
    QString offsetError;
    const std::uint64_t offset = resolveDataOffset(entry, &offsetError);
    if (!offsetError.isEmpty()) {
        return failWith(offsetError);
    }
    if (offset + entry.compressedSize > static_cast<std::uint64_t>(m_size)) {
        return failWith(QStringLiteral("%1: data runs past end of archive").arg(entry.name));
    }
    const std::span<const std::uint8_t> raw(m_map + offset, entry.compressedSize);

    QByteArray data;
    switch (entry.method) {
    case kMethodStored:
        if (entry.compressedSize != entry.uncompressedSize) {
            return failWith(QStringLiteral("%1: stored entry has mismatched sizes").arg(entry.name));
        }
        data = QByteArray(reinterpret_cast<const char*>(raw.data()), static_cast<qsizetype>(raw.size()));
        break;
    case kMethodXMemLzx:
        try {
            const std::vector<std::uint8_t> out = LzxDecoder::decompress(raw, entry.uncompressedSize);
            data = QByteArray(reinterpret_cast<const char*>(out.data()), static_cast<qsizetype>(out.size()));
        } catch (const LzxError& e) {
            return failWith(QStringLiteral("%1: LZX decode failed: %2").arg(entry.name, QString::fromUtf8(e.what())));
        }
        break;
    default:
        return failWith(QStringLiteral("%1: unsupported compression method %2").arg(entry.name).arg(entry.method));
    }

    const auto crc = static_cast<std::uint32_t>(
        crc32(0L, reinterpret_cast<const Bytef*>(data.constData()), static_cast<uInt>(data.size())));
    if (crc != entry.crc32) {
        return failWith(QStringLiteral("%1: CRC mismatch (got %2, expected %3)")
                .arg(entry.name)
                .arg(crc, 8, 16, QLatin1Char('0'))
                .arg(entry.crc32, 8, 16, QLatin1Char('0')));
    }
    return data;
}

} // namespace fh1
