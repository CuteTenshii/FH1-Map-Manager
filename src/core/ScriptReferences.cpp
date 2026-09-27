#include "ScriptReferences.h"

#include <QXmlStreamReader>

namespace fh1 {

void ScriptReferences::addFile(const QString& file, const QByteArray& xml)
{
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        if (reader.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }
        for (const QXmlStreamAttribute& attribute : reader.attributes()) {
            const QString value = attribute.value().toString().toUpper();
            if (!value.isEmpty()) {
                m_values[value].insert(file);
            }
        }
    }
}

QStringList ScriptReferences::filesUsing(const QString& name) const
{
    if (name.isEmpty()) {
        return {};
    }
    const QString exact = name.toUpper();
    const QString suffix = QLatin1Char('_') + exact;
    QSet<QString> files;
    for (auto [value, users] : m_values.asKeyValueRange()) {
        if (value == exact || value.endsWith(suffix)) {
            files.unite(users);
        }
    }
    QStringList sorted(files.begin(), files.end());
    sorted.sort(Qt::CaseInsensitive);
    return sorted;
}

} // namespace fh1
