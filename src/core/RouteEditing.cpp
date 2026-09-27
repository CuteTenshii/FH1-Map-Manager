#include "RouteEditing.h"

#include "Races.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace fh1 {

namespace {

/// How far ahead of the last checkpoint or waypoint a new one goes.
constexpr float kAppendDistance = 50.0F;
/// Exponent digits the game's C library writes.
constexpr qsizetype kExponentDigits = 3;
/// Room for any float written with "%.6g".
constexpr std::size_t kNumberBuffer = 32;
/// Digits of the numbers in route point names ("route_checkpoint_03").
constexpr int kNameNumberDigits = 2;

QByteArray escapeAttribute(const QString& value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('&'), QLatin1String("&amp;"));
    escaped.replace(QLatin1Char('<'), QLatin1String("&lt;"));
    escaped.replace(QLatin1Char('\''), QLatin1String("&apos;"));
    return escaped.toUtf8();
}

QByteArray attribute(const QByteArray& name, const QString& value)
{
    return ' ' + name + "='" + escapeAttribute(value) + '\'';
}

/// A transform written in the file's layout, closing line included.
QByteArray transformText(const RouteTransform& transform, const RouteFile& file)
{
    QByteArray attributes;
    bool haveName = false;
    bool haveWidth = false;
    for (const auto& [name, value] : transform.attributes) {
        if (name == QLatin1String("name")) {
            attributes += attribute("name", transform.name);
            haveName = true;
        } else if (name == QLatin1String("width")) {
            attributes += attribute("width", formatGameFloat(transform.width));
            haveWidth = true;
        } else {
            attributes += attribute(name.toUtf8(), value);
        }
    }
    if (!haveName) {
        attributes.prepend(attribute("name", transform.name));
    }
    if (!haveWidth && transform.width > 0.0F) {
        attributes += attribute("width", formatGameFloat(transform.width));
    }
    const QVector3D& p = transform.position;
    const QVector3D& f = transform.facing;
    QByteArray position;
    position += attribute("pos.x", formatGameFloat(p.x()));
    position += attribute("pos.y", formatGameFloat(p.y()));
    position += attribute("pos.z", formatGameFloat(p.z()));
    position += attribute("facing.x", formatGameFloat(f.x()));
    position += attribute("facing.y", formatGameFloat(f.y()));
    position += attribute("facing.z", formatGameFloat(f.z()));
    return file.indent + "<NamedTransform" + attributes + '>' + file.newline + file.innerIndent + "<Transform"
        + position + "/>" + file.newline + file.indent + "</NamedTransform>" + file.newline;
}

/// The text between the end of the transform before and the line holding
/// this transform's opening tag: blank lines or comments to keep.
QByteArray leadingText(const RouteTransform& transform, const RouteFile& file)
{
    if (transform.sourceStart < 0) {
        return {};
    }
    const qsizetype tag = file.text.indexOf("<NamedTransform", transform.sourceStart);
    if (tag < 0 || tag >= transform.sourceEnd) {
        return {};
    }
    const qsizetype lineStart = file.text.lastIndexOf('\n', tag) + 1;
    return lineStart > transform.sourceStart ? file.text.mid(transform.sourceStart, lineStart - transform.sourceStart)
                                             : QByteArray();
}

/// `name` with its trailing number replaced by `number`, keeping any
/// letters after it ("route_checkpoint_indicator_12b" -> "..._13b").
QString withNumber(const QString& name, int number)
{
    const qsizetype underscore = name.lastIndexOf(QLatin1Char('_'));
    qsizetype end = underscore + 1;
    while (end < name.size() && name.at(end).isDigit()) {
        ++end;
    }
    const auto digits = static_cast<int>(std::max<qsizetype>(end - underscore - 1, kNameNumberDigits));
    return name.left(underscore + 1) + QStringLiteral("%1").arg(number, digits, 10, QLatin1Char('0')) + name.mid(end);
}

/// The kinds numbered along with points of `kind`: a checkpoint's
/// indicators share its number.
bool numberedWith(RoutePointKind kind, RoutePointKind other)
{
    return other == kind || (kind == RoutePointKind::Checkpoint && other == RoutePointKind::CheckpointMarker);
}

/// Renumbers the points of `kind` (and their indicators) numbered above
/// `above` by `step`.
void renumberAbove(RaceRoute& route, RoutePointKind kind, int above, int step)
{
    for (RouteTransform& transform : route.transforms) {
        const int number = routePointNumber(transform.name);
        if (number > above && numberedWith(kind, routePointKind(transform.name))) {
            transform.name = withNumber(transform.name, number + step);
            transform.edited = true;
        }
    }
}

QVector3D level(const QVector3D& direction)
{
    return QVector3D(direction.x(), 0.0F, direction.z()).normalized();
}

