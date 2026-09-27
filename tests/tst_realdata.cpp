// Checks against a real extracted Forza Horizon disc. Set FH1_GAME_DIR to the
// disc folder (holding media) to run these; they are skipped otherwise.

#include "ArchiveUpdate.h"
#include "ForzaZip.h"
#include "GameDatabase.h"
#include "GameInstall.h"
#include "GameObjects.h"
#include "Loaders.h"
#include "MapLoader.h"
#include "ModelRemoval.h"
#include "Races.h"
#include "RenderMesh.h"
#include "RouteEditing.h"
#include "ScatterSet.h"
#include "TrackPlacements.h"
#include "TrackTextures.h"
#include "WorldIndex.h"
#include "XmlElements.h"
#include "ZoneGrid.h"

#include <QTest>

#include <QDir>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QVector2D>

#include <algorithm>
#include <map>

namespace {

QString gameDir()
{
    return qEnvironmentVariable("FH1_GAME_DIR");
}

} // namespace

class TestRealData : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        if (gameDir().isEmpty()) {
            QSKIP("FH1_GAME_DIR is not set");
        }
    }

    void everySmallArchiveDecodes()
    {
        fh1::GameInstall install;
        QVERIFY2(install.open(gameDir()), qPrintable(install.errorString()));
        for (const QString& name : {QStringLiteral("gamemodes.zip"), QStringLiteral("aiopenworld.zip"),
                 QStringLiteral("physics.zip"), QStringLiteral("dynamicpost.zip"), QStringLiteral("UI.zip")}) {
            fh1::ForzaZip zip;
            QVERIFY2(zip.open(install.resolve(name)), qPrintable(zip.errorString()));
            for (const fh1::ZipEntry& entry : zip.entries()) {
                QString error;
                QVERIFY2(!zip.read(entry, &error).isNull(), qPrintable(error));
            }
        }
    }

    void coloradoLoadsCompletely()
    {
        fh1::GameInstall install;
        QVERIFY(install.open(gameDir()));
        const fh1::MapData map = fh1::MapLoader::load(install, QStringLiteral("colorado"));
        QVERIFY2(map.warnings.isEmpty(), qPrintable(map.warnings.join(QLatin1Char('\n'))));
        QCOMPARE(map.background.size(), QSize(5120, 3072));

        std::map<QString, std::size_t> counts;
        for (const fh1::Layer& layer : map.layers) {
            counts[layer.id] = layer.features.size();
        }
        QCOMPARE(counts[QStringLiteral("gameobjs")], std::size_t{2148});
        QCOMPARE(counts[QStringLiteral("collobjs")], std::size_t{21877});
        QCOMPARE(counts[QStringLiteral("nav")], std::size_t{12036});
        QCOMPARE(counts[QStringLiteral("airoutes")], std::size_t{46});
        QCOMPARE(counts[QStringLiteral("ppzones")], std::size_t{16});
        QCOMPARE(counts[QStringLiteral("particles")], std::size_t{2367});

        // The calibration must put the road network on the image.
        for (const fh1::Layer& layer : map.layers) {
            if (layer.id != QLatin1String("nav")) {
                continue;
            }
            const QRectF image(QPointF(0, 0), QSizeF(map.background.size()));
            std::size_t inside = 0;
            for (const fh1::Feature& node : layer.features) {
                inside += image.contains(map.calibration.worldToImage(node.position.x(), node.position.z())) ? 1 : 0;
            }
            QVERIFY(inside > layer.features.size() * 95 / 100);
        }
    }

    void labelsAndIconsResolve()
    {
        fh1::GameInstall install;
        QVERIFY(install.open(gameDir()));
        const fh1::MapData map = fh1::MapLoader::load(install, QStringLiteral("colorado"));

        auto findLayer = [&map](const QString& id) -> const fh1::Layer* {
            for (const fh1::Layer& layer : map.layers) {
                if (layer.id == id) {
                    return &layer;
                }
            }
            return nullptr;
        };
        auto findFeature = [](const fh1::Layer* layer, const QString& name) -> const fh1::Feature* {
            for (const fh1::Feature& feature : layer->features) {
                if (feature.name == name) {
                    return &feature;
                }
            }
            return nullptr;
        };

        const fh1::Layer* routes = findLayer(QStringLiteral("airoutes"));
        QVERIFY(routes != nullptr);
        const fh1::Feature* beaumont = findFeature(routes, QStringLiteral("FESTIVAL_MOUN_007 (route 49)"));
        QVERIFY(beaumont != nullptr);
        QCOMPARE(beaumont->label, QStringLiteral("Beaumont Circuit"));

        const fh1::Layer* gameplay = findLayer(QStringLiteral("gameobjs"));
        QVERIFY(gameplay != nullptr);
        const fh1::Feature* event = findFeature(gameplay, QStringLiteral("FR05"));
        QVERIFY(event != nullptr);
        QCOMPARE(event->label, QStringLiteral("Oakley Blitz"));
        QCOMPARE(event->icon, QStringLiteral("race"));
        QCOMPARE(event->group, QStringLiteral("Race events"));
        const fh1::Feature* node = findFeature(gameplay, QStringLiteral("FR05_NODE"));
        QVERIFY(node != nullptr);
        QCOMPARE(node->group, QStringLiteral("Race events (car placement points)"));
        const fh1::Feature* barnFind = findFeature(gameplay, QStringLiteral("BARNFIND_CUDA_426BF"));
        QVERIFY(barnFind != nullptr);
        QCOMPARE(barnFind->label, QStringLiteral("1971 Plymouth Cuda 426 HEMI"));
        QCOMPARE(barnFind->icon, QStringLiteral("barnfind"));

        std::map<QString, int> iconCounts;
        for (const fh1::Feature& feature : gameplay->features) {
            if (!feature.icon.isEmpty()) {
                ++iconCounts[feature.icon];
            }
        }
        QCOMPARE(iconCounts[QStringLiteral("speed_camera_discovered")], 44);
        QCOMPARE(iconCounts[QStringLiteral("flyer")], 100);
        QCOMPARE(iconCounts[QStringLiteral("gas_station")], 10);
        QCOMPARE(iconCounts[QStringLiteral("barnfind")], 9);
        for (const auto& [key, count] : iconCounts) {
            QVERIFY2(map.icons.contains(key), qPrintable(key));
            QCOMPARE(map.icons.value(key).size(), QSize(128, 128));
        }
    }

    void worldIndexAndMeshes()
    {
        fh1::GameInstall install;
        QVERIFY(install.open(gameDir()));
        fh1::ForzaZip archive;
        QVERIFY(archive.open(install.resolve(QStringLiteral("tracks/colorado/bin.zip"))));
        const std::optional<fh1::WorldIndex> index = fh1::WorldIndex::build(archive);
        QVERIFY(index.has_value());
        // 102,012 model entries hold 14,291 distinct models; about 2,300 are
        // local-space props (not placed without the zone files), about 1,100
        // the crude TERR_CUBE copy of the scene, and a few dozen cages and
        // shadow casters.
        QVERIFY2(index->chunks().size() > 10500 && index->chunks().size() < 11200,
            qPrintable(QString::number(index->chunks().size())));
        QVERIFY(index->localModelCount() > 2000 && index->localModelCount() < 2600);
        QSet<QString> names;
        for (const fh1::WorldChunk& chunk : index->chunks()) {
            const QString name = archive.entries()[chunk.entry].name.toLower();
            QVERIFY2(!names.contains(name), qPrintable(name));
            names.insert(name);
        }
        // A strict parse of every 40th world model.
        for (std::size_t i = 0; i < index->chunks().size(); i += 40) {
            const fh1::ZipEntry& entry = archive.entries()[index->chunks()[i].entry];
            QString error;
            const QByteArray data = archive.read(entry, &error);
            QVERIFY2(!data.isNull(), qPrintable(error));
            const fh1::RenderMesh mesh = fh1::rendermesh::parse(data, entry.name);
            QVERIFY(!mesh.parts.empty());
            QVERIFY2(!mesh.materialTable.empty(), qPrintable(entry.name));
        }
    }

    void racesAndRoutes()
    {
        fh1::GameInstall install;
        QVERIFY(install.open(gameDir()));
        const fh1::MapData map = fh1::MapLoader::load(install, QStringLiteral("colorado"));
        // 119 events, less the free-roam session; each has one race.
        QCOMPARE(map.races.size(), std::size_t{118});
        QCOMPARE(map.raceRoutes.size(), std::size_t{243});
        for (const fh1::Race& race : map.races) {
            QVERIFY2(race.route >= 0, qPrintable(race.eventId));
            QVERIFY2(!race.type.isEmpty() && !race.carClass.isEmpty(), qPrintable(race.eventId));
        }

        const auto find = [&map](const QString& eventId) -> const fh1::Race* {
            for (const fh1::Race& race : map.races) {
                if (race.eventId == eventId) {
                    return &race;
                }
            }
            return nullptr;
        };
        // A festival circuit with an AI racing line: its route id is its
        // track id.
        const fh1::Race* rush = find(QStringLiteral("FR02"));
        QVERIFY(rush != nullptr);
        QCOMPARE(rush->name, QStringLiteral("Recaro Rush"));
        QCOMPARE(rush->type, QStringLiteral("Festival Circuit Race"));
        QCOMPARE(rush->carClass, QStringLiteral("B"));
        QCOMPARE(rush->laps, 2);
        QCOMPARE(rush->length, 2467);
        QCOMPARE(rush->prize, 4000);
        const fh1::RaceRoute& rushRoute = map.raceRoutes[static_cast<std::size_t>(rush->route)];
        QCOMPARE(rushRoute.routeId, 96);
        QVERIFY(!rushRoute.racingLine.empty());
        QCOMPARE(fh1::routePoints(rushRoute, fh1::RoutePointKind::StartSlot).size(), std::size_t{8});

        // A street race on track 1001, whose route is 12; it has
        // checkpoints but no racing line.
        const fh1::Race* plains = find(QStringLiteral("STREET_PLNS_005"));
        QVERIFY(plains != nullptr);
        QCOMPARE(plains->routeId, 12);

        // The game's scripts point the sat nav at FR02 after the opening, and
        // the opening race, HORIZON Heats, has cutscenes of its own; an
        // ordinary race is named only by its own activation.
        QVERIFY(map.scripts.filesUsing(QStringLiteral("FR02")).contains(QStringLiteral("first_time_career.xml")));
        QVERIFY(
            map.scripts.filesUsing(QStringLiteral("festival_02")).contains(QStringLiteral("first_time_career.xml")));
        QVERIFY(!map.scripts.filesUsing(QStringLiteral("FESTIVAL_SECOND")).isEmpty());
        QVERIFY(map.scripts.filesUsing(QStringLiteral("FR08")).isEmpty());
        const fh1::Race* heats = find(QStringLiteral("FESTIVAL_SECOND"));
        QVERIFY(heats != nullptr);
        QCOMPARE(heats->level, -1);
        QCOMPARE(rush->level, 0);
        const fh1::RaceRoute& plainsRoute = map.raceRoutes[static_cast<std::size_t>(plains->route)];
        QVERIFY(plainsRoute.racingLine.empty());
        const std::vector<fh1::Layer> overlay = fh1::raceOverlay(*plains, plainsRoute);
        QCOMPARE(overlay.size(), std::size_t{2});
        QVERIFY(overlay[0].features.front().shapes.front().size() > 2);
    }

    void routesSaveUnchanged()
    {
        fh1::GameInstall install;
        QVERIFY(install.open(gameDir()));
        const fh1::MapData map = fh1::MapLoader::load(install, QStringLiteral("colorado"));
        QCOMPARE(map.raceRoutes.size(), std::size_t{243});
        for (const fh1::RaceRoute& route : map.raceRoutes) {
            QFile file(QDir(install.mediaPath()).filePath(route.mediaPath));
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(route.mediaPath));
            const QByteArray original = file.readAll();
            QVERIFY2(fh1::writeRaceRoute(route) == original, qPrintable(route.mediaPath));

            // Every transform written anew reads back the same: the number
            // format loses nothing the file held. The game's own files come
            // out byte for byte; TrackRoute001, laid out by hand, does not.
            fh1::RaceRoute rewritten = route;
            for (fh1::RouteTransform& transform : rewritten.transforms) {
                transform.edited = true;
            }
            const QByteArray written = fh1::writeRaceRoute(rewritten);
            if (!route.mediaPath.endsWith(QLatin1String("TrackRoute001.xml"))) {
                QVERIFY2(written == original, qPrintable(route.mediaPath));
            }
            const fh1::RaceRoute reread = fh1::loaders::raceRoute(written, route.source);
            QCOMPARE(reread.transforms.size(), route.transforms.size());
            for (std::size_t i = 0; i < route.transforms.size(); ++i) {
                QCOMPARE(reread.transforms[i].name, route.transforms[i].name);
                QCOMPARE(reread.transforms[i].position, route.transforms[i].position);
                QCOMPARE(reread.transforms[i].facing, route.transforms[i].facing);
                QCOMPARE(reread.transforms[i].width, route.transforms[i].width);
                QCOMPARE(reread.transforms[i].attributes, route.transforms[i].attributes);
            }
        }
    }

    void gameObjectsAndRaceSettingsSave()
    {
        fh1::GameInstall install;
        QVERIFY(install.open(gameDir()));
        const fh1::MapData map = fh1::MapLoader::load(install, QStringLiteral("colorado"));
        // The objects kept for editing line up with the map's markers.
        QCOMPARE(map.gameObjects.objects.size(), std::size_t{2148});
        QCOMPARE(map.gameObjects.mediaPath, QStringLiteral("tracks/colorado/Ribbon_00/GameObjs.xml"));
        QFile file(QDir(install.mediaPath()).filePath(map.gameObjects.mediaPath));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray original = file.readAll();
        QVERIFY(fh1::writeGameObjects(map.gameObjects) == original);

        // Every object written anew reads back the same, and the file comes
        // out byte for byte: the number format loses nothing.
        fh1::GameObjectsFile rewritten = map.gameObjects;
        for (fh1::GameObject& object : rewritten.objects) {
            object.edited = true;
        }
        QVERIFY(fh1::writeGameObjects(rewritten) == original);

        // Removing one keeps the others and their numbering unbroken.
        fh1::GameObjectsFile removed = map.gameObjects;
        fh1::removeGameObject(removed, 0);
        const fh1::GameObjectsFile reread = fh1::readGameObjects(fh1::writeGameObjects(removed), {});
        QCOMPARE(reread.objects.size(), std::size_t{2147});
        for (std::size_t i = 0; i < reread.objects.size(); ++i) {
            QCOMPARE(reread.objects[i].element, QStringLiteral("Obj%1").arg(i));
            QCOMPARE(reread.objects[i].gameplayId, map.gameObjects.objects[i + 1].gameplayId);
        }

        // Race settings go to a copy of the database; only the edited race
        // changes, and the game's file is left alone.
        QVERIFY(map.carClasses.size() == 11);
        std::vector<fh1::Race> races = map.races;
        const auto rush
            = std::find_if(races.begin(), races.end(), [](const fh1::Race& r) { return r.eventId == "FR02"; });
        QVERIFY(rush != races.end());
        QCOMPARE(rush->opponents, 7);
        QCOMPARE(rush->timeOfDay, 33420);
        rush->laps = 4;
        rush->prize = 12345;
        rush->timeOfDay = 3600;
        QString error;
        const QString database = install.resolve(QStringLiteral("db/gamedb.slt"));
        const std::optional<QByteArray> edited = fh1::writeRaceSettings(database, races, map.races, {}, &error);
        QVERIFY2(edited.has_value(), qPrintable(error));
        QTemporaryDir dir;
        QFile copy(dir.filePath(QStringLiteral("gamedb.slt")));
        QVERIFY(copy.open(QIODevice::WriteOnly) && copy.write(*edited) == edited->size());
        copy.close();
        fh1::GameDatabase reopened;
        QVERIFY(reopened.open(copy.fileName()));
        std::size_t changed = 0;
        const std::vector<fh1::GameDatabase::RaceRow> rows = reopened.races(QStringLiteral("colorado"));
        QCOMPARE(rows.size(), map.races.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const bool same = rows[i].laps == map.races[i].laps && rows[i].prize == map.races[i].prize
                && rows[i].timeOfDay == map.races[i].timeOfDay;
            changed += same ? 0 : 1;
            if (rows[i].eventId == QLatin1String("FR02")) {
                QCOMPARE(rows[i].laps, 4);
                QCOMPARE(rows[i].prize, 12345);
                QCOMPARE(rows[i].timeOfDay, 3600);
                QCOMPARE(rows[i].opponents, 7);
            }
        }
        QCOMPARE(changed, std::size_t{1});
        // Deleting an event takes every row that refers to it.
        const auto blitz
            = std::find_if(map.races.begin(), map.races.end(), [](const fh1::Race& r) { return r.eventId == "FR05"; });
        QVERIFY(blitz != map.races.end());
        fh1::GameDatabase game;
        QVERIFY(game.open(database));
        QVERIFY(!game.eventReferences(blitz->eventRow).empty());
        std::vector<fh1::Race> remaining = map.races;
        remaining.erase(remaining.begin() + (blitz - map.races.begin()));
        std::vector<fh1::Race> loaded = remaining;
        const std::optional<QByteArray> deleted = fh1::writeRaceSettings(database, remaining, loaded, {*blitz}, &error);
        QVERIFY2(deleted.has_value(), qPrintable(error));
        QFile without(dir.filePath(QStringLiteral("without.slt")));
        QVERIFY(without.open(QIODevice::WriteOnly) && without.write(*deleted) == deleted->size());
        without.close();
        fh1::GameDatabase pruned;
        QVERIFY(pruned.open(without.fileName()));
        QCOMPARE(pruned.races(QStringLiteral("colorado")).size(), map.races.size() - 1);
        QVERIFY(pruned.eventReferences(blitz->eventRow).empty());
    }

    void layerFilesSaveUnchanged()
    {
        fh1::GameInstall install;
        QVERIFY(install.open(gameDir()));
        const fh1::MapData map = fh1::MapLoader::load(install, QStringLiteral("colorado"));
        QCOMPARE(map.layerFiles.size(), 3);
        for (auto [id, file] : map.layerFiles.asKeyValueRange()) {
            QFile original(QDir(install.mediaPath()).filePath(file.mediaPath));
            QVERIFY2(original.open(QIODevice::ReadOnly), qPrintable(file.mediaPath));
            QVERIFY2(fh1::writeXmlElements(file) == original.readAll(), qPrintable(file.mediaPath));
            // Removing the first element leaves the rest, renumbered where
            // the file numbers them.
            fh1::XmlElementsFile removed = file;
            fh1::removeXmlElement(removed, 0);
            const fh1::XmlElementsFile reread = fh1::readXmlElements(fh1::writeXmlElements(removed), file.mediaPath);
            QCOMPARE(reread.elements.size(), file.elements.size() - 1);
            QCOMPARE(reread.numbered, file.numbered);
        }
        QVERIFY(map.layerFiles.value(QStringLiteral("collobjs")).numbered);
        QVERIFY(!map.layerFiles.value(QStringLiteral("particles")).numbered);
    }

    void propPlacements()
    {
        fh1::GameInstall install;
        QVERIFY(install.open(gameDir()));
        fh1::ForzaZip archive;
        QVERIFY(archive.open(install.resolve(QStringLiteral("tracks/colorado/bin.zip"))));
        QFile pvs(install.trackPvsPath(QStringLiteral("colorado")));
        QVERIFY(pvs.open(QIODevice::ReadOnly));
        QString error;
        const std::optional<fh1::TrackPlacements> placements
            = fh1::TrackPlacements::load(pvs.readAll(), archive, nullptr, &error);
        QVERIFY2(placements.has_value(), qPrintable(error));
        // Every distinct zone file reads to its end.
        QCOMPARE(placements->zoneCount(), 1434);
        QCOMPARE(placements->failedZones(), 0);
        QCOMPARE(placements->drawCount(), std::size_t{62173});
        int placed = 0;
        int eventProps = 0;
        for (std::size_t d = 0; d < placements->drawCount(); ++d) {
            const fh1::Placement* p = placements->placement(d);
            placed += p != nullptr ? 1 : 0;
            eventProps += p != nullptr && p->eventProp ? 1 : 0;
        }
        QVERIFY2(placed > 59000, qPrintable(QString::number(placed)));
        // Race and festival gear: 22,263 draws when this was written.
        QVERIFY2(eventProps > 20000 && eventProps < 25000, qPrintable(QString::number(eventProps)));
        // Some of it names its event: 37 draws belong to Ferrari Massimo
        // (FR04).
        int ferrariMassimo = 0;
        for (std::size_t d = 0; d < placements->drawCount(); ++d) {
            const fh1::Placement* p = placements->placement(d);
            ferrariMassimo += p != nullptr && p->eventProp && p->belongsToEvent(QStringLiteral("FR04")) ? 1 : 0;
        }
        QCOMPARE(ferrariMassimo, 37);
        // Most race gear names no event but lists the routes it is put out
        // for: with its named gear, 517 draws for Bondurant Valley Skirmish
        // (FR10, route 151).
        int valleySkirmish = 0;
        for (std::size_t d = 0; d < placements->drawCount(); ++d) {
            const fh1::Placement* p = placements->placement(d);
            valleySkirmish += p != nullptr && p->eventProp && p->belongsToRace(QStringLiteral("FR10"), 151) ? 1 : 0;
        }
        QCOMPARE(valleySkirmish, 517);

        // Obj17026 of CollObjs.xml, a marker pole (render object 701).
        const fh1::Placement* pole = nullptr;
        for (std::size_t d = 0; d < placements->drawCount() && pole == nullptr; ++d) {
            const fh1::Placement* p = placements->placement(d);
            if (placements->drawObject(d) == 701 && p != nullptr
                && (p->position - QVector3D(-1670.525F, 12.5432F, -1207.935F)).length() < 0.01F) {
                pole = p;
            }
        }
        QVERIFY(pole != nullptr);
        // Marker poles are breakable but always there.
        QVERIFY(!pole->eventProp);
        const QVector3D xAxis = pole->apply(QVector3D(1, 0, 0)) - pole->position;
        QVERIFY((xAxis - QVector3D(0.163759F, 0, 0.9865F)).length() < 0.01F);

        // Every procedural model set reads; together they place the trees,
        // bushes, rocks and fences (128,622 copies when this was written).
        QCOMPARE(placements->scatterSets().size(), std::size_t{1562});
        QCOMPARE(placements->failedScatterSets(), 0);
        std::size_t scattered = 0;
        for (const fh1::ScatterSet& set : placements->scatterSets()) {
            scattered += set.instances.size();
        }
        QVERIFY2(scattered > 125000 && scattered < 135000, qPrintable(QString::number(scattered)));

        // The zone at the festival (X -752, Z -260) lists the festival's
        // ground (render object 10020) but not the backdrop terrain over it
        // (render object 12910), which there lies 14 m above the ground.
        QString zoneError;
        const std::optional<fh1::ZoneGrid> zones
            = fh1::ZoneGrid::readFile(install.trackZoneGridPath(QStringLiteral("colorado")), &zoneError);
        QVERIFY2(zones.has_value(), qPrintable(zoneError));
        QCOMPARE(zones->zoneCount(), 1434);
        const int festival = zones->zoneAt(-752.0F, -260.0F);
        QVERIFY(festival >= 0);
        const auto listedAtFestival = [&](std::uint16_t object) {
            for (std::size_t d = 0; d < placements->drawCount(); ++d) {
                const std::vector<std::uint16_t>& listing = placements->zonesListing(d);
                if (placements->drawObject(d) == object
                    && std::binary_search(listing.begin(), listing.end(), festival)) {
                    return true;
                }
            }
            return false;
        };
        QVERIFY(listedAtFestival(10020));
        QVERIFY(!listedAtFestival(12910));

        const std::optional<fh1::WorldIndex> index
            = fh1::WorldIndex::build(archive, {}, nullptr, &*placements, &*zones);
        QVERIFY(index.has_value());
        // Zone placements and every level of every procedural copy.
        QVERIFY2(index->placedCount() > 250000, qPrintable(QString::number(index->placedCount())));
        // The dam's main wall is modelled around a pivot its draw moves to
        // 6719.6, 172.7, -390.5; its centre lies 5, -10 and 22 m from it.
        const bool damPlaced
            = std::any_of(index->chunks().begin(), index->chunks().end(), [](const fh1::WorldChunk& c) {
                  const QVector3D centre = (c.boundsMin + c.boundsMax) / 2.0F;
                  return c.placed && c.boundsMax.x() - c.boundsMin.x() > 200.0F
                      && (centre - QVector3D(6724.6F, 162.7F, -368.5F)).length() < 30.0F;
              });
        QVERIFY(damPlaced);
        int backdrop = 0;
        for (const fh1::WorldChunk& chunk : index->chunks()) {
            if (chunk.backdrop) {
                ++backdrop;
                QVERIFY(!chunk.zones.empty());
            }
        }
        // The 23 finest backdrop pieces; their 44 coarser levels are in no
        // zone's list.
        QCOMPARE(backdrop, 23);
        // Only a few hundred prop models are never placed.
        QVERIFY2(index->localModelCount() < 1000, qPrintable(QString::number(index->localModelCount())));

        // Removing the marker pole zeroes the matrix of every zone record
        // that places it, and of no other.
        const auto poleChunk
            = std::find_if(index->chunks().begin(), index->chunks().end(), [](const fh1::WorldChunk& c) {
                  return c.placed && !c.scattered
                      && (c.placement.position - QVector3D(-1670.525F, 12.5432F, -1207.935F)).length() < 0.01F;
              });
        QVERIFY(poleChunk != index->chunks().end());
        const auto poleIndex = static_cast<std::uint32_t>(poleChunk - index->chunks().begin());
        const std::optional<fh1::ZoneRecordIndex> records = fh1::ZoneRecordIndex::build(archive, &error);
        QVERIFY2(records.has_value(), qPrintable(error));
        QSet<std::int32_t> poleDraws;
        for (const std::uint32_t c : fh1::placedModelChunks(*index, poleIndex)) {
            poleDraws.insert(index->chunks()[c].sourceDraw);
        }
        QVERIFY(!poleDraws.isEmpty());
        QHash<std::uint32_t, QByteArray> edited;
        QVERIFY2(fh1::removePlacedModel(archive, *index, *records, poleIndex, edited, &error), qPrintable(error));
        QVERIFY(!edited.isEmpty());
        for (auto [entry, data] : edited.asKeyValueRange()) {
            const auto before = fh1::TrackPlacements::readZone(archive.read(archive.entries()[entry]));
            const auto after = fh1::TrackPlacements::readZone(data);
            QVERIFY(before && after && before->size() == after->size());
            for (std::size_t r = 0; r < after->size(); ++r) {
                const bool poleRecord = poleDraws.contains(static_cast<std::int32_t>((*after)[r].first));
                const bool zeroed = (*after)[r].second.rows == std::array<float, 9>{};
                QCOMPARE(zeroed, poleRecord);
                QCOMPARE((*after)[r].second.position, (*before)[r].second.position);
            }
        }
    }

    void archiveUpdateKeepsOtherEntries()
    {
        fh1::GameInstall install;
        QVERIFY(install.open(gameDir()));
        const QString source = install.resolve(QStringLiteral("gamemodes.zip"));
        fh1::ForzaZip original;
        QVERIFY(original.open(source));
        QTemporaryDir dir;
        const QString target = dir.filePath(QStringLiteral("gamemodes.zip"));
        QString error;
        QByteArray changed = original.read(original.entries()[3]);
        changed.append("<!-- edited -->");
        QVERIFY2(fh1::writeUpdatedArchive(source, target, {{3, changed}}, &error), qPrintable(error));
        fh1::ForzaZip updated;
        QVERIFY2(updated.open(target), qPrintable(updated.errorString()));
        QCOMPARE(updated.entries().size(), original.entries().size());
        for (std::uint32_t i = 0; i < updated.entries().size(); ++i) {
            const QByteArray data = updated.read(updated.entries()[i], &error);
            QVERIFY2(!data.isNull(), qPrintable(error));
            QCOMPARE(data, i == 3 ? changed : original.read(original.entries()[i]));
        }
    }

    void textureTables()
    {
        fh1::GameInstall install;
        QVERIFY(install.open(gameDir()));
        fh1::ForzaZip archive;
        QVERIFY(archive.open(install.resolve(QStringLiteral("tracks/colorado/bin.zip"))));
        QFile pvs(install.resolve(QStringLiteral("tracks/colorado/Ribbon_00/Colorado_00.pvs")));
        QVERIFY(pvs.open(QIODevice::ReadOnly));
        QString error;
        const std::optional<fh1::TrackTextures> textures = fh1::TrackTextures::load(pvs.readAll(), archive, &error);
        QVERIFY2(textures.has_value(), qPrintable(error));
        // One PVS object per render model number (0 to 14559).
        QCOMPARE(textures->objectCount(), std::size_t{14560});
        QVERIFY(textures->shaderCount() > 150);
        const fh1::ShaderLayout* layout = textures->shader(QStringLiteral("shaders\\track\\h_diff_spec_ao_2.fx"));
        QVERIFY(layout != nullptr);
        QCOMPARE(layout->texcoord0Offset, 16);
        QCOMPARE(layout->vertexBytes, 24);

        // The round "Greasy Toolbox" sign in the main town, and one of the
        // festival's team trailers: their diffuse textures were matched by
        // overlaying the models' texture coordinates on the images.
        const std::vector<std::uint32_t>* sign = textures->objectTextures(6454);
        QVERIFY(sign != nullptr && !sign->empty());
        QCOMPARE(sign->front(), 0x2B40u);
        const std::vector<std::uint32_t>* trailer = textures->objectTextures(2085);
        QVERIFY(trailer != nullptr && !trailer->empty());
        QCOMPARE(trailer->front(), 0x1EDFu);

        const std::optional<fh1::TextureMipChain> bix = textures->loadTexture(archive, 0x2B40, &error);
        QVERIFY2(bix.has_value(), qPrintable(error));
        QCOMPARE(bix->width, 256);
        QCOMPARE(bix->height, 256);
        QCOMPARE(bix->format, fh1::TextureSurface::Format::Dxt1);
        QCOMPARE(bix->levels.size(), std::size_t{9});
        // The sign's oval is red on a dark board.
        const QImage signImage = fh1::surfaceToImage(bix->level(0));
        const QRgb oval = signImage.pixel(40, 64);
        QVERIFY2(qRed(oval) > 2 * qGreen(oval), qPrintable(QString::number(oval, 16)));

        // Nearly every texture has a small copy in the 198 bundle packs; for
        // texture 42 that copy is the only one.
        QVERIFY2(textures->bundledTextureCount() > 18000, qPrintable(QString::number(textures->bundledTextureCount())));
        QVERIFY(archive.find(QStringLiteral("_0x0000002A.bix")) == nullptr);
        QVERIFY(archive.find(QStringLiteral("_0x0000002A.bin")) == nullptr);
        const std::optional<fh1::TextureMipChain> bundled = textures->loadTexture(archive, 42, &error);
        QVERIFY2(bundled.has_value(), qPrintable(error));
        QVERIFY(bundled->width <= 16 && bundled->height <= 16);

        const std::optional<fh1::TextureMipChain> caff = textures->loadTexture(archive, 0x15C7, &error);
        QVERIFY2(caff.has_value(), qPrintable(error));
        QCOMPARE(caff->width, 32);
        QCOMPARE(caff->height, 32);

        // The sign's texture coordinates, through its material's transform,
        // cover the texture without leaving it.
        const QByteArray model = archive.read(QStringLiteral("coloradoout.06454.rmb.bin"), &error);
        QVERIFY2(!model.isNull(), qPrintable(error));
        const fh1::RenderMesh mesh = fh1::rendermesh::parse(model, QStringLiteral("coloradoout.06454.rmb.bin"));
        const fh1::RenderMesh::Part& part = mesh.parts.front();
        const fh1::RenderMesh::Material& material = part.materials.front();
        QVector2D lo(1e9F, 1e9F);
        QVector2D hi(-1e9F, -1e9F);
        for (std::uint32_t v = 0; v < part.positions.size(); ++v) {
            const QVector2D uv = fh1::RenderMesh::texcoord(part, material, v, layout->texcoord0Offset);
            lo = QVector2D(std::min(lo.x(), uv.x()), std::min(lo.y(), uv.y()));
            hi = QVector2D(std::max(hi.x(), uv.x()), std::max(hi.y(), uv.y()));
        }
        QVERIFY2(lo.x() > -0.01F && lo.y() > -0.01F && hi.x() < 1.01F && hi.y() < 1.01F,
            qPrintable(QStringLiteral("%1,%2 %3,%4").arg(lo.x()).arg(lo.y()).arg(hi.x()).arg(hi.y())));
        QVERIFY(hi.x() - lo.x() > 0.9F && hi.y() - lo.y() > 0.9F);
    }
};

QTEST_GUILESS_MAIN(TestRealData)
#include "tst_realdata.moc"
