#include "GameObjects.h"

#include "Loaders.h"

#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace fh1 {

namespace {

/// Room for any float written with "%.40f".
constexpr std::size_t kNumberBuffer = 128;
/// Decimals the game writes in GameObjs.xml.
constexpr std::size_t kFixedDecimals = 6;

[[noreturn]] void fail(const QString& source, const QXmlStreamReader& xml, const QString& message)
{
    throw LoadError(QStringLiteral("%1: line %2: %3").arg(source).arg(xml.lineNumber()).arg(message).toStdString());
}

QVector3D vectorAttributes(const QXmlStreamReader& xml, const QString& source)
{
    QVector3D value;
    for (int axis = 0; axis < 3; ++axis) {
        const QString name(QLatin1Char("xyz"[axis]));
        bool ok = false;
        const QStringView text = xml.attributes().value(name);
        float component = text.toFloat(&ok);
        if (!ok) {
            fail(source, xml, QStringLiteral("<%1> has no numeric %2 attribute").arg(xml.name().toString(), name));
        }
        // The game writes tiny negative values as "-0.000000", which Qt
        // reads as plain zero; the sign is kept so saving writes it back.
        if (component == 0.0F && text.startsWith(QLatin1Char('-'))) {
            component = -0.0F;
        }
        value[axis] = component;
    }
    return value;
}

QString formatFixed(float value)
{
    // Rounded by hand from the exact value: glibc rounds halfway cases to
    // even ("4148.789062"), the game's C library away from zero
    // ("4148.789063"), and Qt's own formatting drops the sign of negative
    // zero, which the game's files keep ("-0.000000").
    std::array<char, kNumberBuffer> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.40f", static_cast<double>(value));
    std::string exact(buffer.data());
    const bool negative = !exact.empty() && exact.front() == '-';
    if (negative) {
        exact.erase(0, 1);
    }
    const std::size_t point = exact.find('.');
    std::string digits = exact.substr(0, point) + exact.substr(point + 1, kFixedDecimals);
    if (exact.size() > point + 1 + kFixedDecimals && exact[point + 1 + kFixedDecimals] >= '5') {
        std::size_t i = digits.size();
        while (i > 0 && digits[i - 1] == '9') {
            digits[--i] = '0';
        }
        if (i == 0) {
            digits.insert(digits.begin(), '1');
        } else {
            ++digits[i - 1];
        }
    }
    digits.insert(digits.size() - kFixedDecimals, 1, '.');
    return QString::fromLatin1(negative ? '-' + digits : digits);
}

/// Writes `value` into the x, y and z attributes of the first `<element`
/// tag in `text`. Returns false if there is no such tag.
bool setVectorAttributes(QByteArray& text, const QByteArray& element, const QVector3D& value)
{
    const qsizetype tag = text.indexOf('<' + element + ' ');
    const qsizetype end = tag < 0 ? -1 : text.indexOf('>', tag);
    if (end < 0) {
        return false;
    }
    QString tagText = QString::fromLatin1(text.mid(tag, end - tag));
    for (int axis = 0; axis < 3; ++axis) {
        const QRegularExpression pattern(QStringLiteral("(\\s%1=\")[^\"]*(\")").arg(QLatin1Char("xyz"[axis])));
        if (!pattern.match(tagText).hasMatch()) {
            return false;
        }
        tagText.replace(pattern, QStringLiteral("\\1%1\\2").arg(formatFixed(value[axis])));
    }
    text.replace(tag, end - tag, tagText.toLatin1());
    return true;
}

/// An object written anew in the layout of the game's files.
QByteArray objectText(const GameObject& object, const QString& element)
{
    const auto vector = [](const char* name, const QVector3D& v) {
        return QByteArray(name) + " x=\"" + formatFixed(v.x()).toLatin1() + "\" y=\"" + formatFixed(v.y()).toLatin1()
            + "\" z=\"" + formatFixed(v.z()).toLatin1() + "\"/>\r\n";
    };
    QString id = object.gameplayId;
    id.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    id.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    id.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    const QByteArray name = element.toLatin1();
    return "\t<" + name + " GameplayID=\"" + id.toUtf8() + "\">\r\n\t\t" + vector("<Pos", object.position)
        + "\t\t<Orientation>\r\n\t\t\t" + vector("<XAxis", object.axes[0]) + "\t\t\t" + vector("<YAxis", object.axes[1])
        + "\t\t\t" + vector("<ZAxis", object.axes[2]) + "\t\t</Orientation>\r\n\t</" + name + ">\r\n";
}

/// The object's text with its element renamed to `element` and, if it was
/// edited, its numbers rewritten; empty if the text does not have the
/// expected elements.
QByteArray patchedText(const GameObject& object, const QString& element, const QByteArray& original)
{
    QByteArray text = original;
    if (element != object.element) {
        const QByteArray from = object.element.toLatin1();
        const QByteArray to = element.toLatin1();
        const qsizetype open = text.indexOf('<' + from + ' ');
        const qsizetype close = text.lastIndexOf("</" + from + '>');
        if (open < 0 || close < 0) {
            return {};
        }
        text.replace(close + 2, from.size(), to);
        text.replace(open + 1, from.size(), to);
    }
    if (object.edited
        && (!setVectorAttributes(text, "Pos", object.position) || !setVectorAttributes(text, "XAxis", object.axes[0])
            || !setVectorAttributes(text, "YAxis", object.axes[1])
            || !setVectorAttributes(text, "ZAxis", object.axes[2]))) {
        return {};
    }
    return text;
}

QVector3D turned(const QVector3D& v, float cosine, float sine)
{
    return {v.x() * cosine - v.z() * sine, v.y(), v.x() * sine + v.z() * cosine};
}

} // namespace

