#include "XmlElements.h"

#include "Loaders.h"

#include <QXmlStreamReader>

#include <algorithm>
#include <string_view>

namespace fh1 {

namespace {

constexpr std::string_view kByteOrderMark("\xEF\xBB\xBF");

[[noreturn]] void fail(const QString& source, const QString& message)
{
    throw LoadError(QStringLiteral("%1: %2").arg(source, message).toStdString());
}

/// `text` with the element named `from` renamed to `to`, in its opening
/// and closing tags.
QByteArray renamed(QByteArray text, const QString& from, const QString& to)
{
    const QByteArray oldName = from.toLatin1();
    const QByteArray newName = to.toLatin1();
    const qsizetype close = text.lastIndexOf("</" + oldName + '>');
    const qsizetype open = text.indexOf('<' + oldName);
    if (close >= 0) {
        text.replace(close + 2, oldName.size(), newName);
    }
    if (open >= 0) {
        text.replace(open + 1, oldName.size(), newName);
    }
    return text;
}

} // namespace

XmlElementsFile readXmlElements(const QByteArray& data, const QString& source)
{
    XmlElementsFile file;
    file.text = data;
    const auto offset
        = data.startsWith(QByteArrayView(kByteOrderMark)) ? static_cast<qsizetype>(kByteOrderMark.size()) : 0;
    const QByteArray body = data.mid(offset);
    if (std::any_of(body.begin(), body.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; })) {
        fail(source, QStringLiteral("not plain ASCII, so its elements cannot be edited"));
    }
    QXmlStreamReader xml(body);
    int depth = 0;
    qsizetype nextStart = -1;
    qsizetype start = -1;
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            ++depth;
            if (depth == 2) {
                const QString name = xml.name().toString();
                const qsizetype tag = body.lastIndexOf('<' + name.toLatin1(), xml.characterOffset());
                start = nextStart < 0 ? body.lastIndexOf('\n', tag) + 1 : nextStart;
                file.names.push_back(name);
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (depth == 2) {
                qsizetype end = xml.characterOffset();
                if (body.mid(end, 2) == "\r\n") {
                    end += 2;
                } else if (body.mid(end, 1) == "\n") {
                    end += 1;
                }
                file.elements.emplace_back(start + offset, end + offset);
                nextStart = end;
            }
            --depth;
        }
    }
    if (xml.hasError()) {
        fail(source, QStringLiteral("line %1: %2").arg(xml.lineNumber()).arg(xml.errorString()));
    }
    if (!file.elements.empty()) {
        file.elementsStart = file.elements.front().first;
        file.elementsEnd = file.elements.back().second;
    }
    file.numbered = !file.names.empty();
    for (std::size_t i = 0; i < file.names.size() && file.numbered; ++i) {
        file.numbered = file.names[i] == QStringLiteral("Obj%1").arg(i);
    }
    return file;
}

void removeXmlElement(XmlElementsFile& file, std::size_t index)
{
    if (index < file.elements.size()) {
        file.elements.erase(file.elements.begin() + static_cast<std::ptrdiff_t>(index));
        file.names.erase(file.names.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

QByteArray writeXmlElements(const XmlElementsFile& file)
{
    if (file.elementsStart < 0) {
        return file.text;
    }
    QByteArray out = file.text.left(file.elementsStart);
    for (std::size_t i = 0; i < file.elements.size(); ++i) {
        const auto [start, end] = file.elements[i];
        const QByteArray text = file.text.mid(start, end - start);
        const QString name = QStringLiteral("Obj%1").arg(i);
        out += file.numbered && file.names[i] != name ? renamed(text, file.names[i], name) : text;
    }
    return out + file.text.mid(file.elementsEnd);
}

} // namespace fh1
