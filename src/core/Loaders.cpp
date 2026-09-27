#include "Loaders.h"

#include <QXmlStreamReader>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace fh1::loaders {

namespace {

[[noreturn]] void fail(const QString& source, const QString& message)
{
    throw LoadError(QStringLiteral("%1: %2").arg(source, message).toStdString());
}

[[noreturn]] void failXml(const QString& source, const QXmlStreamReader& xml, const QString& message)
{
    fail(source, QStringLiteral("line %1: %2").arg(xml.lineNumber()).arg(message));
}

float attributeFloat(const QXmlStreamReader& xml, const QString& source, const QString& name)
{
    const QStringView text = xml.attributes().value(name);
    if (text.isNull()) {
        failXml(source, xml, QStringLiteral("<%1> has no %2 attribute").arg(xml.name().toString(), name));
    }
    bool ok = false;
    const float value = text.toFloat(&ok);
    if (!ok) {
        failXml(source, xml, QStringLiteral("%1=\"%2\" is not a number").arg(name, text.toString()));
    }
    return value;
}

QVector3D attributeVector(const QXmlStreamReader& xml, const QString& source, const QString& prefix)
{
    return {attributeFloat(xml, source, prefix + QLatin1Char('x')),
        attributeFloat(xml, source, prefix + QLatin1Char('y')), attributeFloat(xml, source, prefix + QLatin1Char('z'))};
}

QString formatVector(const QVector3D& v)
{
    return QStringLiteral("%1, %2, %3")
        .arg(static_cast<double>(v.x()), 0, 'f', 2)
        .arg(static_cast<double>(v.y()), 0, 'f', 2)
        .arg(static_cast<double>(v.z()), 0, 'f', 2);
}

void checkXml(const QXmlStreamReader& xml, const QString& source)
{
    if (xml.hasError()) {
        failXml(source, xml, xml.errorString());
    }
}

/// Leading run of letters, upper-cased: "speed_camera_30" -> "SPEED",
/// "FR50_00" -> "FR", "BF_CUDA_426BF" -> "BF".
QString letterPrefix(const QString& name)
{
    qsizetype end = 0;
    while (end < name.size() && name.at(end).isLetter()) {
        ++end;
    }
    return end == 0 ? QStringLiteral("(other)") : name.left(end).toUpper();
}

std::uint32_t be32(const QByteArray& data, qsizetype offset)
{
    return qFromBigEndian<std::uint32_t>(data.constData() + offset);
}

float beFloat(const QByteArray& data, qsizetype offset)
{
    const std::uint32_t bits = be32(data, offset);
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

} // namespace

QString transformKind(const QString& name)
{
    QStringList parts = name.split(QLatin1Char('_'));
    auto isNumber = [](const QString& part) {
        return !part.isEmpty() && std::all_of(part.begin(), part.end(), [](QChar c) { return c.isDigit(); });
    };
    while (parts.size() > 1 && isNumber(parts.last())) {
        parts.removeLast();
    }
    const QString kind = parts.join(QLatin1Char('_'));
    return kind.isEmpty() ? QStringLiteral("(unnamed)") : kind;
}

QString gameplayGroup(const QString& gameplayId)
{
    return letterPrefix(gameplayId);
}

QString physicsGroup(const QString& physicsType)
{
    QString name = physicsType;
    for (const QLatin1String prefix : {QLatin1String("CO_"), QLatin1String("OBJ_")}) {
        if (name.startsWith(prefix, Qt::CaseInsensitive)) {
            name = name.mid(prefix.size());
            break;
        }
    }
    qsizetype cut = 0;
    while (cut < name.size() && name.at(cut) != QLatin1Char('_') && name.at(cut) != QLatin1Char('.')) {
        ++cut;
    }
    const QString group = name.left(cut);
    return group.isEmpty() ? QStringLiteral("(other)") : group;
}

Layer placements(const QByteArray& data, const QString& layerId, const QString& title, const QString& source,
    const QString& typeAttribute)
{
    Layer layer;
    layer.id = layerId;
    layer.title = title;
    layer.source = source;
    layer.kind = FeatureKind::Point;

    const bool isGameplay = typeAttribute == QLatin1String("GameplayID");
    QXmlStreamReader xml(data);
    int depth = 0;
    Feature current;
    bool havePosition = false;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            ++depth;
            if (depth == 2) {
                havePosition = false;
                const QString type = xml.attributes().value(typeAttribute).toString();
                current.name = type.isEmpty() ? xml.name().toString() : type;
                current.group = isGameplay ? gameplayGroup(type) : physicsGroup(type);
                current.properties.append({QStringLiteral("Element"), xml.name().toString()});
                for (const QXmlStreamAttribute& attribute : xml.attributes()) {
                    current.properties.append({attribute.name().toString(), attribute.value().toString()});
                }
            } else if (depth == 3 && xml.name() == QLatin1String("Pos")) {
                current.position = attributeVector(xml, source, {});
                havePosition = true;
            } else if (depth == 4 && xml.name() == QLatin1String("ZAxis")) {
                current.forward = attributeVector(xml, source, {});
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (depth == 2) {
                if (!havePosition) {
                    failXml(source, xml, QStringLiteral("object %1 has no <Pos>").arg(current.name));
                }
                current.properties.append({QStringLiteral("Position"), formatVector(current.position)});
                if (!current.forward.isNull()) {
                    current.properties.append({QStringLiteral("Forward (Z axis)"), formatVector(current.forward)});
                }
                layer.features.push_back(std::move(current));
                current = Feature();
            }
            --depth;
        }
    }
    checkXml(xml, source);
    return layer;
}

