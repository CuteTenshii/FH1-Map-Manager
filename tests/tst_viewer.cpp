#include "EditHistoryPanel.h"
#include "EditSession.h"
#include "FeatureTableModel.h"
#include "GameObjects.h"
#include "LayerItem.h"
#include "Loaders.h"
#include "MainWindow.h"
#include "MapView.h"
#include "RaceTableModel.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QMessageBox>
#include <QSettings>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeWidget>

#include <array>
#include <functional>
#include <map>

namespace {

fh1::Feature point(const QString& name, const QString& group, float x, float z)
{
    fh1::Feature feature;
    feature.name = name;
    feature.group = group;
    feature.position = QVector3D(x, 0.0F, z);
    return feature;
}

/// Answers the next modal message box by clicking the button labelled
/// `button`, as soon as the box is open.
void answerNextMessageBox(const QString& button)
{
    auto* poll = new QTimer(qApp);
    QObject::connect(poll, &QTimer::timeout, poll, [poll, button] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box == nullptr) {
            return;
        }
        for (QAbstractButton* candidate : box->buttons()) {
            if (candidate->text().remove(QLatin1Char('&')) == button) {
                poll->deleteLater();
                candidate->click();
                return;
            }
        }
    });
    poll->start(10);
}

/// A game folder with one track, "testbed", whose database has one race on
/// route 5: a start slot and two checkpoints. Its gameplay objects are
/// `placed`, by ID and X, in a row along Z = 0.
using PlacedObjects = std::vector<std::pair<const char*, int>>;