bool isInsertable(RoutePointKind kind)
{
    return kind == RoutePointKind::Checkpoint || kind == RoutePointKind::Waypoint;
}

/// The point of `kind` numbered next after `number`, if any.
const RouteTransform* nextPoint(const RaceRoute& route, RoutePointKind kind, int number)
{
    const RouteTransform* next = nullptr;
    int nextNumber = std::numeric_limits<int>::max();
    for (const RouteTransform& transform : route.transforms) {
        const int candidate = routePointNumber(transform.name);
        if (routePointKind(transform.name) == kind && candidate > number && candidate < nextNumber) {
            next = &transform;
            nextNumber = candidate;
        }
    }
    return next;
}

} // namespace

QString formatGameFloat(float value)
{
    // Qt's own formatting drops the sign of negative zero; the game's
    // C library keeps it.
    std::array<char, kNumberBuffer> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.6g", static_cast<double>(value));
    QString text = QString::fromLatin1(buffer.data());
    const qsizetype exponent = text.indexOf(QLatin1Char('e'));
    if (exponent >= 0) {
        const qsizetype digits = text.size() - exponent - 2;
        if (digits < kExponentDigits) {
            text.insert(exponent + 2, QString(kExponentDigits - digits, QLatin1Char('0')));
        }
    } else if (std::isfinite(value) && !text.contains(QLatin1Char('.'))) {
        text += QLatin1String(".0");
    }
    return text;
}

QByteArray writeRaceRoute(const RaceRoute& route)
{
    const RouteFile& file = route.file;
    if (file.text.isEmpty() || file.transformsStart < 0) {
        const QByteArray& nl = file.newline;
        QByteArray out = "<TrackRoute>" + nl + "\t<NamedTransforms>" + nl;
        for (const RouteTransform& transform : route.transforms) {
            out += transformText(transform, file);
        }
        return out + "\t</NamedTransforms>" + nl + "</TrackRoute>" + nl;
    }
    QByteArray out = file.text.left(file.transformsStart);
    for (const RouteTransform& transform : route.transforms) {
        if (!transform.edited && transform.sourceStart >= 0 && transform.sourceEnd >= transform.sourceStart) {
            out += file.text.mid(transform.sourceStart, transform.sourceEnd - transform.sourceStart);
        } else {
            out += leadingText(transform, file) + transformText(transform, file);
        }
    }
    return out + file.text.mid(file.transformsEnd);
}

void moveRoutePoint(RaceRoute& route, std::size_t index, const QVector3D& position)
{
    if (index >= route.transforms.size()) {
        return;
    }
    RouteTransform& moved = route.transforms[index];
    const QVector3D delta = position - moved.position;
    moved.position = position;
    moved.edited = true;
    const RoutePointKind kind = routePointKind(moved.name);
    const int number = routePointNumber(moved.name);
    for (RouteTransform& transform : route.transforms) {
        const RoutePointKind other = routePointKind(transform.name);
        const bool indicator = kind == RoutePointKind::Checkpoint && other == RoutePointKind::CheckpointMarker
            && routePointNumber(transform.name) == number;
        const bool cannon = kind == RoutePointKind::Finish && other == RoutePointKind::FinishCannon;
        if (indicator || cannon) {
            transform.position += delta;
            transform.edited = true;
        }
    }
}

void turnRoutePoint(RaceRoute& route, std::size_t index, const QVector3D& facing)
{
    const QVector3D direction = level(facing);
    if (index >= route.transforms.size() || direction.isNull()) {
        return;
    }
    route.transforms[index].facing = direction;
    route.transforms[index].edited = true;
}

bool canInsertOrRemove(const RaceRoute& route, std::size_t index)
{
    return index < route.transforms.size() && isInsertable(routePointKind(route.transforms[index].name))
        && routePointNumber(route.transforms[index].name) >= 0;
}