RaceRoute raceRoute(const QByteArray& data, const QString& source)
{
    RaceRoute route;
    route.source = source;
    QXmlStreamReader xml(data);
    QString transformName;
    float width = 0.0F;
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }
        if (xml.name() == QLatin1String("NamedTransform")) {
            transformName = xml.attributes().value(QLatin1String("name")).toString();
            width = xml.attributes().hasAttribute(QLatin1String("width"))
                ? attributeFloat(xml, source, QStringLiteral("width"))
                : 0.0F;
        } else if (xml.name() == QLatin1String("Transform")) {
            RouteTransform transform;
            transform.name = transformName;
            transform.position = attributeVector(xml, source, QStringLiteral("pos."));
            transform.facing = attributeVector(xml, source, QStringLiteral("facing."));
            transform.width = width;
            route.transforms.push_back(std::move(transform));
        }
    }
    checkXml(xml, source);
    return route;
}

void appendTrackRoute(const RaceRoute& route, const QString& routeLabel, Layer& layer)
{
    for (const RouteTransform& transform : route.transforms) {
        Feature feature;
        feature.name = transform.name;
        feature.group = transformKind(transform.name);
        feature.position = transform.position;
        feature.forward = transform.facing;
        feature.properties = {
            {QStringLiteral("Route file"), routeLabel},
            {QStringLiteral("Name"), transform.name},
            {QStringLiteral("Position"), formatVector(transform.position)},
            {QStringLiteral("Facing"), formatVector(transform.facing)},
        };
        if (transform.width > 0.0F) {
            feature.properties.append(
                {QStringLiteral("Width"), QStringLiteral("%1 m").arg(static_cast<double>(transform.width))});
        }
        layer.features.push_back(std::move(feature));
    }
}