GameObjectsFile readGameObjects(const QByteArray& data, const QString& source)
{
    GameObjectsFile file;
    const bool ascii
        = std::all_of(data.begin(), data.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    if (ascii) {
        file.text = data;
    }
    QXmlStreamReader xml(data);
    int depth = 0;
    GameObject object;
    bool havePosition = false;
    qsizetype nextStart = -1;
    while (!xml.atEnd()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            ++depth;
            if (depth == 2) {
                object = GameObject();
                havePosition = false;
                object.element = xml.name().toString();
                object.gameplayId = xml.attributes().value(QLatin1String("GameplayID")).toString();
                if (ascii) {
                    const qsizetype tag = data.lastIndexOf('<' + object.element.toLatin1(), xml.characterOffset());
                    object.sourceStart = nextStart < 0 ? data.lastIndexOf('\n', tag) + 1 : nextStart;
                }
            } else if (depth == 3 && xml.name() == QLatin1String("Pos")) {
                object.position = vectorAttributes(xml, source);
                havePosition = true;
            } else if (depth == 4 && xml.name() == QLatin1String("XAxis")) {
                object.axes[0] = vectorAttributes(xml, source);
            } else if (depth == 4 && xml.name() == QLatin1String("YAxis")) {
                object.axes[1] = vectorAttributes(xml, source);
            } else if (depth == 4 && xml.name() == QLatin1String("ZAxis")) {
                object.axes[2] = vectorAttributes(xml, source);
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (depth == 2) {
                if (!havePosition) {
                    fail(source, xml, QStringLiteral("object %1 has no <Pos>").arg(object.gameplayId));
                }
                if (ascii) {
                    qsizetype end = xml.characterOffset();
                    if (data.mid(end, 2) == "\r\n") {
                        end += 2;
                    } else if (data.mid(end, 1) == "\n") {
                        end += 1;
                    }
                    object.sourceEnd = end;
                    nextStart = end;
                }
                file.objects.push_back(object);
            }
            --depth;
        }
    }
    if (xml.hasError()) {
        fail(source, xml, xml.errorString());
    }
    if (ascii && !file.objects.empty()) {
        file.objectsStart = file.objects.front().sourceStart;
        file.objectsEnd = file.objects.back().sourceEnd;
    }
    return file;
}

QByteArray writeGameObjects(const GameObjectsFile& file)
{
    QByteArray objects;
    for (std::size_t i = 0; i < file.objects.size(); ++i) {
        const GameObject& object = file.objects[i];
        const QString element = QStringLiteral("Obj%1").arg(i);
        const bool fromFile = !file.text.isEmpty() && object.sourceStart >= 0;
        if (fromFile && !object.edited && element == object.element) {
            objects += file.text.mid(object.sourceStart, object.sourceEnd - object.sourceStart);
            continue;
        }
        const QByteArray patched = fromFile
            ? patchedText(object, element, file.text.mid(object.sourceStart, object.sourceEnd - object.sourceStart))
            : QByteArray();
        objects += patched.isEmpty() ? objectText(object, element) : patched;
    }
    if (file.text.isEmpty() || file.objectsStart < 0) {
        return "<?xml version=\"1.0\" ?>\r\n<GameObjs>\r\n" + objects + "</GameObjs>\r\n";
    }
    return file.text.left(file.objectsStart) + objects + file.text.mid(file.objectsEnd);
}

void moveGameObject(GameObjectsFile& file, std::size_t index, const QVector3D& position)
{
    if (index < file.objects.size()) {
        file.objects[index].position = position;
        file.objects[index].edited = true;
    }
}

void turnGameObject(GameObjectsFile& file, std::size_t index, const QVector3D& facing)
{
    if (index >= file.objects.size()) {
        return;
    }
    GameObject& object = file.objects[index];
    const QVector3D forward = object.axes[2];
    const float current = std::atan2(forward.z(), forward.x());
    const float target = std::atan2(facing.z(), facing.x());
    if (QVector3D(facing.x(), 0.0F, facing.z()).isNull() || QVector3D(forward.x(), 0.0F, forward.z()).isNull()) {
        return;
    }
    const float cosine = std::cos(target - current);
    const float sine = std::sin(target - current);
    for (QVector3D& axis : object.axes) {
        axis = turned(axis, cosine, sine);
    }
    object.edited = true;
}

namespace {

/// Words ending an ID that name an object's role within its group.
bool isRoleSuffix(const QString& part)
{
    static const QSet<QString> roles{QStringLiteral("NODE"), QStringLiteral("ENDNODE"), QStringLiteral("L"),
        QStringLiteral("R"), QStringLiteral("LEFT"), QStringLiteral("RIGHT"), QStringLiteral("DISCOVERY"),
        QStringLiteral("OPEN"), QStringLiteral("CLOSED"), QStringLiteral("OPENC"), QStringLiteral("CLOSEDC")};
    return roles.contains(part.toUpper());
}

/// What an ID names, with role suffixes and object numbers dropped (see
/// gameObjectGroup()), upper-cased.
QString groupKey(const QString& id, const QSet<QString>& ids)
{
    QString key = id.toUpper();
    for (;;) {
        const qsizetype underscore = key.lastIndexOf(QLatin1Char('_'));
        if (underscore <= 0) {
            break;
        }
        const QString part = key.mid(underscore + 1);
        const QString rest = key.left(underscore);
        const bool number
            = !part.isEmpty() && std::all_of(part.begin(), part.end(), [](QChar c) { return c.isDigit(); });
        if (isRoleSuffix(part) || (number && ids.contains(rest))) {
            key = rest;
        } else {
            break;
        }
    }
    if (key.startsWith(QLatin1String("BF_"))) {
        key = QStringLiteral("BARNFIND_") + key.mid(3);
    }
    return key;
}

} // namespace

std::vector<std::size_t> gameObjectGroup(const GameObjectsFile& file, std::size_t index, float radius)
{
    std::vector<std::size_t> group;
    if (index >= file.objects.size()) {
        return group;
    }
    QSet<QString> ids;
    for (const GameObject& object : file.objects) {
        ids.insert(object.gameplayId.toUpper());
    }
    const GameObject& grabbed = file.objects[index];
    const QString key = groupKey(grabbed.gameplayId, ids);
    group.push_back(index);
    for (std::size_t i = 0; i < file.objects.size(); ++i) {
        const GameObject& other = file.objects[i];
        if (i != index && (other.position - grabbed.position).length() <= radius
            && groupKey(other.gameplayId, ids) == key) {
            group.push_back(i);
        }
    }
    return group;
}

void turnGameObjects(
    GameObjectsFile& file, const std::vector<std::size_t>& indices, const QVector3D& pivot, float radians)
{
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    for (const std::size_t index : indices) {
        if (index >= file.objects.size()) {
            continue;
        }
        GameObject& object = file.objects[index];
        object.position = pivot + turned(object.position - pivot, cosine, sine);
        for (QVector3D& axis : object.axes) {
            axis = turned(axis, cosine, sine);
        }
        object.edited = true;
    }
}

void removeGameObject(GameObjectsFile& file, std::size_t index)
{
    if (index < file.objects.size()) {
        file.objects.erase(file.objects.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

} // namespace fh1
