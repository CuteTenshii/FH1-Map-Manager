#pragma once

#include "ForzaZip.h"

#include <QByteArray>
#include <QHash>
#include <QString>

#include <memory>
#include <optional>

namespace fh1 {

class GameInstall;

/// One localized string table (a `.str` file from media/stringtables/<LANG>.zip).
///
/// Layout (big-endian unless noted): "LSB2", header words, then at 0x28 a u32
/// entry count N, then N entries {u16 key, u32 offset} sorted by key and a
/// 0xFFFF sentinel entry, then NUL-terminated UTF-16BE strings. Offsets count
/// UTF-16 code units from the start of the string area.
class StringTable {
public:
    /// Parses a table; on failure returns nothing and sets `error`.
    static std::optional<StringTable> parse(const QByteArray& data, QString* error = nullptr);

    /// The string for `key`, or a null QString if the table has none.
    QString value(quint16 key) const { return m_strings.value(key); }
    qsizetype size() const { return m_strings.size(); }

private:
    QHash<quint16, QString> m_strings;
};

/// The string tables of one language, loaded on demand.
///
/// Database text columns hold references such as "_&202713995": the low 16
/// bits are the key in the table named after the database table (Events.str
/// for Events, Tracks.str for Tracks); the high bits identify the table.
class StringTables {
public:
    /// Opens media/stringtables/<language>.zip. On failure returns false and
    /// sets errorString(); lookups then return null strings.
    bool open(const GameInstall& install, const QString& language = QStringLiteral("EN"));
    QString errorString() const { return m_error; }

    /// Looks up `key` in `table` (a file name such as "Events.str").
    QString lookup(const QString& table, quint16 key);
    /// Resolves a database reference ("_&123") through `table`; returns a null
    /// QString if `reference` is not such a reference or has no entry.
    QString resolve(const QString& table, const QString& reference);

    /// Parses "_&<number>" into its 16-bit key.
    static std::optional<quint16> referenceKey(const QString& reference);

private:
    const StringTable* table(const QString& name);

    std::unique_ptr<ForzaZip> m_zip;
    QHash<QString, std::shared_ptr<StringTable>> m_tables;
    QString m_error;
};

} // namespace fh1
