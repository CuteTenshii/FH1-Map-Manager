#pragma once

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <memory>
#include <vector>

namespace fh1 {

/// One file in a Forza zip archive, as described by the central directory.
struct ZipEntry {
    QString name;
    std::uint16_t method = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::uint32_t headerOffset = 0;
    /// Absolute offset of the entry's data. Forza writes it into extra field
    /// 0x1123 because many of its archives omit local file headers entirely.
    std::uint64_t dataOffset = 0;
    bool dataOffsetKnown = false;
};

/// Read-only access to the zip archives shipped with Forza Horizon.
///
/// Supports stored entries (method 0) and XMemCompress LZX entries
/// (method 21). Name lookups are case-insensitive and accept either slash
/// direction, matching how the game resolves paths.
class ForzaZip {
public:
    ForzaZip() = default;
    ForzaZip(const ForzaZip&) = delete;
    ForzaZip& operator=(const ForzaZip&) = delete;

    /// Opens the archive and reads its central directory. On failure returns
    /// false and sets errorString().
    bool open(const QString& path);
    bool isOpen() const { return m_file.isOpen(); }
    QString path() const { return m_file.fileName(); }
    QString errorString() const { return m_error; }

    const std::vector<ZipEntry>& entries() const { return m_entries; }
    const ZipEntry* find(const QString& name) const;
    /// Names of all entries whose normalized path starts with `prefix`.
    QStringList list(const QString& prefix = {}) const;

    /// Returns the decompressed contents of `entry`, verified against its
    /// CRC-32. On failure returns a null QByteArray and sets `error`.
    QByteArray read(const ZipEntry& entry, QString* error = nullptr) const;
    QByteArray read(const QString& name, QString* error = nullptr) const;
    /// Returns the first `size` bytes of `entry` (or all of it, if shorter),
    /// decompressing only as much as needed. The CRC cannot be checked on a
    /// partial read. On failure returns a null QByteArray and sets `error`.
    QByteArray readPrefix(const ZipEntry& entry, qsizetype size, QString* error = nullptr) const;

    static QString normalizeName(const QString& name);

private:
    bool fail(const QString& message);
    bool readCentralDirectory(const uchar* base, qint64 size);
    std::uint64_t resolveDataOffset(const ZipEntry& entry, QString* error) const;

    mutable QFile m_file;
    const uchar* m_map = nullptr;
    qint64 m_size = 0;
    std::vector<ZipEntry> m_entries;
    QHash<QString, std::size_t> m_index;
    QString m_error;
};

} // namespace fh1
