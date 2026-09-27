#pragma once

#include <QByteArray>
#include <QString>
#include <QVector3D>

#include <array>
#include <cstddef>
#include <vector>

namespace fh1 {

/// One object of a track's Ribbon_NN/GameObjs.xml: a gameplay marker such as
/// an event start, a barn find or a speed camera.
struct GameObject {
    /// Its element name, "Obj17"; the game numbers them from 0 in order.
    QString element;
    QString gameplayId;
    QVector3D position;
    /// The `<Orientation>` basis: X, Y and Z axes; Z is where it faces.
    std::array<QVector3D, 3> axes{};
    /// Where the object's text lies in GameObjectsFile::text, from the end
    /// of the one before it to the end of its closing line.
    qsizetype sourceStart = -1;
    qsizetype sourceEnd = -1;
    /// Moved or turned since it was read.
    bool edited = false;
};

/// A GameObjs.xml as read, so that saving keeps what was not edited byte
/// for byte.
struct GameObjectsFile {
    /// The file's path under the media folder, as the disc spells it.
    QString mediaPath;
    QByteArray text;
    /// Where the first object's text starts and the last one's ends.
    qsizetype objectsStart = -1;
    qsizetype objectsEnd = -1;
    std::vector<GameObject> objects;
};

/// Parses GameObjs.xml: `<ObjN GameplayID>` elements holding a `<Pos>` and
/// an `<Orientation>` with `<XAxis>`, `<YAxis>` and `<ZAxis>`, each with x,
/// y and z attributes. Throws LoadError when the data is malformed.
GameObjectsFile readGameObjects(const QByteArray& data, const QString& source);

/// The contents of the file. Objects kept as they were keep their text
/// byte for byte; moved or turned ones get new numbers, written as the game
/// does ("%.6f"), and objects after a removed one are renamed so the
/// numbering stays unbroken, as in the game's files.
QByteArray writeGameObjects(const GameObjectsFile& file);

/// Moves object `index` to `position`.
void moveGameObject(GameObjectsFile& file, std::size_t index, const QVector3D& position);

/// Turns object `index` about the vertical so that it faces along
/// `facing`, keeping any tilt it has.
void turnGameObject(GameObjectsFile& file, std::size_t index, const QVector3D& facing);

/// Removes object `index`.
void removeGameObject(GameObjectsFile& file, std::size_t index);

/// The objects that belong with object `index` (itself included, first):
/// those whose IDs name the same thing once a role suffix is dropped, and
/// that lie within `radius` metres of it. The suffixes are roles such as
/// "_NODE", "_L", "_right", "_DISCOVERY", "_OPEN" or "_CLOSEDC", and a
/// number when the ID without it is itself an object's: "FR04_07" and
/// "FR04_NODE" go with "FR04", "speed_camera_30_left" with
/// "speed_camera_30_right", but "flyer_07" stands alone because there is
/// no "flyer". Barn finds are named both "BF_<car>" and "BARNFIND_<car>".
/// Inferred from the names in Colorado's GameObjs.xml.
std::vector<std::size_t> gameObjectGroup(const GameObjectsFile& file, std::size_t index, float radius);

/// Turns the objects `indices` together about the vertical through
/// `pivot` by `radians`, counterclockwise seen from above with X east and
/// Z north: their positions around the pivot and their axes.
void turnGameObjects(
    GameObjectsFile& file, const std::vector<std::size_t>& indices, const QVector3D& pivot, float radians);

} // namespace fh1
