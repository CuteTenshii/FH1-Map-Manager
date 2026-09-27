#include "ArchiveUpdate.h"

#include <QFile>
#include <QSaveFile>

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace fh1 {

namespace {

constexpr std::uint32_t kEndOfCentralDirSig = 0x06054b50;
constexpr std::uint32_t kCentralDirSig = 0x02014b50;
constexpr std::uint32_t kLocalHeaderSig = 0x04034b50;
constexpr std::uint16_t kOffsetExtraField = 0x1123;
constexpr std::uint16_t kMethodStored = 0;
constexpr qsizetype kEndRecordBytes = 22;
constexpr qsizetype kMaxCommentBytes = 0xFFFF;
constexpr qsizetype kCentralRecordBytes = 46;
constexpr qsizetype kLocalHeaderBytes = 30;
/// Bytes copied at a time.
constexpr qint64 kCopyBlock = qint64{16} * 1024 * 1024;

std::uint16_t le16(const char* p)
{
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return static_cast<std::uint16_t>(u[0] | (u[1] << 8));
}

std::uint32_t le32(const char* p)
{
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return static_cast<std::uint32_t>(u[0]) | (static_cast<std::uint32_t>(u[1]) << 8)
        | (static_cast<std::uint32_t>(u[2]) << 16) | (static_cast<std::uint32_t>(u[3]) << 24);
}

void put16(QByteArray& data, qsizetype at, std::uint16_t value)
{
    data[at] = static_cast<char>(value & 0xFF);
    data[at + 1] = static_cast<char>(value >> 8);
}

void put32(QByteArray& data, qsizetype at, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        data[at + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
    }
}

void append16(QByteArray& data, std::uint16_t value)
{
    data.append(2, '\0');
    put16(data, data.size() - 2, value);
}

void append32(QByteArray& data, std::uint32_t value)
{
    data.append(4, '\0');
    put32(data, data.size() - 4, value);
}

bool fail(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

} // namespace

bool writeUpdatedArchive(const QString& source, const QString& target, const QHash<std::uint32_t, QByteArray>& replaced,
    QString* error, const std::function<void(qint64 done, qint64 total)>& progress)
{
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) {
        return fail(error, QStringLiteral("cannot read %1: %2").arg(source, in.errorString()));
    }
    const qint64 size = in.size();
    const qint64 tailStart = std::max<qint64>(0, size - kEndRecordBytes - kMaxCommentBytes);
    in.seek(tailStart);
    const QByteArray tail = in.read(size - tailStart);
    qsizetype end = -1;
    for (qsizetype pos = tail.size() - kEndRecordBytes; pos >= 0; --pos) {
        if (le32(tail.constData() + pos) == kEndOfCentralDirSig) {
            end = pos;
            break;
        }
    }
    if (end < 0) {
        return fail(error, QStringLiteral("%1 is not a zip archive").arg(source));
    }
    QByteArray endRecord = tail.mid(end);
    const std::uint32_t directorySize = le32(endRecord.constData() + 12);
    const std::uint32_t directoryOffset = le32(endRecord.constData() + 16);
    if (static_cast<qint64>(directoryOffset) + directorySize > size) {
        return fail(error, QStringLiteral("%1 has a central directory outside the file").arg(source));
    }
    in.seek(directoryOffset);
    QByteArray directory = in.read(directorySize);
    if (directory.size() != static_cast<qsizetype>(directorySize)) {
        return fail(error, QStringLiteral("cannot read the central directory of %1").arg(source));
    }

    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly)) {
        return fail(error, QStringLiteral("cannot write %1: %2").arg(target, out.errorString()));
    }
    // Everything up to the central directory, as it is.
    in.seek(0);
    qint64 copied = 0;
    while (copied < directoryOffset) {
        const QByteArray block = in.read(std::min<qint64>(kCopyBlock, directoryOffset - copied));
        if (block.isEmpty() || out.write(block) != block.size()) {
            return fail(error, QStringLiteral("cannot copy %1 to %2").arg(source, target));
        }
        copied += block.size();
        if (progress) {
            progress(copied, directoryOffset);
        }
    }

    // The new contents, and the directory pointed at them.
    qint64 offset = directoryOffset;
    qsizetype pos = 0;
    for (std::uint32_t index = 0; pos + kCentralRecordBytes <= directory.size(); ++index) {
        const char* record = directory.constData() + pos;
        if (le32(record) != kCentralDirSig) {
            return fail(error, QStringLiteral("%1: bad central directory record %2").arg(source).arg(index));
        }
        const std::uint16_t nameLength = le16(record + 28);
        const std::uint16_t extraLength = le16(record + 30);
        const std::uint16_t commentLength = le16(record + 32);
        const qsizetype recordBytes = kCentralRecordBytes + nameLength + extraLength + commentLength;
        if (pos + recordBytes > directory.size()) {
            return fail(error, QStringLiteral("%1: truncated central directory").arg(source));
        }
        const auto found = replaced.constFind(index);
        if (found != replaced.cend()) {
            const QByteArray& data = found.value();
            const auto crc = static_cast<std::uint32_t>(
                crc32(0L, reinterpret_cast<const Bytef*>(data.constData()), static_cast<uInt>(data.size())));
            const QByteArray name = directory.mid(pos + kCentralRecordBytes, nameLength);
            const qint64 dataOffset = offset + kLocalHeaderBytes + nameLength + 8;
            if (dataOffset + data.size() > std::numeric_limits<std::uint32_t>::max()) {
                return fail(error, QStringLiteral("%1 would grow past 4 GB").arg(target));
            }
            QByteArray header;
            append32(header, kLocalHeaderSig);
            append16(header, le16(record + 6));
            append16(header, 0);
            append16(header, kMethodStored);
            append16(header, le16(record + 12));
            append16(header, le16(record + 14));
            append32(header, crc);
            append32(header, static_cast<std::uint32_t>(data.size()));
            append32(header, static_cast<std::uint32_t>(data.size()));
            append16(header, nameLength);
            append16(header, 8);
            header += name;
            append16(header, kOffsetExtraField);
            append16(header, 4);
            append32(header, static_cast<std::uint32_t>(dataOffset));
            if (out.write(header) != header.size() || out.write(data) != data.size()) {
                return fail(error, QStringLiteral("cannot write %1: %2").arg(target, out.errorString()));
            }
            put16(directory, pos + 10, kMethodStored);
            put32(directory, pos + 16, crc);
            put32(directory, pos + 20, static_cast<std::uint32_t>(data.size()));
            put32(directory, pos + 24, static_cast<std::uint32_t>(data.size()));
            put32(directory, pos + 42, static_cast<std::uint32_t>(offset));
            // The game finds the data through the offset field.
            qsizetype extra = pos + kCentralRecordBytes + nameLength;
            const qsizetype extraEnd = extra + extraLength;
            while (extra + 4 <= extraEnd) {
                const std::uint16_t id = le16(directory.constData() + extra);
                const std::uint16_t length = le16(directory.constData() + extra + 2);
                if (id == kOffsetExtraField && length >= 4) {
                    put32(directory, extra + 4, static_cast<std::uint32_t>(dataOffset));
                }
                extra += 4 + length;
            }
            offset = dataOffset + data.size();
        }
        pos += recordBytes;
    }
    if (offset > std::numeric_limits<std::uint32_t>::max()) {
        return fail(error, QStringLiteral("%1 would grow past 4 GB").arg(target));
    }
    put32(endRecord, 16, static_cast<std::uint32_t>(offset));
    if (out.write(directory) != directory.size() || out.write(endRecord) != endRecord.size() || !out.commit()) {
        return fail(error, QStringLiteral("cannot write %1: %2").arg(target, out.errorString()));
    }
    return true;
}

} // namespace fh1
