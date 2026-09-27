#pragma once

#include <QHash>
#include <QString>

#include <optional>
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
    /// Naming parts of a Data_Car row, or nothing if the id is unknown.
    std::optional<Car> car(const QString& carId);

private:
    QString m_connection;
    bool m_open = false;
    QString m_error;
};

} // namespace fh1
