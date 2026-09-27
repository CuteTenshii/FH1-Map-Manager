#pragma once

#include "MapData.h"

#include <QByteArray>
#include <QHash>
#include <QString>

#include <optional>
#include <utility>
#include <vector>

namespace fh1 {

/// Read-only queries against media/db/gamedb.slt (SQLite). Text columns such
/// as names hold string-table references ("_&123"); see StringTables.
///
/// A QSqlDatabase connection belongs to the thread that opened it, so an
/// instance must be used on one thread only.
class GameDatabase {
public:
    struct Route {
        QString devName;
        QString displayName;
    };
    struct Event {
        QString name;
        QString shortName;
        QString description;
    };
    /// A race event, joined from Events, Races, Tracks, CareerEventTypes and
    /// CarClasses. Text columns are string-table references.
    struct RaceRow {
        QString eventId;
        /// Events.Name (Events.str).
        QString name;
        /// CareerEventTypes.Name (CareerEventTypes.str).
        QString type;
        /// CarClasses.DisplayName (CarClasses.str).
        QString carClass;
        /// Tracks.DisplayName (Tracks.str).
        QString routeName;
        int routeId = -1;
        int laps = 1;
        int length = 0;
        int prize = 0;
        /// Events.Id and Races.Id, which edits are written back to.
        int eventRow = -1;
        int raceRow = -1;
        /// Events.NumberOfDrivers: the AI cars racing the player.
        int opponents = 0;
        /// Events.TimeOfDayStart, in seconds after midnight.
        int timeOfDay = 0;
        /// Events.TargetClass, a CarClasses.Id.
        int carClassId = -1;
    };
    /// A car class: CarClasses.Id and its DisplayName reference
    /// (CarClasses.str).
    struct CarClass {
        int id = -1;
        QString name;
    };
    /// A car's naming parts; the game shows "<year> <make> <model>".
    struct Car {
        int year = 0;
        /// List_CarMake.DisplayName reference (List_CarMake.str).
        QString make;
        /// Data_Car.DisplayName reference (Data_Car.str): the model only.
        QString model;
    };

    GameDatabase();
    ~GameDatabase();
    GameDatabase(const GameDatabase&) = delete;
    GameDatabase& operator=(const GameDatabase&) = delete;

    bool open(const QString& path);
    bool isOpen() const { return m_open; }
    QString errorString() const { return m_error; }

    /// Routes of the track whose MediaName is `mediaName`, by RouteId.
    QHash<int, Route> routes(const QString& mediaName);
    /// Career events by HorizonEventID (e.g. "FR05").
    QHash<QString, Event> events();
    /// The race events run on the track whose MediaName is `mediaName`, in
    /// event order; the free-roam session (CareerEventStyle 0) is left out.
    std::vector<RaceRow> races(const QString& mediaName);
    /// Every car class, by id.
    std::vector<CarClass> carClasses();
    /// Rows of other tables that refer to event `eventRow` (an Events.Id),
    /// by table, for the tables that have any (see eventReferenceColumns()).
    std::vector<std::pair<QString, int>> eventReferences(int eventRow);
    /// Naming parts of a Data_Car row, or nothing if the id is unknown.
    std::optional<Car> car(const QString& carId);

private:
    QString m_connection;
    bool m_open = false;
    QString m_error;
};

/// The tables and columns of gamedb.slt that refer to an event by its
/// Events.Id: its race, AI participants, recommended cars, restrictions,
/// showroom challenges, colours, music, prizes, unlocks, hub and saved
/// progress.
const std::vector<std::pair<QString, QString>>& eventReferenceColumns();

/// The database file at `path` with the settings of `races` (laps,
/// opponents, prize, car class, start time) written into their Events and
/// Races rows, for each race that differs from the same race in `original`,
/// and with the events of `deleted` removed along with every row that
/// refers to them (see eventReferenceColumns()); the file itself is left
/// as it is. Returns nothing, with `error` set, if the copy cannot be made
/// or updated.
std::optional<QByteArray> writeRaceSettings(const QString& path, const std::vector<Race>& races,
    const std::vector<Race>& original, const std::vector<Race>& deleted, QString* error);

} // namespace fh1
