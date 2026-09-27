#pragma once

#include "MapData.h"

#include <QByteArray>
#include <QString>
#include <QVector3D>

#include <cstddef>
#include <optional>

namespace fh1 {

/// A number as the game writes it in its XML files: C's "%.6g" with a
/// three-digit exponent, and ".0" after whole numbers ("-1276.13", "0.0",
/// "-2.8213e-007"). Reproduces every value of Colorado's route files.
QString formatGameFloat(float value);

/// The contents of `route`'s file. Transforms that were not edited keep
/// their text from the file byte for byte, as does everything around them;
/// edited and added ones are written in the file's own layout. A route read
/// from nothing is written whole.
QByteArray writeRaceRoute(const RaceRoute& route);

/// Moves transform `index` to `position`. A checkpoint takes its
/// indicators along, and the finish its cannons.
void moveRoutePoint(RaceRoute& route, std::size_t index, const QVector3D& position);

/// Turns transform `index` to face along `facing`, kept level.
void turnRoutePoint(RaceRoute& route, std::size_t index, const QVector3D& facing);

/// True if points like transform `index` can be added and removed: the
/// checkpoints and the waypoints.
bool canInsertOrRemove(const RaceRoute& route, std::size_t index);

/// Adds a checkpoint or waypoint after transform `index` (one of those),
/// halfway to the next one or, after the last, 50 m ahead, numbering the
/// later ones up by one. A checkpoint gets an indicator placed as the one
/// of transform `index` is. Returns the new transform's index, or nothing
/// when transform `index` is neither kind.
std::optional<std::size_t> insertRoutePointAfter(RaceRoute& route, std::size_t index);

/// Removes checkpoint or waypoint `index`, with a checkpoint's indicators,
/// numbering the later ones down by one. Returns false when transform
/// `index` is neither kind.
bool removeRoutePoint(RaceRoute& route, std::size_t index);

/// Removes transform `index`: a checkpoint or waypoint as removeRoutePoint()
/// does, anything else on its own. Returns false if there is no such
/// transform.
bool removeRouteTransform(RaceRoute& route, std::size_t index);

/// The index of the transform named `name`, or nothing.
std::optional<std::size_t> routeTransformIndex(const RaceRoute& route, const QString& name);

/// True if any transform of `route` was edited, added or removed since it
/// was read.
bool isRouteEdited(const RaceRoute& route);

/// Height of the road network (the "nav" layer's nodes) nearest `x`, `z`
/// within `radius` metres, or nothing.
std::optional<float> roadHeightNear(const MapData& map, float x, float z, float radius);

/// Where an edited file goes under `outputFolder`: its media path below
/// the folder's `media` subfolder when it has one (a copy of the disc),
/// otherwise below the folder itself (a copy of `media`).
QString routeOutputPath(const QString& outputFolder, const QString& mediaPath);

/// Gets `path` ready to be written: creates its folder and, when `path` is
/// the game's own file, `originalPath`, copies the original to `backupPath`
/// unless a backup is already there. Returns false, with `error` set, if
/// that fails.
bool prepareEditedFile(const QString& path, const QString& originalPath, const QString& backupPath, QString* error);

/// Writes `contents` to `path`. When `path` is the game's own file,
/// `originalPath`, the original is first copied to `backupPath`, unless a
/// backup is already there; keeping backups out of the game folder leaves
/// it holding only the files the game expects. Returns false, with `error`
/// set, if a file cannot be written.
bool saveEditedFile(const QByteArray& contents, const QString& path, const QString& originalPath,
    const QString& backupPath, QString* error);

} // namespace fh1
