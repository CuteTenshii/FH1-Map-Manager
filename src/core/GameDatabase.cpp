#include "GameDatabase.h"

#include <QAtomicInteger>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>

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
        "t.Length, e.CashPrize, e.Id, r.Id, e.NumberOfDrivers, e.TimeOfDayStart, e.TargetClass, e.Level FROM Races r "
        "JOIN "
        "Events e ON e.Id = r.EventId JOIN Tracks t ON t.id = r.TrackId "
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
        row.eventRow = query.value(9).toInt();
        row.raceRow = query.value(10).toInt();
        row.opponents = query.value(11).toInt();
        row.timeOfDay = query.value(12).toInt();
        row.carClassId = query.value(13).toInt();
        row.level = query.value(14).toInt();
        result.push_back(std::move(row));
    }
    return result;
}

std::vector<GameDatabase::CarClass> GameDatabase::carClasses()
{
    std::vector<CarClass> result;
    if (!m_open) {
        return result;
    }
    QSqlQuery query(QSqlDatabase::database(m_connection, false));
    if (!query.exec(QStringLiteral("SELECT Id, DisplayName FROM CarClasses ORDER BY Id"))) {
        m_error = QStringLiteral("car class query failed: %1").arg(query.lastError().text());
        return result;
    }
    while (query.next()) {
        result.push_back({query.value(0).toInt(), query.value(1).toString()});
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

const std::vector<std::pair<QString, QString>>& eventReferenceColumns()
{
    static const std::vector<std::pair<QString, QString>> columns{
        {QStringLiteral("Races"), QStringLiteral("EventId")},
        {QStringLiteral("EventParticipants"), QStringLiteral("EventID")},
        {QStringLiteral("EventRecommendedCars"), QStringLiteral("EventID")},
        {QStringLiteral("EventRestrictions"), QStringLiteral("EventID")},
        {QStringLiteral("EventShowroomChallenges"), QStringLiteral("EventID")},
        {QStringLiteral("EventUIColors"), QStringLiteral("EventId")},
        {QStringLiteral("Event_Music"), QStringLiteral("EventId")},
        {QStringLiteral("EventHubInitialEvents"), QStringLiteral("EventId")},
        {QStringLiteral("Rewards_EventPrizes"), QStringLiteral("EventId")},
        {QStringLiteral("Rewards_EventUnlock"), QStringLiteral("EventId")},
        {QStringLiteral("Rewards_EventUnlock"), QStringLiteral("UnlockEventId")},
        {QStringLiteral("NewProfile_CareerEventUnlocks"), QStringLiteral("EventId")},
        {QStringLiteral("NewProfile_Career_EventResults"), QStringLiteral("EventId")},
    };
    return columns;
}

std::vector<std::pair<QString, int>> GameDatabase::eventReferences(int eventRow)
{
    std::vector<std::pair<QString, int>> result;
    if (!m_open) {
        return result;
    }
    const QSqlDatabase db = QSqlDatabase::database(m_connection, false);
    const QStringList tables = db.tables();
    for (const auto& [table, column] : eventReferenceColumns()) {
        if (!tables.contains(table)) {
            continue;
        }
        QSqlQuery query(db);
        query.prepare(QStringLiteral("SELECT count(*) FROM %1 WHERE %2 = ?").arg(table, column));
        query.addBindValue(eventRow);
        if (query.exec() && query.next() && query.value(0).toInt() > 0) {
            // Two columns of one table add up.
            if (!result.empty() && result.back().first == table) {
                result.back().second += query.value(0).toInt();
            } else {
                result.emplace_back(table, query.value(0).toInt());
            }
        }
    }
    return result;
}

std::optional<QByteArray> writeRaceSettings(const QString& path, const std::vector<Race>& races,
    const std::vector<Race>& original, const std::vector<Race>& deleted, QString* error)
{
    const auto fail = [error](const QString& message) -> std::optional<QByteArray> {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };
    const QTemporaryDir dir;
    const QString copy = dir.filePath(QStringLiteral("gamedb.slt"));
    if (!dir.isValid() || !QFile::copy(path, copy)) {
        return fail(QStringLiteral("cannot copy %1 to edit it").arg(path));
    }
    static QAtomicInteger<int> counter;
    const QString connection = QStringLiteral("fh1-gamedb-edit-%1").arg(counter.fetchAndAddRelaxed(1));
    QString failure;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(copy);
        if (!db.open()) {
            failure = QStringLiteral("cannot open %1: %2").arg(copy, db.lastError().text());
        } else {
            db.transaction();
            QSqlQuery event(db);
            event.prepare(QStringLiteral("UPDATE Events SET NumberOfDrivers = ?, TimeOfDayStart = ?, "
                                         "TargetClass = ?, CashPrize = ? WHERE Id = ?"));
            QSqlQuery race(db);
            race.prepare(QStringLiteral("UPDATE Races SET NumLaps = ? WHERE Id = ?"));
            for (std::size_t i = 0; i < races.size() && i < original.size() && failure.isEmpty(); ++i) {
                const Race& r = races[i];
                const Race& o = original[i];
                if (r.opponents != o.opponents || r.timeOfDay != o.timeOfDay || r.carClassId != o.carClassId
                    || r.prize != o.prize) {
                    for (const int value : {r.opponents, r.timeOfDay, r.carClassId, r.prize, r.eventRow}) {
                        event.addBindValue(value);
                    }
                    if (!event.exec()) {
                        failure = QStringLiteral("cannot update event %1: %2").arg(r.eventId, event.lastError().text());
                    }
                }
                if (failure.isEmpty() && r.laps != o.laps) {
                    race.addBindValue(r.laps);
                    race.addBindValue(r.raceRow);
                    if (!race.exec()) {
                        failure = QStringLiteral("cannot update race %1: %2").arg(r.eventId, race.lastError().text());
                    }
                }
            }
            const QStringList tables = db.tables();
            for (const Race& gone : deleted) {
                for (const auto& [table, column] : eventReferenceColumns()) {
                    if (!failure.isEmpty() || !tables.contains(table)) {
                        continue;
                    }
                    QSqlQuery remove(db);
                    remove.prepare(QStringLiteral("DELETE FROM %1 WHERE %2 = ?").arg(table, column));
                    remove.addBindValue(gone.eventRow);
                    if (!remove.exec()) {
                        failure = QStringLiteral("cannot delete %1 from %2: %3")
                                      .arg(gone.eventId, table, remove.lastError().text());
                    }
                }
                QSqlQuery remove(db);
                remove.prepare(QStringLiteral("DELETE FROM Events WHERE Id = ?"));
                remove.addBindValue(gone.eventRow);
                if (failure.isEmpty() && !remove.exec()) {
                    failure = QStringLiteral("cannot delete event %1: %2").arg(gone.eventId, remove.lastError().text());
                }
            }
            if (failure.isEmpty() && !db.commit()) {
                failure = QStringLiteral("cannot write %1: %2").arg(copy, db.lastError().text());
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    if (!failure.isEmpty()) {
        return fail(failure);
    }
    QFile file(copy);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("cannot read %1: %2").arg(copy, file.errorString()));
    }
    return file.readAll();
}

} // namespace fh1