bool writeRaceInstall(
    const QString& root, const PlacedObjects& placed = {{"FR98", 50}, {"FR98_NODE", 60}, {"flyer_001", -50}})
{
    const QString ribbon = root + QStringLiteral("/media/tracks/testbed/Ribbon_00");
    if (!QDir().mkpath(ribbon) || !QDir().mkpath(root + QStringLiteral("/media/db"))) {
        return false;
    }
    QFile route(ribbon + QStringLiteral("/TrackRoute005.xml"));
    if (!route.open(QIODevice::WriteOnly)) {
        return false;
    }
    route.write("<TrackRoute>\r\n\t<NamedTransforms>\r\n"
                "\t\t<NamedTransform name='route_checkpoint_00' width='20.0'>\r\n"
                "\t\t\t<Transform pos.x='0.0' pos.y='5.0' pos.z='0.0' facing.x='0.0' facing.y='0.0' "
                "facing.z='1.0'/>\r\n\t\t</NamedTransform>\r\n"
                "\t\t<NamedTransform name='route_checkpoint_01' width='20.0'>\r\n"
                "\t\t\t<Transform pos.x='0.0' pos.y='5.0' pos.z='100.0' facing.x='0.0' facing.y='0.0' "
                "facing.z='1.0'/>\r\n\t\t</NamedTransform>\r\n"
                "\t\t<NamedTransform name='start_location_00'>\r\n"
                "\t\t\t<Transform pos.x='0.0' pos.y='5.0' pos.z='-100.0' facing.x='0.0' facing.y='0.0' "
                "facing.z='1.0'/>\r\n\t\t</NamedTransform>\r\n"
                "\t</NamedTransforms>\r\n</TrackRoute>\r\n");
    route.close();

    // By default a race start with its node 10 m east, and a flyer on its
    // own. That race has no event in the database, so its markers are not
    // in the "Event objects" group, which the map hides at first.
    QFile objects(ribbon + QStringLiteral("/GameObjs.xml"));
    if (!objects.open(QIODevice::WriteOnly)) {
        return false;
    }
    objects.write("<?xml version=\"1.0\" ?>\r\n<GameObjs>\r\n");
    for (std::size_t i = 0; i < placed.size(); ++i) {
        objects.write(QStringLiteral("\t<Obj%1 GameplayID=\"%2\">\r\n"
                                     "\t\t<Pos x=\"%3.000000\" y=\"5.000000\" z=\"0.000000\"/>\r\n"
                                     "\t\t<Orientation>\r\n"
                                     "\t\t\t<XAxis x=\"1.000000\" y=\"0.000000\" z=\"0.000000\"/>\r\n"
                                     "\t\t\t<YAxis x=\"0.000000\" y=\"1.000000\" z=\"0.000000\"/>\r\n"
                                     "\t\t\t<ZAxis x=\"0.000000\" y=\"0.000000\" z=\"1.000000\"/>\r\n"
                                     "\t\t</Orientation>\r\n\t</Obj%1>\r\n")
                .arg(i)
                .arg(QLatin1String(placed[i].first))
                .arg(placed[i].second)
                .toLatin1());
    }
    objects.write("</GameObjs>\r\n");
    objects.close();

    const QString connection = QStringLiteral("race-install");
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(root + QStringLiteral("/media/db/gamedb.slt"));
        ok = db.open();
        QSqlQuery query(db);
        const char* const createEvents = "CREATE TABLE Events (Id, Name, ShortName, Description, CareerTypeId, "
                                         "TargetClass, CashPrize, HorizonEventID, CareerEventStyle, "
                                         "NumberOfDrivers, TimeOfDayStart, Level)";
        for (const char* statement : {
                 "CREATE TABLE Tracks (id, DisplayName, MediaName, Length, RouteId, DevName)",
                 createEvents,
                 "CREATE TABLE Races (Id, EventId, RaceNumber, TrackId, NumLaps)",
                 "CREATE TABLE CareerEventTypes (id, Name)",
                 "CREATE TABLE CarClasses (Id, DisplayName)",
                 "INSERT INTO Tracks VALUES (1005, 'Test Loop', 'testbed', 1200, 5, 'TEST_LOOP')",
                 "INSERT INTO Events VALUES (1, 'Test Race', '', '', 1, 0, 1000, 'FR99', 1, 7, 36000, 0)",
                 "INSERT INTO Races VALUES (1, 1, 1, 1005, 2)",
                 "INSERT INTO CareerEventTypes VALUES (1, 'Circuit')",
                 "INSERT INTO CarClasses VALUES (0, 'D')",
             }) {
            ok = ok && query.exec(QString::fromLatin1(statement));
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

/// 1 image pixel per metre, Z not flipped: scene coordinates equal world X/Z.
const fh1::MapCalibration kIdentity{1.0, 0.0, 1.0, 0.0};

} // namespace

class TestViewer : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Keeps the cache test away from the user's own cache.
        QStandardPaths::setTestModeEnabled(true);
    }

    void clearCacheDeletesWorldIndexes()
    {
        const QString directory
            = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/world");
        QVERIFY(QDir().mkpath(directory));
        const QStringList indexes{QStringLiteral("colorado.index"), QStringLiteral("coloradodirt.index")};
        for (const QString& name : indexes + QStringList{QStringLiteral("notes.txt")}) {
            QFile file(QDir(directory).filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("cached");
        }

        MainWindow window;
        QAction* clearCache = nullptr;
        for (QAction* action : window.findChildren<QAction*>()) {
            if (action->text().startsWith(QLatin1String("Clear &Cache"))) {
                clearCache = action;
            }
        }
        QVERIFY(clearCache != nullptr);

        // Cancelling keeps everything.
        answerNextMessageBox(QStringLiteral("Cancel"));
        clearCache->trigger();
        for (const QString& name : indexes) {
            QVERIFY(QFile::exists(QDir(directory).filePath(name)));
        }

        answerNextMessageBox(QStringLiteral("Delete"));
        clearCache->trigger();
        for (const QString& name : indexes) {
            QVERIFY(!QFile::exists(QDir(directory).filePath(name)));
        }
        // Only index files are the viewer's to delete.
        QVERIFY(QFile::exists(QDir(directory).filePath(QStringLiteral("notes.txt"))));

        // With nothing cached the action says so and deletes nothing.
        answerNextMessageBox(QStringLiteral("OK"));
        clearCache->trigger();
        QVERIFY(QFile::remove(QDir(directory).filePath(QStringLiteral("notes.txt"))));
    }

    void eventPropsChoiceStaysInStep()
    {
        QSettings settings;
        settings.remove(QStringLiteral("view/eventProps"));
        // The on/off switch this choice replaced becomes "All events".
        settings.setValue(QStringLiteral("view/showEventProps"), true);

        MainWindow window;
        std::map<QString, QAction*> actions;
        for (QAction* action : window.findChildren<QAction*>()) {
            actions[action->text().remove(QLatin1Char('&'))] = action;
        }
        QComboBox* combo = nullptr;
        for (QComboBox* candidate : window.findChildren<QComboBox*>()) {
            if (candidate->findText(QStringLiteral("All events")) >= 0) {
                combo = candidate;
            }
        }
        QVERIFY(combo != nullptr);
        QVERIFY(actions.count(QStringLiteral("All Events")) == 1);
        QVERIFY(actions[QStringLiteral("All Events")]->isChecked());
        QCOMPARE(combo->currentText(), QStringLiteral("All events"));
        QCOMPARE(settings.value(QStringLiteral("view/eventProps")).toString(), QStringLiteral("all"));
        QVERIFY(!settings.contains(QStringLiteral("view/showEventProps")));

        // Without a race, the race choice says so and cannot be picked.
        QAction* selectedRace = actions[QStringLiteral("Selected Race (none selected)")];
        QVERIFY(selectedRace != nullptr);
        QVERIFY(!selectedRace->isEnabled());
        QCOMPARE(combo->itemText(1), QStringLiteral("Selected race (none selected)"));
        const auto* items = qobject_cast<const QStandardItemModel*>(combo->model());
        QVERIFY(items != nullptr);
        QVERIFY(!items->item(1)->isEnabled());

        // The menu and the Events panel follow each other.
        actions[QStringLiteral("None")]->trigger();
        QCOMPARE(combo->currentText(), QStringLiteral("None"));
        combo->activated(1);
        QVERIFY(selectedRace->isChecked());
        QCOMPARE(settings.value(QStringLiteral("view/eventProps")).toString(), QStringLiteral("race"));
        settings.remove(QStringLiteral("view/eventProps"));
    }

    void routeEditingOnTheMap()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeRaceInstall(dir.filePath(QStringLiteral("disc"))));
        const QString output = dir.filePath(QStringLiteral("edited"));
        QSettings().setValue(QStringLiteral("edit/outputFolder"), output);

        MainWindow window;
        window.resize(1200, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.openGameFolder(dir.filePath(QStringLiteral("disc"))));

        QTableView* races = nullptr;
        for (QTableView* table : window.findChildren<QTableView*>()) {
            if (table->model()->headerData(0, Qt::Horizontal).toString() == QLatin1String("Event")) {
                races = table;
            }
        }
        QVERIFY(races != nullptr);
        QTRY_COMPARE(races->model()->rowCount(), 1);
        races->selectionModel()->select(
            races->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

        std::map<QString, QAction*> actions;
        for (QAction* action : window.findChildren<QAction*>()) {
            actions[action->text().remove(QLatin1Char('&'))] = action;
        }
        QAction* editRoute = actions[QStringLiteral("Edit on Map")];
        QAction* saveRoute = actions[QStringLiteral("Save Edits")];
        QVERIFY(editRoute != nullptr && saveRoute != nullptr);
        QVERIFY(editRoute->isEnabled());
        QVERIFY(!saveRoute->isEnabled());
        editRoute->setChecked(true);

        // Drag checkpoint 01 (world 0, 100) 30 m east. The track has no map
        // image, so a scene unit is a metre with Z pointing up the screen.
        auto* view = window.findChild<MapView*>();
        QVERIFY(view != nullptr);
        const QPoint from = view->mapFromScene(QPointF(0.0, -100.0));
        const QPoint to = view->mapFromScene(QPointF(30.0, -100.0));
        QTest::mousePress(view->viewport(), Qt::LeftButton, {}, from);
        QTest::mouseMove(view->viewport(), (from + to) / 2);
        QTest::mouseMove(view->viewport(), to);
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, {}, to);
        QVERIFY(saveRoute->isEnabled());
        QVERIFY(window.isWindowModified());
        QVERIFY(races->model()->data(races->model()->index(0, 0)).toString().endsWith(QLatin1String(" *")));

        // Undoing leaves nothing to save; redoing brings the edit back.
        QAction* undo = nullptr;
        QAction* redo = nullptr;
        for (auto& [text, action] : actions) {
            if (text.startsWith(QLatin1String("Undo"))) {
                undo = action;
            } else if (text.startsWith(QLatin1String("Redo"))) {
                redo = action;
            }
        }
        QVERIFY(undo != nullptr && redo != nullptr);
        QVERIFY(undo->text().contains(QLatin1String("route_checkpoint_01")));
        undo->trigger();
        QVERIFY(!saveRoute->isEnabled());
        redo->trigger();
        QVERIFY(saveRoute->isEnabled());

        saveRoute->trigger();
        QVERIFY(!window.isWindowModified());
        QFile saved(output + QStringLiteral("/tracks/testbed/Ribbon_00/TrackRoute005.xml"));
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const fh1::RaceRoute route = fh1::loaders::raceRoute(saved.readAll(), {});
        QCOMPARE(route.transforms.size(), std::size_t{3});
        QCOMPARE(route.transforms[1].name, QStringLiteral("route_checkpoint_01"));
        const QVector3D moved = route.transforms[1].position;
        QVERIFY2(std::abs(moved.x() - 30.0F) < 3.0F && std::abs(moved.z() - 100.0F) < 3.0F,
            qPrintable(QStringLiteral("%1, %2").arg(moved.x()).arg(moved.z())));
        // With no 3D world or roads to go by, the height stays.
        QCOMPARE(moved.y(), 5.0F);
        QCOMPARE(route.transforms[0].position, QVector3D(0.0F, 5.0F, 0.0F));

        // Choosing the game folder saves over the disc's own paths.
        QAction* outputFolder = actions[QStringLiteral("Where to Save Edits…")];
        QVERIFY(outputFolder != nullptr);
        answerNextMessageBox(QStringLiteral("Game Folder"));
        outputFolder->trigger();
        QCOMPARE(QFileInfo(QSettings().value(QStringLiteral("edit/outputFolder")).toString()).canonicalFilePath(),
            QFileInfo(dir.filePath(QStringLiteral("disc/media"))).canonicalFilePath());
        QSettings().remove(QStringLiteral("edit/outputFolder"));
    }

    void gameObjectAndRaceEditing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeRaceInstall(dir.filePath(QStringLiteral("disc"))));
        const QString output = dir.filePath(QStringLiteral("edited"));
        QSettings().setValue(QStringLiteral("edit/outputFolder"), output);

        MainWindow window;
        window.resize(1200, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.openGameFolder(dir.filePath(QStringLiteral("disc"))));
        QTableView* races = nullptr;
        for (QTableView* table : window.findChildren<QTableView*>()) {
            if (table->model()->headerData(0, Qt::Horizontal).toString() == QLatin1String("Event")) {
                races = table;
            }
        }
        QVERIFY(races != nullptr);
        QTRY_COMPARE(races->model()->rowCount(), 1);
        races->selectionModel()->select(
            races->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

        std::map<QString, QAction*> actions;
        for (QAction* action : window.findChildren<QAction*>()) {
            actions[action->text().remove(QLatin1Char('&'))] = action;
        }
        actions[QStringLiteral("Edit on Map")]->setChecked(true);
        auto* view = window.findChild<MapView*>();
        QVERIFY(view != nullptr);

        // Dragging the race start 40 m north takes its node along. With no
        // map image a scene unit is a metre, Z pointing up the screen.
        const QPoint from = view->mapFromScene(QPointF(50.0, 0.0));
        const QPoint to = view->mapFromScene(QPointF(50.0, -40.0));
        QTest::mousePress(view->viewport(), Qt::LeftButton, {}, from);
        QTest::mouseMove(view->viewport(), (from + to) / 2);
        QTest::mouseMove(view->viewport(), to);
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, {}, to);

        // Clicking the flyer selects it, and Delete deletes it alone; the
        // selected object goes before the selected race.
        QAction* remove = actions[QStringLiteral("Delete")];
        QAction* removeGroup = actions[QStringLiteral("Delete with Related Objects")];
        QVERIFY(remove != nullptr && removeGroup != nullptr);
        QCOMPARE(remove->shortcut(), QKeySequence(QKeySequence::Delete));
        QVERIFY(!removeGroup->isEnabled());
        const QPoint flyer = view->mapFromScene(QPointF(-50.0, 0.0));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, flyer);
        QVERIFY(remove->isEnabled());
        // A flyer has no group.
        QVERIFY(!removeGroup->isEnabled());
        remove->trigger();
        QCOMPARE(races->model()->rowCount(), 1);
        QVERIFY(window.findChild<QDialog*>() == nullptr);

        // Race settings go through their dialog.
        actions[QStringLiteral("Race Settings…")]->trigger();
        QTRY_VERIFY(window.findChild<QDialog*>() != nullptr && window.findChild<QDialog*>()->isVisible());
        QDialog* settings = window.findChild<QDialog*>();
        const QList<QSpinBox*> spins = settings->findChildren<QSpinBox*>();
        QVERIFY(spins.size() >= 3);
        spins[0]->setValue(4);
        settings->accept();
        QVERIFY(races->model()->data(races->model()->index(0, 0)).toString().endsWith(QLatin1String(" *")));

        actions[QStringLiteral("Save Edits")]->trigger();
        QVERIFY(!window.isWindowModified());
        QFile objects(output + QStringLiteral("/tracks/testbed/Ribbon_00/GameObjs.xml"));
        QVERIFY(objects.open(QIODevice::ReadOnly));
        const fh1::GameObjectsFile saved = fh1::readGameObjects(objects.readAll(), {});
        QCOMPARE(saved.objects.size(), std::size_t{2});
        QCOMPARE(saved.objects[1].element, QStringLiteral("Obj1"));
        QCOMPARE(saved.objects[1].gameplayId, QStringLiteral("FR98_NODE"));
        const QVector3D start = saved.objects[0].position;
        const QVector3D node = saved.objects[1].position;
        QVERIFY2(std::abs(start.z() - 40.0F) < 3.0F, qPrintable(QString::number(start.z())));
        // The node kept its place beside the start.
        QVERIFY(std::abs(node.x() - start.x() - 10.0F) < 0.01F && std::abs(node.z() - start.z()) < 0.01F);

        const QString connection = QStringLiteral("saved-gamedb");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(output + QStringLiteral("/db/gamedb.slt"));
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("SELECT NumLaps FROM Races WHERE Id = 1")) && query.next());
            QCOMPARE(query.value(0).toInt(), 4);
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        QSettings().remove(QStringLiteral("edit/outputFolder"));
    }

    void deletingARaceEvent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeRaceInstall(
            dir.filePath(QStringLiteral("disc")), {{"FR99", 50}, {"FR99_NODE", 60}, {"flyer_001", -50}}));
        const QString output = dir.filePath(QStringLiteral("edited"));
        QSettings().setValue(QStringLiteral("edit/outputFolder"), output);

        MainWindow window;
        window.resize(1200, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.openGameFolder(dir.filePath(QStringLiteral("disc"))));
        QTableView* races = nullptr;
        for (QTableView* table : window.findChildren<QTableView*>()) {
            if (table->model()->headerData(0, Qt::Horizontal).toString() == QLatin1String("Event")) {
                races = table;
            }
        }
        QVERIFY(races != nullptr);
        QTRY_COMPARE(races->model()->rowCount(), 1);
        std::map<QString, QAction*> actions;
        for (QAction* action : window.findChildren<QAction*>()) {
            actions[action->text().remove(QLatin1Char('&'))] = action;
        }
        QAction* deleteEvent = actions[QStringLiteral("Delete Race Event…\tDel")];
        QVERIFY(deleteEvent != nullptr);
        QVERIFY(!deleteEvent->isEnabled());
        QVERIFY(!actions[QStringLiteral("Delete")]->isEnabled());
        races->selectionModel()->select(
            races->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QVERIFY(deleteEvent->isEnabled());

        // Delete in the Events list asks what to delete, everything ticked at
        // first: the event's two objects and its rows.
        // The key goes through the window, as a real key press does, so
        // that shortcuts see it.
        window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        races->setFocus();
        QTest::keyClick(window.windowHandle(), Qt::Key_Delete);
        QTRY_VERIFY(window.findChild<QDialog*>() != nullptr && window.findChild<QDialog*>()->isVisible());
        QDialog* dialog = window.findChild<QDialog*>();
        const QList<QCheckBox*> boxes = dialog->findChildren<QCheckBox*>();
        QCOMPARE(boxes.size(), 2);
        for (const QCheckBox* box : boxes) {
            QVERIFY(box->isChecked() && box->isEnabled());
        }
        QVERIFY(boxes[0]->text().contains(QLatin1String("FR99_NODE")));
        QVERIFY(boxes[1]->text().contains(QLatin1String("race 1")));
        dialog->accept();
        QCOMPARE(races->model()->rowCount(), 0);

        // The history names both edits, each under its file.
        auto* history = window.findChild<EditHistoryPanel*>();
        QVERIFY(history != nullptr);
        auto* tree = history->findChild<QTreeWidget*>();
        QVERIFY(tree != nullptr);
        QCOMPARE(tree->topLevelItemCount(), 2);
        QStringList edits;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QCOMPARE(tree->topLevelItem(i)->childCount(), 1);
            edits.append(tree->topLevelItem(i)->child(0)->text(0));
        }
        edits.sort();
        QCOMPARE(edits,
            (QStringList{QStringLiteral("Delete FR99 and 1 related object(s)"), QStringLiteral("Delete event FR99")}));

        actions[QStringLiteral("Save Edits")]->trigger();
        QFile objects(output + QStringLiteral("/tracks/testbed/Ribbon_00/GameObjs.xml"));
        QVERIFY(objects.open(QIODevice::ReadOnly));
        const fh1::GameObjectsFile saved = fh1::readGameObjects(objects.readAll(), {});
        QCOMPARE(saved.objects.size(), std::size_t{1});
        QCOMPARE(saved.objects[0].gameplayId, QStringLiteral("flyer_001"));
        const QString connection = QStringLiteral("deleted-event");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(output + QStringLiteral("/db/gamedb.slt"));
            QVERIFY(db.open());
            QSqlQuery query(db);
            for (const QString& table : {QStringLiteral("Events"), QStringLiteral("Races")}) {
                QVERIFY(query.exec(QStringLiteral("SELECT count(*) FROM %1").arg(table)) && query.next());
                QCOMPARE(query.value(0).toInt(), 0);
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        QSettings().remove(QStringLiteral("edit/outputFolder"));
    }

    void restoringOriginals()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString disc = dir.filePath(QStringLiteral("disc"));
        QVERIFY(writeRaceInstall(disc));
        const QString objectsPath = disc + QStringLiteral("/media/tracks/testbed/Ribbon_00/GameObjs.xml");
        QFile original(objectsPath);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray originalObjects = original.readAll();
        original.close();
        QDir(EditSession::backupFolder()).removeRecursively();
        // Saving into the game folder itself backs up the original.
        QSettings().setValue(QStringLiteral("edit/outputFolder"), disc);

        MainWindow window;
        window.resize(1200, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QVERIFY(window.openGameFolder(disc));
        auto* view = window.findChild<MapView*>();
        QVERIFY(view != nullptr);
        std::map<QString, QAction*> actions;
        for (QAction* action : window.findChildren<QAction*>()) {
            actions[action->text().remove(QLatin1Char('&'))] = action;
        }
        QTRY_VERIFY(actions[QStringLiteral("Edit on Map")]->isEnabled());
        actions[QStringLiteral("Edit on Map")]->setChecked(true);
        const QPoint flyer = view->mapFromScene(QPointF(-50.0, 0.0));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, flyer);
        actions[QStringLiteral("Delete")]->trigger();
        actions[QStringLiteral("Save Edits")]->trigger();
        QVERIFY(original.open(QIODevice::ReadOnly));
        QVERIFY(original.readAll() != originalObjects);
        original.close();

        answerNextMessageBox(QStringLiteral("Restore"));
        answerNextMessageBox(QStringLiteral("No"));
        actions[QStringLiteral("Restore Originals…")]->trigger();
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), originalObjects);
        QDir(EditSession::backupFolder()).removeRecursively();
        QSettings().remove(QStringLiteral("edit/outputFolder"));
    }

    void pointBoundsCoverEveryPoint()
    {
        // Regression: bounds built with QRectF::united() collapsed to the last
        // point, which hid every other marker once the view zoomed in.
        fh1::Layer layer;
        layer.features = {point("a", "g", -500, -300), point("b", "g", 800, 20), point("c", "g", 10, 900)};
        PointLayerItem item(layer, kIdentity, 0, 3.0);
        const QRectF bounds = item.boundingRect();
        QVERIFY(bounds.contains(QPointF(-500, -300)));
        QVERIFY(bounds.contains(QPointF(800, 20)));
        QVERIFY(bounds.contains(QPointF(10, 900)));
        for (int i = 0; i < 3; ++i) {
            const auto hit = item.hitTest(item.featureBounds(i).center(), 1.0);
            QVERIFY(hit.has_value());
            QCOMPARE(hit->feature, i);
        }
    }

    void pointHitTestPicksNearestShown()
    {
        fh1::Layer layer;
        layer.features
            = {point("near", "shown", 10, 10), point("nearer", "hidden", 10, 11), point("far", "shown", 50, 50)};
        PointLayerItem item(layer, kIdentity, 0, 3.0);

        auto hit = item.hitTest(QPointF(10, 11.2), 5.0);
        QVERIFY(hit.has_value());
        QCOMPARE(hit->feature, 1);

        item.setGroupVisible(item.groupIndex(QStringLiteral("hidden")), false);
        hit = item.hitTest(QPointF(10, 11.2), 5.0);
        QVERIFY(hit.has_value());
        QCOMPARE(hit->feature, 0);

        QVERIFY(!item.hitTest(QPointF(30, 30), 5.0).has_value());
    }

    void groupsAreOrderedBySize()
    {
        fh1::Layer layer;
        layer.features = {point("1", "small", 0, 0), point("2", "big", 0, 0), point("3", "big", 0, 0)};
        PointLayerItem item(layer, kIdentity, 0, 3.0);
        QCOMPARE(item.groups(), (QStringList{QStringLiteral("big"), QStringLiteral("small")}));
        QCOMPARE(item.groupSize(0), 2);
        QVERIFY(item.groupColor(0) != item.groupColor(1));
    }

    void polylineAndPolygonHitTests()
    {
        fh1::Layer lines;
        lines.kind = fh1::FeatureKind::Polyline;
        fh1::Feature line = point("line", "g", 0, 0);
        line.shapes = {{QVector3D(0, 0, 0), QVector3D(100, 0, 0)}};
        lines.features = {line};
        ShapeLayerItem lineItem(lines, kIdentity, 0);
        auto hit = lineItem.hitTest(QPointF(50, 3), 5.0);
        QVERIFY(hit.has_value());
        QVERIFY(std::abs(hit->distance - 3.0) < 1e-9);
        QVERIFY(!lineItem.hitTest(QPointF(50, 30), 5.0).has_value());

        fh1::Layer zones;
        zones.kind = fh1::FeatureKind::Polygon;
        fh1::Feature zone = point("zone", "g", 5, 5);
        zone.shapes = {{QVector3D(0, 0, 0), QVector3D(10, 0, 0), QVector3D(0, 0, 10)},
            {QVector3D(10, 0, 0), QVector3D(10, 0, 10), QVector3D(0, 0, 10)}};
        zones.features = {zone};
        ShapeLayerItem zoneItem(zones, kIdentity, 0);
        // A click deep inside a zone ranks at the tolerance, so nearby markers win.
        hit = zoneItem.hitTest(QPointF(5, 5), 2.0);
        QVERIFY(hit.has_value());
        QCOMPARE(hit->distance, 2.0);
        // The shared diagonal of the two triangles is not an edge of the zone.
        hit = zoneItem.hitTest(QPointF(5.2, 5.2), 2.0);
        QVERIFY(hit.has_value());
        QCOMPARE(hit->distance, 2.0);
        QVERIFY(!zoneItem.hitTest(QPointF(20, 20), 2.0).has_value());
    }

    void tableListsTopLayerFirst()
    {
        fh1::MapData map;
        fh1::Layer bottom;
        bottom.title = QStringLiteral("Bottom");
        bottom.features = {point("b0", "g", 0, 0), point("b1", "g", 0, 0)};
        fh1::Layer top;
        top.title = QStringLiteral("Top");
        top.features = {point("t0", "g", 1.5F, 2.25F)};
        top.features[0].label = QStringLiteral("Oakley Blitz");
        map.layers = {bottom, top};

        FeatureTableModel model;
        model.setMap(&map);
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.data(model.index(0, FeatureTableModel::Name), Qt::DisplayRole).toString(),
            QStringLiteral("Oakley Blitz"));
        QCOMPARE(model.data(model.index(0, FeatureTableModel::Id), Qt::DisplayRole).toString(), QStringLiteral("t0"));
        QCOMPARE(model.data(model.index(1, FeatureTableModel::Name), Qt::DisplayRole).toString(), QStringLiteral("b0"));
        QCOMPARE(model.data(model.index(0, FeatureTableModel::X), Qt::DisplayRole).toString(), QStringLiteral("1.5"));
        QCOMPARE(model.data(model.index(0, FeatureTableModel::Z), Qt::EditRole).toDouble(), 2.25);
        const FeatureTableModel::Location location = model.locationAt(2);
        QCOMPARE(location.layer, 0);
        QCOMPARE(location.feature, 1);
        QCOMPARE(model.rowOf(0, 1), 2);
        QCOMPARE(model.rowOf(1, 0), 0);
        QCOMPARE(model.locationAt(3).layer, -1);

        model.setMap(nullptr);
        QCOMPARE(model.rowCount(), 0);
    }

    void layerColoursOverridePalette()
    {
        fh1::Layer layer;
        layer.features = {point("1", "start", 0, 0), point("2", "other", 0, 0)};
        layer.groupColours = {{QStringLiteral("start"), QColor(1, 2, 3)}};
        PointLayerItem item(layer, kIdentity, 0, 3.0);
        QCOMPARE(item.groupColor(item.groupIndex(QStringLiteral("start"))), QColor(1, 2, 3));
        QVERIFY(item.groupColor(item.groupIndex(QStringLiteral("other"))) != QColor(1, 2, 3));
    }

    void raceTableSortsByValue()
    {
        fh1::MapData map;
        fh1::Race blitz;
        blitz.eventId = QStringLiteral("FR05");
        blitz.name = QStringLiteral("Oakley Blitz");
        blitz.length = 1785;
        blitz.prize = 3000;
        blitz.route = 0;
        fh1::Race run = blitz;
        run.eventId = QStringLiteral("STREET_PLNS_011");
        run.name = QStringLiteral("Goliath");
        run.length = 60123;
        run.prize = 100000;
        run.route = -1;
        run.routeId = 12;
        map.races = {run, blitz};

        RaceTableModel model;
        model.setMap(&map);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.data(model.index(0, RaceTableModel::EventId), Qt::DisplayRole).toString(),
            QStringLiteral("STREET_PLNS_011"));
        QCOMPARE(model.data(model.index(1, RaceTableModel::Prize), Qt::EditRole).toInt(), 3000);
        QCOMPARE(model.data(model.index(0, RaceTableModel::Length), Qt::EditRole).toInt(), 60123);
        QVERIFY(model.data(model.index(0, RaceTableModel::Length), Qt::DisplayRole)
                .toString()
                .endsWith(QLatin1String(" km")));
        // A race without a route file says why it cannot be shown.
        QVERIFY(
            model.data(model.index(0, RaceTableModel::Name), Qt::ToolTipRole).toString().contains(QLatin1String("12")));
        QVERIFY(!model.data(model.index(1, RaceTableModel::Name), Qt::ToolTipRole).isValid());

        // Sorting uses the raw numbers, not the formatted text.
        QSortFilterProxyModel proxy;
        proxy.setSourceModel(&model);
        proxy.setSortRole(Qt::EditRole);
        proxy.sort(RaceTableModel::Prize);
        QCOMPARE(proxy.data(proxy.index(0, RaceTableModel::Name)).toString(), QStringLiteral("Oakley Blitz"));

        model.setMap(nullptr);
        QCOMPARE(model.rowCount(), 0);
    }

    void viewClickVersusDrag()
    {
        QGraphicsScene scene(0, 0, 1000, 1000);
        MapView view;
        view.setScene(&scene);
        view.resize(400, 400);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.fitScene();

        QSignalSpy clicks(&view, &MapView::clicked);
        const QPoint centre = view.viewport()->rect().center();
        QTest::mouseClick(view.viewport(), Qt::LeftButton, {}, centre);
        QCOMPARE(clicks.count(), 1);
        const QPointF scenePos = clicks.first().first().toPointF();
        QVERIFY((scenePos - view.mapToScene(centre)).manhattanLength() < 1e-6);

        QTest::mousePress(view.viewport(), Qt::LeftButton, {}, centre);
        QTest::mouseMove(view.viewport(), centre + QPoint(40, 0));
        QTest::mouseRelease(view.viewport(), Qt::LeftButton, {}, centre + QPoint(40, 0));
        QCOMPARE(clicks.count(), 1);
    }

    void viewZoomIsClamped()
    {
        QGraphicsScene scene(0, 0, 1000, 1000);
        MapView view;
        view.setScene(&scene);
        view.resize(400, 400);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.fitScene();
        const double fitted = view.zoom();
        for (int i = 0; i < 50; ++i) {
            view.zoomBy(2.0);
        }
        QCOMPARE(view.zoom(), 16.0);
        for (int i = 0; i < 50; ++i) {
            view.zoomBy(0.5);
        }
        QVERIFY(view.zoom() < fitted);
        QVERIFY(view.zoom() >= fitted * 0.49);

        view.focusOn(QRectF(100, 100, 0, 0), 4.0);
        QCOMPARE(view.zoom(), 4.0);
        QVERIFY((view.mapToScene(view.viewport()->rect().center()) - QPointF(100, 100)).manhattanLength() < 1.0);
    }
};

QTEST_MAIN(TestViewer)
#include "tst_viewer.moc"