std::optional<std::size_t> insertRoutePointAfter(RaceRoute& route, std::size_t index)
{
    if (!canInsertOrRemove(route, index)) {
        return std::nullopt;
    }
    const RouteTransform before = route.transforms[index];
    const RoutePointKind kind = routePointKind(before.name);
    const int number = routePointNumber(before.name);

    RouteTransform added;
    added.name = withNumber(before.name, number + 1);
    added.width = before.width;
    added.edited = true;
    if (const RouteTransform* next = nextPoint(route, kind, number)) {
        added.position = (before.position + next->position) / 2.0F;
        const QVector3D along = level(next->position - before.position);
        added.facing = along.isNull() ? before.facing : along;
    } else {
        added.facing = level(before.facing).isNull() ? before.facing : level(before.facing);
        added.position = before.position + added.facing * kAppendDistance;
    }
    for (const auto& [name, value] : before.attributes) {
        if (name == QLatin1String("name") || name == QLatin1String("width")) {
            added.attributes.append({name, value});
        }
    }

    // The checkpoint's first indicator (not its "b" one) gives where the new
    // checkpoint's indicator goes.
    std::optional<RouteTransform> indicator;
    std::optional<QString> indicatorAfter;
    if (kind == RoutePointKind::Checkpoint) {
        for (const RouteTransform& transform : route.transforms) {
            if (routePointKind(transform.name) == RoutePointKind::CheckpointMarker
                && routePointNumber(transform.name) == number && transform.name.back().isDigit()) {
                RouteTransform marker;
                marker.name = withNumber(transform.name, number + 1);
                marker.position = added.position + (transform.position - before.position);
                marker.facing = added.facing;
                marker.attributes = {{QStringLiteral("name"), marker.name}};
                marker.edited = true;
                indicator = marker;
                indicatorAfter = transform.name;
                break;
            }
        }
    }

    renumberAbove(route, kind, number, 1);
    route.transforms.insert(route.transforms.begin() + static_cast<std::ptrdiff_t>(index) + 1, added);
    if (indicator && indicatorAfter) {
        const std::optional<std::size_t> at = routeTransformIndex(route, *indicatorAfter);
        const std::size_t position = at ? *at + 1 : route.transforms.size();
        route.transforms.insert(route.transforms.begin() + static_cast<std::ptrdiff_t>(position), *indicator);
        if (position <= index) {
            return index + 2;
        }
    }
    return index + 1;
}

bool removeRoutePoint(RaceRoute& route, std::size_t index)
{
    if (!canInsertOrRemove(route, index)) {
        return false;
    }
    const RoutePointKind kind = routePointKind(route.transforms[index].name);
    const int number = routePointNumber(route.transforms[index].name);
    route.transforms.erase(route.transforms.begin() + static_cast<std::ptrdiff_t>(index));
    if (kind == RoutePointKind::Checkpoint) {
        std::erase_if(route.transforms, [number](const RouteTransform& transform) {
            return routePointKind(transform.name) == RoutePointKind::CheckpointMarker
                && routePointNumber(transform.name) == number;
        });
    }
    renumberAbove(route, kind, number, -1);
    return true;
}

bool removeRouteTransform(RaceRoute& route, std::size_t index)
{
    if (canInsertOrRemove(route, index)) {
        return removeRoutePoint(route, index);
    }
    if (index >= route.transforms.size()) {
        return false;
    }
    route.transforms.erase(route.transforms.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

std::optional<std::size_t> routeTransformIndex(const RaceRoute& route, const QString& name)
{
    for (std::size_t i = 0; i < route.transforms.size(); ++i) {
        if (route.transforms[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

bool isRouteEdited(const RaceRoute& route)
{
    std::size_t fromFile = 0;
    for (const RouteTransform& transform : route.transforms) {
        if (transform.edited || transform.sourceStart < 0) {
            return true;
        }
        ++fromFile;
    }
    return fromFile != route.file.transformCount;
}

std::optional<float> roadHeightNear(const MapData& map, float x, float z, float radius)
{
    std::optional<float> height;
    float best = radius;
    for (const Layer& layer : map.layers) {
        if (layer.id != QLatin1String("nav")) {
            continue;
        }
        for (const Feature& node : layer.features) {
            const float distance = std::hypot(node.position.x() - x, node.position.z() - z);
            if (distance <= best) {
                best = distance;
                height = node.position.y();
            }
        }
    }
    return height;
}

QString routeOutputPath(const QString& outputFolder, const QString& mediaPath)
{
    const QDir folder(outputFolder);
    for (const QString& entry : folder.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (entry.compare(QLatin1String("media"), Qt::CaseInsensitive) == 0) {
            return QDir::cleanPath(folder.filePath(entry + QLatin1Char('/') + mediaPath));
        }
    }
    return QDir::cleanPath(folder.filePath(mediaPath));
}

bool saveEditedFile(const QByteArray& contents, const QString& path, const QString& originalPath,
    const QString& backupPath, QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    const QFileInfo target(path);
    if (!QDir().mkpath(target.absolutePath())) {
        return fail(QStringLiteral("cannot create %1").arg(target.absolutePath()));
    }
    const QFileInfo original(originalPath);
    if (target.exists() && original.exists() && target.canonicalFilePath() == original.canonicalFilePath()) {
        if (!QFileInfo::exists(backupPath)
            && (!QDir().mkpath(QFileInfo(backupPath).absolutePath()) || !QFile::copy(path, backupPath))) {
            return fail(QStringLiteral("cannot back up %1 to %2").arg(path, backupPath));
        }
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
    }
    if (file.write(contents) != contents.size() || !file.commit()) {
        return fail(QStringLiteral("cannot write %1: %2").arg(path, file.errorString()));
    }
    return true;
}

} // namespace fh1
