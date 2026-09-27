#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

#include <cstdint>
#include <functional>

namespace fh1 {

/// Writes `target`: the game's zip archive `source` with the contents of the
/// entries at positions `replaced` (indices into its central directory, as
/// ForzaZip::entries() lists them) replaced.
///
/// Every byte of `source` before its central directory is kept, so entries
/// that did not change are neither moved nor recompressed. The new contents
/// follow, each after a local header, stored rather than compressed: the
/// game reads stored entries (bin.zip stores its .pgeo sets that way), but
/// read zone files that the lzx library had compressed as garbage, and
/// crashed. Then comes the central directory, record for record as in
/// `source` but pointing the replaced entries at their new data, both in
/// the local header offset and in the game's data offset field (0x1123).
/// Entries are matched by position, so an archive that stores a name more
/// than once, as bin.zip does, is handled. The archive is written to a
/// temporary file that replaces `target` only once complete, so `target`
/// may be `source`. `progress` is told how many of the bytes to copy are
/// done. Returns false, with `error` set, if `source` is not such an
/// archive or a file cannot be read or written.
bool writeUpdatedArchive(const QString& source, const QString& target, const QHash<std::uint32_t, QByteArray>& replaced,
    QString* error = nullptr, const std::function<void(qint64 done, qint64 total)>& progress = {});

} // namespace fh1
