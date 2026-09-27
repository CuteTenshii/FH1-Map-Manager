#include "GameDatabase.h"

#include <QAtomicInteger>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

namespace fh1 {

GameDatabase::GameDatabase()
{
    static QAtomicInteger<int> counter;
    m_connection = QStringLiteral("fh1-gamedb-%1").arg(counter.fetchAndAddRelaxed(1));
}

GameDatabase::~GameDatabase()
{
    if (QSqlDatabase::contains(m_connection)) {
        QSqlDatabase::database(m_connection, false).close();
        QSqlDatabase::removeDatabase(m_connection);
    }
}

bool GameDatabase::open(const QString& path)
{
    QSqlDatabase db = QSqlDatabase::contains(m_connection)
        ? QSqlDatabase::database(m_connection, false)
        : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    db.setDatabaseName(path);
    db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    m_open = db.open();
    if (!m_open) {
        m_error = QStringLiteral("cannot open %1: %2").arg(path, db.lastError().text());
    }
    return m_open;
}

QHash<int, GameDatabase::Route> GameDatabase::routes(const QString& mediaName)
{
    QHash<int, Route> result;
    if (!m_open) {
        return result;
    }
    QSqlQuery query(QSqlDatabase::database(m_connection, false));
    query.prepare(QStringLiteral("SELECT RouteId, DevName, DisplayName FROM Tracks "
                                 "WHERE lower(MediaName) = lower(?) ORDER BY id"));
    query.addBindValue(mediaName);
    if (!query.exec()) {
        m_error = QStringLiteral("route query failed: %1").arg(query.lastError().text());
        return result;
    }
    while (query.next()) {
        const int routeId = query.value(0).toInt();
        if (!result.contains(routeId)) {
            result.insert(routeId, {query.value(1).toString(), query.value(2).toString()});
        }
    }
    return result;
}

QHash<QString, GameDatabase::Event> GameDatabase::events()
{
    QHash<QString, Event> result;
    if (!m_open) {
        return result;
    }
    QSqlQuery query(QSqlDatabase::database(m_connection, false));
    if (!query.exec(QStringLiteral("SELECT HorizonEventID, Name, ShortName, Description FROM Events"))) {
        m_error = QStringLiteral("event query failed: %1").arg(query.lastError().text());
        return result;
    }
    while (query.next()) {
        const QString id = query.value(0).toString();
        if (!id.isEmpty() && !result.contains(id)) {
            result.insert(id, {query.value(1).toString(), query.value(2).toString(), query.value(3).toString()});
        }
    }
    return result;
}

std::vector<GameDatabase::RaceRow> GameDatabase::races(const QString& mediaName)
{
    std::vector<RaceRow> result;
    if (!m_open) {
        return result;
    }
    QSqlQuery query(QSqlDatabase::database(m_connection, false));
    query.prepare(QStringLiteral(
        "SELECT e.HorizonEventID, e.Name, ty.Name, cl.DisplayName, t.DisplayName, t.RouteId, r.NumLaps, "
        "t.Length, e.CashPrize FROM Races r JOIN Events e ON e.Id = r.EventId JOIN Tracks t ON t.id = r.TrackId "
        "LEFT JOIN CareerEventTypes ty ON ty.id = e.CareerTypeId LEFT JOIN CarClasses cl ON cl.Id = e.TargetClass "
        "WHERE lower(t.MediaName) = lower(?) AND e.CareerEventStyle <> 0 ORDER BY e.Id, r.RaceNumber"));
    query.addBindValue(mediaName);
    if (!query.exec()) {
        m_error = QStringLiteral("race query failed: %1").arg(query.lastError().text());
        return result;
    }
    while (query.next()) {
        RaceRow row;
        row.eventId = query.value(0).toString();
        row.name = query.value(1).toString();
        row.type = query.value(2).toString();
        row.carClass = query.value(3).toString();
        row.routeName = query.value(4).toString();
        row.routeId = query.value(5).toInt();
        row.laps = query.value(6).toInt();
        row.length = query.value(7).toInt();
        row.prize = query.value(8).toInt();
        result.push_back(std::move(row));
    }
    return result;
}

std::optional<GameDatabase::Car> GameDatabase::car(const QString& carId)
{
    bool ok = false;
    const int id = carId.toInt(&ok);
    if (!m_open || !ok) {
        return std::nullopt;
    }
    QSqlQuery query(QSqlDatabase::database(m_connection, false));
    query.prepare(QStringLiteral("SELECT c.Year, m.DisplayName, c.DisplayName FROM Data_Car c "
                                 "LEFT JOIN List_CarMake m ON m.Id = c.MakeID WHERE c.Id = ?"));
    query.addBindValue(id);
    if (!query.exec() || !query.next()) {
        return std::nullopt;
    }
    return Car{query.value(0).toInt(), query.value(1).toString(), query.value(2).toString()};
}

} // namespace fh1
