#include "StringTable.h"

#include "GameInstall.h"

#include <QtEndian>

namespace fh1 {

namespace {

constexpr qsizetype kCountOffset = 0x28;
constexpr qsizetype kEntriesOffset = 0x2C;
constexpr qsizetype kEntrySize = 6;

} // namespace

std::optional<StringTable> StringTable::parse(const QByteArray& data, QString* error)
{
    auto fail = [error](const QString& message) -> std::optional<StringTable> {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };
    if (data.size() < kEntriesOffset || !data.startsWith("LSB2")) {
        return fail(QStringLiteral("not an LSB2 string table"));
    }
    const auto count = qFromBigEndian<quint32>(data.constData() + kCountOffset);
    // The entries are followed by one sentinel entry.
    const qsizetype stringsOffset = kEntriesOffset + (static_cast<qsizetype>(count) + 1) * kEntrySize;
    if (stringsOffset > data.size()) {
        return fail(QStringLiteral("string table claims %1 entries but is too short").arg(count));
    }
    const qsizetype stringsUnits = (data.size() - stringsOffset) / 2;

    StringTable table;
    table.m_strings.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        const char* entry = data.constData() + kEntriesOffset + static_cast<qsizetype>(i) * kEntrySize;
        const auto key = qFromBigEndian<quint16>(entry);
        const auto offset = static_cast<qsizetype>(qFromBigEndian<quint32>(entry + 2));
        if (offset >= stringsUnits) {
            return fail(QStringLiteral("string %1 starts past the end of the table").arg(key));
        }
        QString text;
        for (qsizetype unit = offset; unit < stringsUnits; ++unit) {
            const auto c = qFromBigEndian<quint16>(data.constData() + stringsOffset + unit * 2);
            if (c == 0) {
                break;
            }
            text.append(QChar(c));
        }
        table.m_strings.insert(key, text);
    }
    return table;
}

bool StringTables::open(const GameInstall& install, const QString& language)
{
    m_tables.clear();
    m_zip.reset();
    const QString path = install.resolve(QStringLiteral("stringtables/%1.zip").arg(language));
    if (path.isEmpty()) {
        m_error = QStringLiteral("stringtables/%1.zip not found").arg(language);
        return false;
    }
    auto zip = std::make_unique<ForzaZip>();
    if (!zip->open(path)) {
        m_error = zip->errorString();
        return false;
    }
    m_zip = std::move(zip);
    return true;
}

const StringTable* StringTables::table(const QString& name)
{
    const QString key = name.toLower();
    const auto it = m_tables.constFind(key);
    if (it != m_tables.cend()) {
        return it.value().get();
    }
    std::shared_ptr<StringTable> loaded;
    if (m_zip) {
        QString error;
        const QByteArray data = m_zip->read(name, &error);
        if (!data.isNull()) {
            if (std::optional<StringTable> parsed = StringTable::parse(data, &error)) {
                loaded = std::make_shared<StringTable>(std::move(*parsed));
            }
        }
    }
    // A missing table is remembered too, so it is not searched for again.
    m_tables.insert(key, loaded);
    return loaded.get();
}

QString StringTables::lookup(const QString& table, quint16 key)
{
    const StringTable* strings = this->table(table);
    return strings == nullptr ? QString() : strings->value(key);
}

std::optional<quint16> StringTables::referenceKey(const QString& reference)
{
    if (!reference.startsWith(QLatin1String("_&"))) {
        return std::nullopt;
    }
    bool ok = false;
    const qulonglong id = QStringView(reference).mid(2).toULongLong(&ok);
    if (!ok) {
        return std::nullopt;
    }
    return static_cast<quint16>(id & 0xFFFFu);
}

QString StringTables::resolve(const QString& table, const QString& reference)
{
    const std::optional<quint16> key = referenceKey(reference);
    return key ? lookup(table, *key) : QString();
}

} // namespace fh1