Layer particleEmitters(const QByteArray& data, const QString& source)
{
    Layer layer;
    layer.id = QStringLiteral("particles");
    layer.title = QStringLiteral("Particle emitters and trigger zones");
    layer.source = source;
    layer.kind = FeatureKind::Point;

    QXmlStreamReader xml(data);
    int depth = 0;
    Feature current;
    bool havePosition = false;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            ++depth;
            if (depth == 2) {
                havePosition = false;
                current.group = xml.name().toString();
                current.properties.append({QStringLiteral("Kind"), current.group});
            } else if (depth == 3) {
                const QString name = xml.name().toString();
                if (name == QLatin1String("Position")) {
                    current.position = attributeVector(xml, source, QStringLiteral("v."));
                    havePosition = true;
                } else if (xml.attributes().hasAttribute(QLatin1String("value"))) {
                    const QString value = xml.attributes().value(QLatin1String("value")).toString();
                    current.properties.append({name, value});
                    if (name == QLatin1String("Name")) {
                        current.name = value;
                    }
                }
            } else if (depth == 4 && xml.name() == QLatin1String("ZAxis")) {
                current.forward = attributeVector(xml, source, QStringLiteral("v."));
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (depth == 2) {
                if (!havePosition) {
                    failXml(source, xml, QStringLiteral("%1 has no <Position>").arg(current.group));
                }
                if (current.name.isEmpty()) {
                    current.name = current.group;
                }
                current.properties.append({QStringLiteral("Position"), formatVector(current.position)});
                layer.features.push_back(std::move(current));
                current = Feature();
            }
            --depth;
        }
    }
    checkXml(xml, source);
    return layer;
}

Layer postProcessingZones(const QByteArray& data, const QString& source)
{
    Layer layer;
    layer.id = QStringLiteral("ppzones");
    layer.title = QStringLiteral("Post-processing zones");
    layer.source = source;
    layer.kind = FeatureKind::Polygon;

    QXmlStreamReader xml(data);
    Feature current;
    bool inZone = false;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const auto name = xml.name();
            if (name == QLatin1String("PostProcessingZone")) {
                inZone = true;
                current.name = xml.attributes().value(QLatin1String("Name")).toString();
                current.group = current.name;
                for (const QXmlStreamAttribute& attribute : xml.attributes()) {
                    current.properties.append({attribute.name().toString(), attribute.value().toString()});
                }
            } else if (inZone && name == QLatin1String("PostProcessingZoneTriangle")) {
                current.shapes.emplace_back();
            } else if (inZone && name == QLatin1String("PostProcessingZoneTrianglePoint")) {
                if (current.shapes.empty()) {
                    failXml(source, xml, QStringLiteral("triangle point outside a triangle"));
                }
                current.shapes.back().emplace_back(attributeFloat(xml, source, QStringLiteral("posX")), 0.0F,
                    attributeFloat(xml, source, QStringLiteral("posZ")));
            }
        } else if (token == QXmlStreamReader::EndElement && xml.name() == QLatin1String("PostProcessingZone")) {
            inZone = false;
            QVector3D sum;
            int count = 0;
            for (const auto& triangle : current.shapes) {
                if (triangle.size() != 3) {
                    failXml(source, xml,
                        QStringLiteral("zone %1 has a triangle with %2 points").arg(current.name).arg(triangle.size()));
                }
                for (const QVector3D& point : triangle) {
                    sum += point;
                    ++count;
                }
            }
            if (count == 0) {
                continue;
            }
            current.position = sum / static_cast<float>(count);
            current.properties.append({QStringLiteral("Triangles"), QString::number(current.shapes.size())});
            layer.features.push_back(std::move(current));
            current = Feature();
        }
    }
    checkXml(xml, source);
    return layer;
}

