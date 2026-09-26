#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QString>
#include <QStringList>

#include <vector>

namespace fh1 {

/// One `<Activity>` from a gamemodes.zip config, such as a gas station, a
/// barn find or a career event activation point.
struct Activity {
    QString type;
    QString name;
    /// The config file it came from, e.g. "Colorado/gas_stations.xml".
    QString file;
    /// `mapTag` of its trigger zone: the in-game map's icon category, if set.
    QString mapTag;
    /// GameObjs IDs its `<TriggerZone object=...>` elements place it at: the
    /// spot the player drives into to be offered the activity.
    QStringList triggerObjects;
    /// `radius` of each trigger zone, in metres, by trigger object ID.
    QHash<QString, QString> triggerRadius;
    /// Objects named by `<Behaviour id="CPlaceCarAtObject" object_name=...>`:
    /// where the game puts the player's car when the activity starts, e.g. the
    /// "FR08_NODE" of a race event or gas station.
    QStringList carPlacementObjects;
    /// Every other attribute value in the activity, used to find objects it
    /// refers to, such as a barn find's open and closed door models.
    QStringList referencedValues;
    /// `<UnlockCar id=...>`: the car a barn find awards.
    QString unlockCarId;
};

/// Map-marker metadata derived from the configs and the map render profile.
namespace activities {

/// Parses every `<Activity>` element of one config file. Throws LoadError on
/// malformed XML.
std::vector<Activity> parse(const QByteArray& xml, const QString& file);

/// The in-game map icon category (an `activity_type` in
/// ui/MapProfileFullscreen.xml) for an activity, or an empty string when the
/// map shows none. `eventId` is the Horizon event ID a career event
/// activation belongs to.
QString iconCategory(const Activity& activity, const QString& eventId);

/// A readable, plural name for an icon category or, when that is empty, for
/// the activity type.
QString categoryTitle(const QString& iconCategory, const QString& activityType);

/// Icon images keyed by `activity_type`, built from the map render profile and
/// the icon sheet it references. Each icon composites the atlas cells of its
/// base symbolizers (types icon_up and icon_up_career) in sort order. Throws
/// LoadError on malformed XML.
QHash<QString, QImage> buildIcons(const QByteArray& profileXml, const QImage& iconSheet);

} // namespace activities
} // namespace fh1