Layer navNodes(const QByteArray& data, const QString& source)
{
    // Big-endian: a 10-word header whose second word is the node count, then
    // 32-byte nodes {u32 id, f32 x, y, z, u32 linkCount, u32 firstLink,
    // u32 unknownA, u32 unknownB}. The link counts sum to header word 4.
    constexpr qsizetype kHeaderSize = 40;
    constexpr qsizetype kNodeSize = 32;
    if (data.size() < kHeaderSize) {
        fail(source, QStringLiteral("file is too small (%1 bytes)").arg(data.size()));
    }
    const std::uint32_t nodeCount = be32(data, 4);
    const std::uint32_t linkCount = be32(data, 16);
    if (kHeaderSize + static_cast<qsizetype>(nodeCount) * kNodeSize > data.size()) {
        fail(source, QStringLiteral("header claims %1 nodes but the file holds fewer").arg(nodeCount));
    }

    Layer layer;
    layer.id = QStringLiteral("nav");
    layer.title = QStringLiteral("Road network nodes");
    layer.source = source;
    layer.kind = FeatureKind::Point;
    layer.features.reserve(nodeCount);

    std::uint64_t linkTotal = 0;
    for (std::uint32_t i = 0; i < nodeCount; ++i) {
        const qsizetype offset = kHeaderSize + static_cast<qsizetype>(i) * kNodeSize;
        const std::uint32_t id = be32(data, offset);
        const std::uint32_t links = be32(data, offset + 16);
        linkTotal += links;

        Feature feature;
        feature.position = QVector3D(beFloat(data, offset + 4), beFloat(data, offset + 8), beFloat(data, offset + 12));
        feature.name = QStringLiteral("Node %1").arg(id);
        if (links <= 1) {
            feature.group = QStringLiteral("Dead ends");
        } else if (links == 2) {
            feature.group = QStringLiteral("Road");
        } else {
            feature.group = QStringLiteral("Junctions");
        }
        feature.properties = {
            {QStringLiteral("Node id"), QString::number(id)},
            {QStringLiteral("Links"), QString::number(links)},
            {QStringLiteral("First link index"), QString::number(be32(data, offset + 20))},
            {QStringLiteral("Unknown field (+24)"), QString::number(be32(data, offset + 24))},
            {QStringLiteral("Unknown field (+28)"), QString::number(be32(data, offset + 28))},
            {QStringLiteral("Position"), formatVector(feature.position)},
        };
        layer.features.push_back(std::move(feature));
    }
    if (linkTotal != linkCount) {
        fail(source, QStringLiteral("node link counts sum to %1, header says %2").arg(linkTotal).arg(linkCount));
    }
    return layer;
}

Feature aiRoute(const QByteArray& data, const QString& name)
{
    // Big-endian: "OWTM", u32 version (1), 8 bytes zero, u32 pointCount,
    // u32 isLoop, 8 bytes zero; then 48-byte points starting with f32 x, y, z;
    // then a 16-byte trailer repeating the magic and version.
    constexpr qsizetype kHeaderSize = 32;
    constexpr qsizetype kPointSize = 48;
    constexpr qsizetype kTrailerSize = 16;
    if (data.size() < kHeaderSize || !data.startsWith("OWTM")) {
        fail(name, QStringLiteral("not an OWTM route file"));
    }
    const std::uint32_t version = be32(data, 4);
    if (version != 1) {
        fail(name, QStringLiteral("unsupported route version %1").arg(version));
    }
    const std::uint32_t count = be32(data, 16);
    const bool loop = be32(data, 20) != 0;
    const qsizetype expected = kHeaderSize + static_cast<qsizetype>(count) * kPointSize + kTrailerSize;
    if (data.size() != expected) {
        fail(name, QStringLiteral("%1 points need %2 bytes, file has %3").arg(count).arg(expected).arg(data.size()));
    }
    if (count < 2) {
        fail(name, QStringLiteral("route has %1 points").arg(count));
    }

    Feature feature;
    feature.name = name;
    feature.shapes.emplace_back();
    auto& points = feature.shapes.back();
    points.reserve(count + (loop ? 1 : 0));
    for (std::uint32_t i = 0; i < count; ++i) {
        const qsizetype offset = kHeaderSize + static_cast<qsizetype>(i) * kPointSize;
        points.emplace_back(beFloat(data, offset), beFloat(data, offset + 4), beFloat(data, offset + 8));
    }
    if (loop) {
        points.push_back(points.front());
    }
    feature.position = points.front();

    double length = 0.0;
    for (std::size_t i = 1; i < points.size(); ++i) {
        length += static_cast<double>((points[i] - points[i - 1]).length());
    }
    feature.properties = {
        {QStringLiteral("Points"), QString::number(count)},
        {QStringLiteral("Closed circuit"), loop ? QStringLiteral("yes") : QStringLiteral("no")},
        {QStringLiteral("Length"), QStringLiteral("%1 km").arg(length / 1000.0, 0, 'f', 2)},
        {QStringLiteral("Start"), formatVector(points.front())},
    };
    return feature;
}

} // namespace fh1::loaders
