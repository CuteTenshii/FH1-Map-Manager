#include "GameInstall.h"
#include "Loaders.h"
#include "MapCalibration.h"
#include "MapLoader.h"
#include "Races.h"
#include "RouteEditing.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <cstring>

namespace {

void appendBe32(QByteArray& out, std::uint32_t value)
{
    char bytes[4];
    qToBigEndian(value, bytes);
    out.append(bytes, 4);
}

void appendBeFloat(QByteArray& out, float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    appendBe32(out, bits);
}

struct NavNode {
    std::uint32_t id;
    float x, y, z;
    std::uint32_t links;
};

QByteArray navFile(const QList<NavNode>& nodes, std::uint32_t declaredLinks)
{
    QByteArray data;
    appendBe32(data, 0x0E177551);
    appendBe32(data, static_cast<std::uint32_t>(nodes.size()));
    appendBe32(data, 0);
    appendBe32(data, 0);
    appendBe32(data, declaredLinks);
    appendBe32(data, declaredLinks);
    for (int i = 0; i < 4; ++i) {
        appendBe32(data, 0);
    }
    std::uint32_t firstLink = 0;
    for (const NavNode& node : nodes) {
        appendBe32(data, node.id);
        appendBeFloat(data, node.x);
        appendBeFloat(data, node.y);
        appendBeFloat(data, node.z);
        appendBe32(data, node.links);
        appendBe32(data, firstLink);
        appendBe32(data, 0);
        appendBe32(data, 0);
        firstLink += node.links;
    }
    return data;
}

QByteArray owtFile(const QList<QVector3D>& points, bool loop)
{
    QByteArray data("OWTM");
    appendBe32(data, 1);
    appendBe32(data, 0);
    appendBe32(data, 0);
    appendBe32(data, static_cast<std::uint32_t>(points.size()));
    appendBe32(data, loop ? 1 : 0);
    appendBe32(data, 0);
    appendBe32(data, 0);
    for (const QVector3D& p : points) {
        appendBeFloat(data, p.x());
        appendBeFloat(data, p.y());
        appendBeFloat(data, p.z());
        for (int i = 0; i < 9; ++i) {
            appendBeFloat(data, 0.0F);
        }
    }
    data.append("OWTM");
    appendBe32(data, 1);
    appendBe32(data, 0);
    appendBe32(data, 0);
    return data;
}

const char* const kCollObjs = R"(<?xml version="1.0" ?>
<CollObjs>
	<Obj0 PhysicsType="CO_ArmcoArrow_001.0.rmb" GraphicsName="#0">
		<Pos x="-608.297485" y="87.266258" z="-2952.991943"/>
		<Orientation>
			<XAxis x="0.933504" y="0.000000" z="-0.358568"/>
			<YAxis x="0.000000" y="1.000000" z="0.000000"/>
			<ZAxis x="0.358568" y="-0.000000" z="0.933504"/>
		</Orientation>
	</Obj0>
	<Obj1 PhysicsType="OBJ_CLRD_Signs_Reservoir_Slow.rmb" GraphicsName="#1">
		<Pos x="10" y="20" z="30"/>
	</Obj1>
</CollObjs>
)";

const char* const kGameObjs = R"(<?xml version="1.0" ?>
<GameObjs>
	<Obj0 GameplayID="BF_CUDA_426BF_CLOSEDC">
		<Pos x="-1291.224976" y="46.967731" z="-3502.273926"/>
		<Orientation>
			<ZAxis x="0.457902" y="-0.019211" z="-0.888795"/>
		</Orientation>
	</Obj0>
	<Obj1 GameplayID="speed_camera_30_left">
		<Pos x="1" y="2" z="3"/>
	</Obj1>
</GameObjs>
)";

const char* const kTrackRoute = R"(<TrackRoute>
	<NamedTransforms>
		<NamedTransform name='start_location_00'>
			<Transform pos.x='1584.97' pos.y='119.51' pos.z='-1843.62' facing.x='-0.313279' facing.y='0.0' facing.z='-0.949661'/>
		</NamedTransform>
		<NamedTransform name='anim_couple_01_01'>
			<Transform pos.x='-3892.29' pos.y='-0.334236' pos.z='837.49' facing.x='-0.999991' facing.y='0.0' facing.z='-0.00428313'/>
		</NamedTransform>
		<NamedTransform name='end_race_cannon_trigger' width='50.0'>
			<Transform pos.x='-3352.49' pos.y='3.65841' pos.z='-1499.24' facing.x='-0.981728' facing.y='0.0' facing.z='-0.190288'/>
		</NamedTransform>
	</NamedTransforms>
</TrackRoute>
)";

/// A route file in the game's layout, CRLF line ends, with a comment
/// between two transforms.
QByteArray checkpointRoute()
{
    return QByteArrayLiteral(
        "<TrackRoute>\r\n\t<NamedTransforms>\r\n"
        "\t\t<NamedTransform name='route_checkpoint_00' width='50.0'>\r\n"
        "\t\t\t<Transform pos.x='0.0' pos.y='1.5' pos.z='0.0' facing.x='0.0' facing.y='0.0' facing.z='1.0'/>\r\n"
        "\t\t</NamedTransform>\r\n"
        "\t\t<!-- second gate -->\r\n"
        "\t\t<NamedTransform name='route_checkpoint_01' width='40.0'>\r\n"
        "\t\t\t<Transform pos.x='0.0' pos.y='2.0' pos.z='100.0' facing.x='0.0' facing.y='0.0' facing.z='1.0'/>\r\n"
        "\t\t</NamedTransform>\r\n"
        "\t\t<NamedTransform name='route_checkpoint_indicator_00'>\r\n"
        "\t\t\t<Transform pos.x='4.0' pos.y='1.5' pos.z='0.0' facing.x='0.0' facing.y='0.0' facing.z='1.0'/>\r\n"
        "\t\t</NamedTransform>\r\n"
        "\t\t<NamedTransform name='route_checkpoint_indicator_01'>\r\n"
        "\t\t\t<Transform pos.x='4.0' pos.y='2.0' pos.z='100.0' facing.x='0.0' facing.y='0.0' facing.z='1.0'/>\r\n"
        "\t\t</NamedTransform>\r\n"
        "\t\t<NamedTransform name='route_checkpoint_indicator_01b' tag='e'>\r\n"
        "\t\t\t<Transform pos.x='-4.0' pos.y='2.0' pos.z='100.0' facing.x='0.0' facing.y='0.0' facing.z='1.0'/>\r\n"
        "\t\t</NamedTransform>\r\n"
        "\t\t<NamedTransform name='end_race_cannon_trigger' width='30.0'>\r\n"
        "\t\t\t<Transform pos.x='0.0' pos.y='3.0' pos.z='200.0' facing.x='0.0' facing.y='0.0' facing.z='1.0'/>\r\n"
        "\t\t</NamedTransform>\r\n"
        "\t\t<NamedTransform name='end_race_cannon_left_00'>\r\n"
        "\t\t\t<Transform pos.x='-10.0' pos.y='3.0' pos.z='200.0' facing.x='0.0' facing.y='0.0' facing.z='1.0'/>\r\n"
        "\t\t</NamedTransform>\r\n"
        "\t</NamedTransforms>\r\n"
        "\t<GameObjects/>\r\n"
        "</TrackRoute>\r\n");
}

fh1::RouteTransform routePoint(const QString& name, const QVector3D& position, const QVector3D& facing = {0, 0, 1})
{
    fh1::RouteTransform transform;
    transform.name = name;
    transform.position = position;
    transform.facing = facing;
    return transform;
}

QStringList transformNames(const fh1::RaceRoute& route)
{
    QStringList names;
    for (const fh1::RouteTransform& transform : route.transforms) {
        names.append(transform.name);
    }
    return names;
}

const char* const kParticles = R"(<?xml version="1.0" encoding="utf-8"?>
<ParticleEmitters>
  <SimpleEmitter>
    <GameplayID value="speed_camera_30_left" />
    <Name value="co_fest_speedcamera_001_1_Superspray_001" />
    <Effect value="AMB_Camera_Flash" />
    <Position v.x="159.7818" v.y="118.7879" v.z="3346.107" />
    <Orientation>
      <ZAxis v.x="0" v.y="1" v.z="0" />
    </Orientation>
  </SimpleEmitter>
  <TriggerZone>
    <Name value="zone_a" />
    <Position v.x="1" v.y="2" v.z="3" />
    <Radius value="12" />
  </TriggerZone>
</ParticleEmitters>
)";

const char* const kZones = R"(<?xml version="1.0" encoding="utf-8" ?>
<PostProcessingZones>
	<PostProcessingZone Name="Redrock" NumTriangles="2" Fog="Redrock">
		<PostProcessingZoneTriangle>
			<PostProcessingZoneTrianglePoint posX="0" posZ="0"/>
			<PostProcessingZoneTrianglePoint posX="10" posZ="0"/>
			<PostProcessingZoneTrianglePoint posX="0" posZ="10"/>
		</PostProcessingZoneTriangle>
		<PostProcessingZoneTriangle>
			<PostProcessingZoneTrianglePoint posX="10" posZ="0"/>
			<PostProcessingZoneTrianglePoint posX="10" posZ="10"/>
			<PostProcessingZoneTrianglePoint posX="0" posZ="10"/>
		</PostProcessingZoneTriangle>
	</PostProcessingZone>
</PostProcessingZones>
)";

bool writeFile(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class TestLoaders : public QObject {
    Q_OBJECT

private slots:
    void collisionObjects()
    {
        const fh1::Layer layer = fh1::loaders::placements(kCollObjs, QStringLiteral("collobjs"), QStringLiteral("C"),
            QStringLiteral("CollObjs.xml"), QStringLiteral("PhysicsType"));
        QCOMPARE(layer.features.size(), std::size_t{2});
        const fh1::Feature& first = layer.features[0];
        QCOMPARE(first.name, QStringLiteral("CO_ArmcoArrow_001.0.rmb"));
        QCOMPARE(first.group, QStringLiteral("ArmcoArrow"));
        QVERIFY(qFuzzyCompare(first.position.x(), -608.297485F));
        QVERIFY(qFuzzyCompare(first.forward.z(), 0.933504F));
        QCOMPARE(layer.features[1].group, QStringLiteral("CLRD"));
        QVERIFY(layer.features[1].forward.isNull());
    }

    void gameplayObjects()
    {
        const fh1::Layer layer = fh1::loaders::placements(kGameObjs, QStringLiteral("gameobjs"), QStringLiteral("G"),
            QStringLiteral("GameObjs.xml"), QStringLiteral("GameplayID"));
        QCOMPARE(layer.features.size(), std::size_t{2});
        QCOMPARE(layer.features[0].group, QStringLiteral("BF"));
        QCOMPARE(layer.features[1].group, QStringLiteral("SPEED"));
    }

    void placementErrors()
    {
        QVERIFY_THROWS_EXCEPTION(fh1::LoadError,
            fh1::loaders::placements(
                "<A><Obj0 PhysicsType='x'></Obj0></A>", {}, {}, {}, QStringLiteral("PhysicsType")));
        QVERIFY_THROWS_EXCEPTION(fh1::LoadError,
            fh1::loaders::placements(
                "<A><Obj0><Pos x='a' y='1' z='2'/></Obj0></A>", {}, {}, {}, QStringLiteral("PhysicsType")));
        QVERIFY_THROWS_EXCEPTION(
            fh1::LoadError, fh1::loaders::placements("<A><Obj0>", {}, {}, {}, QStringLiteral("PhysicsType")));
    }

    void trackRoutes()
    {
        fh1::Layer layer;
        layer.source = QStringLiteral("Ribbon_00");
        const fh1::RaceRoute route = fh1::loaders::raceRoute(kTrackRoute, QStringLiteral("TrackRoute000.xml"));
        QCOMPARE(route.transforms.size(), std::size_t{3});
        QCOMPARE(route.transforms[0].name, QStringLiteral("start_location_00"));
        QCOMPARE(route.transforms[0].width, 0.0F);
        QCOMPARE(route.transforms[2].width, 50.0F);
        QVERIFY(qFuzzyCompare(route.transforms[2].position.x(), -3352.49F));
        fh1::loaders::appendTrackRoute(route, QStringLiteral("TrackRoute000"), layer);
        QCOMPARE(layer.features.size(), std::size_t{3});
        QCOMPARE(layer.features[0].group, QStringLiteral("start_location"));
        QCOMPARE(layer.features[1].group, QStringLiteral("anim_couple"));
        QCOMPARE(layer.features[2].group, QStringLiteral("end_race_cannon_trigger"));
        QVERIFY(qFuzzyCompare(layer.features[0].forward.z(), -0.949661F));
        bool hasRouteFile = false;
        for (const auto& property : layer.features[0].properties) {
            hasRouteFile |= property.second == QLatin1String("TrackRoute000");
        }
        QVERIFY(hasRouteFile);
    }

    void trackRouteErrors()
    {
        QVERIFY_THROWS_EXCEPTION(fh1::LoadError,
            fh1::loaders::raceRoute("<R><NamedTransform name='a' width='wide'><Transform pos.x='1' pos.y='2' "
                                    "pos.z='3' facing.x='1' facing.y='0' facing.z='0'/></NamedTransform></R>",
                {}));
        QVERIFY_THROWS_EXCEPTION(fh1::LoadError,
            fh1::loaders::raceRoute("<R><NamedTransform name='a'><Transform pos.x='1'/></NamedTransform></R>", {}));
        QVERIFY_THROWS_EXCEPTION(fh1::LoadError, fh1::loaders::raceRoute("<R><NamedTransform", {}));
    }

    void routePointKinds()
    {
        QCOMPARE(fh1::routePointKind(QStringLiteral("start_location_03")), fh1::RoutePointKind::StartSlot);
        QCOMPARE(fh1::routePointKind(QStringLiteral("route_checkpoint_12")), fh1::RoutePointKind::Checkpoint);
        QCOMPARE(fh1::routePointKind(QStringLiteral("route_checkpoint_indicator_12b")),
            fh1::RoutePointKind::CheckpointMarker);
        QCOMPARE(fh1::routePointKind(QStringLiteral("end_race_cannon_trigger")), fh1::RoutePointKind::Finish);
        QCOMPARE(fh1::routePointKind(QStringLiteral("end_race_cannon_left_02")), fh1::RoutePointKind::FinishCannon);
        QCOMPARE(fh1::routePointKind(QStringLiteral("mission_photo_01")), fh1::RoutePointKind::Other);
    }

    void raceOverlayWithoutRacingLine()
    {
        const auto transform
            = [](const QString& name, float x, float z) { return routePoint(name, QVector3D(x, 0, z)); };
        fh1::RaceRoute route;
        route.source = QStringLiteral("Ribbon_00/TrackRoute012.xml");
        // Out of order in the file, as numbers past 9 sort after 1 by name.
        route.transforms = {transform(QStringLiteral("route_checkpoint_10"), 0, 100),
            transform(QStringLiteral("start_location_01"), 0, -5),
            transform(QStringLiteral("route_checkpoint_2"), 0, 50),
            transform(QStringLiteral("start_location_00"), 0, 0), transform(QStringLiteral("route_waypoint_00"), 9, 9),
            transform(QStringLiteral("anim_couple_01"), 7, 7)};
        fh1::RouteTransform finish = transform(QStringLiteral("end_race_cannon_trigger"), 0, 120);
        finish.width = 20.0F;
        route.transforms.push_back(finish);

        const std::vector<const fh1::RouteTransform*> checkpoints
            = fh1::routePoints(route, fh1::RoutePointKind::Checkpoint);
        QCOMPARE(checkpoints.size(), std::size_t{2});
        QCOMPARE(checkpoints[0]->name, QStringLiteral("route_checkpoint_2"));

        fh1::Race race;
        race.eventId = QStringLiteral("STREET_PLNS_005");
        race.name = QStringLiteral("Plains Run");
        const std::vector<fh1::Layer> layers = fh1::raceOverlay(race, route);
        QCOMPARE(layers.size(), std::size_t{2});
        const fh1::Layer& path = layers[0];
        QCOMPARE(path.id, QStringLiteral("racepath"));
        QCOMPARE(path.kind, fh1::FeatureKind::Polyline);
        QCOMPARE(path.features.size(), std::size_t{2});
        // Pole position, the checkpoints in order (not the waypoint), finish.
        const std::vector<QVector3D> expected{
            QVector3D(0, 0, 0), QVector3D(0, 0, 50), QVector3D(0, 0, 100), QVector3D(0, 0, 120)};
        QCOMPARE(path.features[0].shapes.front(), expected);
        // The finish line spans the trigger's width across its heading.
        const std::vector<QVector3D>& finishLine = path.features[1].shapes.front();
        QCOMPARE(finishLine.size(), std::size_t{2});
        QCOMPARE((finishLine[1] - finishLine[0]).length(), 20.0F);
        QCOMPARE(finishLine[0].z(), 120.0F);
        QVERIFY(path.groupColours.contains(QStringLiteral("Route")));

        const fh1::Layer& points = layers[1];
        QCOMPARE(points.id, QStringLiteral("racepoints"));
        QStringList labels;
        for (const fh1::Feature& feature : points.features) {
            labels.append(feature.label);
        }
        QCOMPARE(labels,
            (QStringList{QStringLiteral("Start"), QString(), QStringLiteral("Checkpoint 1"),
                QStringLiteral("Checkpoint 2"), QString(), QStringLiteral("Finish")}));
        QCOMPARE(points.features[1].group, QStringLiteral("Start grid"));
        QCOMPARE(points.features[4].group, QStringLiteral("Waypoints"));
    }

    void raceOverlayFollowsRacingLine()
    {
        fh1::RaceRoute route;
        route.transforms = {routePoint(QStringLiteral("start_location_00"), QVector3D(1, 0, 1), QVector3D(1, 0, 0)),
            routePoint(QStringLiteral("route_waypoint_00"), QVector3D(50, 0, 1), QVector3D(1, 0, 0))};
        route.racingLine = {QVector3D(0, 0, 0), QVector3D(10, 0, 0), QVector3D(20, 0, 5)};
        const std::vector<fh1::Layer> layers = fh1::raceOverlay(fh1::Race{}, route);
        QCOMPARE(layers.size(), std::size_t{2});
        QCOMPARE(layers[0].features.size(), std::size_t{1});
        QCOMPARE(layers[0].features[0].shapes.front(), route.racingLine);

        // A route file with nothing to draw adds no layers.
        QVERIFY(fh1::raceOverlay(fh1::Race{}, fh1::RaceRoute{}).empty());
    }

    void gameFloatFormat()
    {
        QCOMPARE(fh1::formatGameFloat(0.0F), QStringLiteral("0.0"));
        QCOMPARE(fh1::formatGameFloat(50.0F), QStringLiteral("50.0"));
        QCOMPARE(fh1::formatGameFloat(-1276.13F), QStringLiteral("-1276.13"));
        QCOMPARE(fh1::formatGameFloat(0.905893F), QStringLiteral("0.905893"));
        QCOMPARE(fh1::formatGameFloat(-2.8213e-7F), QStringLiteral("-2.8213e-007"));
        QCOMPARE(fh1::formatGameFloat(1234567.0F), QStringLiteral("1.23457e+006"));
    }

    void routeSavesWhatItRead()
    {
        const fh1::RaceRoute route = fh1::loaders::raceRoute(checkpointRoute(), QStringLiteral("TrackRoute005.xml"));
        QCOMPARE(route.transforms.size(), std::size_t{7});
        QCOMPARE(route.file.transformCount, std::size_t{7});
        QVERIFY(!fh1::isRouteEdited(route));
        QCOMPARE(fh1::writeRaceRoute(route), checkpointRoute());

        // Only the moved transform is written anew; the comment before it
        // and every other line stay as they were.
        fh1::RaceRoute moved = route;
        fh1::moveRoutePoint(moved, 1, QVector3D(2.5F, 2.0F, 110.0F));
        QVERIFY(fh1::isRouteEdited(moved));
        QByteArray expected = checkpointRoute();
        expected.replace("pos.x='0.0' pos.y='2.0' pos.z='100.0'", "pos.x='2.5' pos.y='2.0' pos.z='110.0'");
        // The checkpoint's indicators came along.
        expected.replace("pos.x='4.0' pos.y='2.0' pos.z='100.0'", "pos.x='6.5' pos.y='2.0' pos.z='110.0'");
        expected.replace("pos.x='-4.0' pos.y='2.0' pos.z='100.0'", "pos.x='-1.5' pos.y='2.0' pos.z='110.0'");
        QCOMPARE(fh1::writeRaceRoute(moved), expected);

        // A route with no file behind it is written whole.
        fh1::RaceRoute scratch;
        scratch.transforms = {routePoint(QStringLiteral("start_location_00"), QVector3D(1, 2, 3))};
        scratch.transforms[0].width = 12.5F;
        QCOMPARE(fh1::writeRaceRoute(scratch),
            QByteArray("<TrackRoute>\r\n\t<NamedTransforms>\r\n\t\t<NamedTransform name='start_location_00' "
                       "width='12.5'>\r\n\t\t\t<Transform pos.x='1.0' pos.y='2.0' pos.z='3.0' facing.x='0.0' "
                       "facing.y='0.0' facing.z='1.0'/>\r\n\t\t</NamedTransform>\r\n\t</NamedTransforms>\r\n"
                       "</TrackRoute>\r\n"));
    }

    void routePointsMoveAndTurn()
    {
        fh1::RaceRoute route = fh1::loaders::raceRoute(checkpointRoute(), {});
        // The finish takes its cannons along.
        fh1::moveRoutePoint(route, 5, QVector3D(5.0F, 3.0F, 210.0F));
        QCOMPARE(route.transforms[6].position, QVector3D(-5.0F, 3.0F, 210.0F));
        // A heading is kept level and of unit length.
        fh1::turnRoutePoint(route, 0, QVector3D(3.0F, 2.0F, 4.0F));
        QCOMPARE(route.transforms[0].facing, QVector3D(0.6F, 0.0F, 0.8F));
        fh1::turnRoutePoint(route, 0, QVector3D(0.0F, 1.0F, 0.0F));
        QCOMPARE(route.transforms[0].facing, QVector3D(0.6F, 0.0F, 0.8F));
    }

    void checkpointsInsertAndRemove()
    {
        const fh1::RaceRoute original = fh1::loaders::raceRoute(checkpointRoute(), {});
        QVERIFY(fh1::canInsertOrRemove(original, 0));
        QVERIFY(!fh1::canInsertOrRemove(original, 5));

        fh1::RaceRoute route = original;
        const std::optional<std::size_t> added = fh1::insertRoutePointAfter(route, 0);
        QVERIFY(added.has_value());
        QCOMPARE(*added, std::size_t{1});
        QCOMPARE(transformNames(route),
            (QStringList{QStringLiteral("route_checkpoint_00"), QStringLiteral("route_checkpoint_01"),
                QStringLiteral("route_checkpoint_02"), QStringLiteral("route_checkpoint_indicator_00"),
                QStringLiteral("route_checkpoint_indicator_01"), QStringLiteral("route_checkpoint_indicator_02"),
                QStringLiteral("route_checkpoint_indicator_02b"), QStringLiteral("end_race_cannon_trigger"),
                QStringLiteral("end_race_cannon_left_00")}));
        // Halfway to the next checkpoint, facing it, as wide as the one before.
        const fh1::RouteTransform& checkpoint = route.transforms[1];
        QCOMPARE(checkpoint.position, QVector3D(0.0F, 1.75F, 50.0F));
        QCOMPARE(checkpoint.facing, QVector3D(0.0F, 0.0F, 1.0F));
        QCOMPARE(checkpoint.width, 50.0F);
        // Its indicator sits where checkpoint 00's does beside it.
        QCOMPARE(route.transforms[4].position, QVector3D(4.0F, 1.75F, 50.0F));
        // The renumbered "b" indicator keeps its other attributes.
        const QByteArray written = fh1::writeRaceRoute(route);
        QVERIFY(written.contains("<NamedTransform name='route_checkpoint_indicator_02b' tag='e'>"));
        QVERIFY(written.contains("<NamedTransform name='route_checkpoint_01' width='50.0'>"));

        // After the last one, 50 m ahead.
        const std::optional<std::size_t> appended = fh1::insertRoutePointAfter(route, 2);
        QVERIFY(appended.has_value());
        QCOMPARE(route.transforms[*appended].name, QStringLiteral("route_checkpoint_03"));
        QCOMPARE(route.transforms[*appended].position, QVector3D(0.0F, 2.0F, 150.0F));

        // Removing both again numbers the rest back and restores the file.
        QVERIFY(fh1::removeRoutePoint(route, *appended));
        QVERIFY(fh1::removeRoutePoint(route, 1));
        QCOMPARE(transformNames(route), transformNames(original));
        const fh1::RaceRoute reread = fh1::loaders::raceRoute(fh1::writeRaceRoute(route), {});
        for (std::size_t i = 0; i < original.transforms.size(); ++i) {
            QCOMPARE(reread.transforms[i].position, original.transforms[i].position);
        }
        QVERIFY(!fh1::removeRoutePoint(route, 5));
    }

    void routeFilesSaveWhereChosen()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString media = QStringLiteral("tracks/colorado/Ribbon_00/TrackRoute005.xml");
        // A copy of the disc has a media folder; a copy of media does not.
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("disc/Media"))));
        QCOMPARE(fh1::routeOutputPath(dir.filePath(QStringLiteral("disc")), media),
            dir.filePath(QStringLiteral("disc/Media/") + media));
        QCOMPARE(fh1::routeOutputPath(dir.filePath(QStringLiteral("copy")), media),
            dir.filePath(QStringLiteral("copy/") + media));

        fh1::RaceRoute route = fh1::loaders::raceRoute(checkpointRoute(), {});
        fh1::moveRoutePoint(route, 0, QVector3D(1.0F, 1.5F, 0.0F));
        const QString original = dir.filePath(QStringLiteral("disc/Media/") + media);
        QVERIFY(QDir().mkpath(QFileInfo(original).absolutePath()));
        QVERIFY(writeFile(original, checkpointRoute()));

        // Saving elsewhere creates the folders and leaves the game's file.
        QString error;
        const QString elsewhere = dir.filePath(QStringLiteral("copy/") + media);
        const QString backup = dir.filePath(QStringLiteral("backups/") + media);
        QVERIFY2(
            fh1::saveEditedFile(fh1::writeRaceRoute(route), elsewhere, original, backup, &error), qPrintable(error));
        QCOMPARE(readFile(elsewhere), fh1::writeRaceRoute(route));
        QVERIFY(!QFile::exists(backup));

        // Saving over the game's file keeps the original once, outside the
        // game folder, which gains no files.
        const QStringList gameFiles = QDir(QFileInfo(original).absolutePath()).entryList(QDir::Files);
        QVERIFY2(
            fh1::saveEditedFile(fh1::writeRaceRoute(route), original, original, backup, &error), qPrintable(error));
        QCOMPARE(readFile(backup), checkpointRoute());
        fh1::moveRoutePoint(route, 0, QVector3D(2.0F, 1.5F, 0.0F));
        QVERIFY2(
            fh1::saveEditedFile(fh1::writeRaceRoute(route), original, original, backup, &error), qPrintable(error));
        QCOMPARE(readFile(backup), checkpointRoute());
        QCOMPARE(readFile(original), fh1::writeRaceRoute(route));
        QCOMPARE(QDir(QFileInfo(original).absolutePath()).entryList(QDir::Files), gameFiles);
    }

    void transformKinds()
    {
        QCOMPARE(fh1::loaders::transformKind(QStringLiteral("route_waypoint_12")), QStringLiteral("route_waypoint"));
        QCOMPARE(fh1::loaders::transformKind(QStringLiteral("anim_couple_01_01")), QStringLiteral("anim_couple"));
        QCOMPARE(fh1::loaders::transformKind(QStringLiteral("finish")), QStringLiteral("finish"));
        QCOMPARE(fh1::loaders::transformKind(QStringLiteral("42")), QStringLiteral("42"));
    }

    void groups()
    {
        QCOMPARE(fh1::loaders::gameplayGroup(QStringLiteral("FR50_00")), QStringLiteral("FR"));
        QCOMPARE(fh1::loaders::gameplayGroup(QStringLiteral("speed_camera")), QStringLiteral("SPEED"));
        QCOMPARE(fh1::loaders::gameplayGroup(QStringLiteral("123")), QStringLiteral("(other)"));
        QCOMPARE(fh1::loaders::physicsGroup(QStringLiteral("CO_MarkerPole_002.1.rmb")), QStringLiteral("MarkerPole"));
        QCOMPARE(fh1::loaders::physicsGroup(QStringLiteral("CO_CLRD_Sign_Speed35.rmb")), QStringLiteral("CLRD"));
        QCOMPARE(fh1::loaders::physicsGroup(QStringLiteral("CO_")), QStringLiteral("(other)"));
    }

    void particles()
    {
        const fh1::Layer layer = fh1::loaders::particleEmitters(kParticles, QStringLiteral("ParticleEmitters.xml"));
        QCOMPARE(layer.features.size(), std::size_t{2});
        QCOMPARE(layer.features[0].group, QStringLiteral("SimpleEmitter"));
        QCOMPARE(layer.features[0].name, QStringLiteral("co_fest_speedcamera_001_1_Superspray_001"));
        QVERIFY(qFuzzyCompare(layer.features[0].position.z(), 3346.107F));
        QCOMPARE(layer.features[1].group, QStringLiteral("TriggerZone"));
    }

    void zones()
    {
        const fh1::Layer layer = fh1::loaders::postProcessingZones(kZones, QStringLiteral("zones.xml"));
        QCOMPARE(layer.kind, fh1::FeatureKind::Polygon);
        QCOMPARE(layer.features.size(), std::size_t{1});
        const fh1::Feature& zone = layer.features[0];
        QCOMPARE(zone.name, QStringLiteral("Redrock"));
        QCOMPARE(zone.shapes.size(), std::size_t{2});
        QVERIFY(qFuzzyCompare(zone.position.x(), 5.0F));
        QVERIFY(qFuzzyCompare(zone.position.z(), 5.0F));
    }

    void navNodes()
    {
        const QByteArray data = navFile({{0, 1.0F, 2.0F, 3.0F, 1}, {7, 4.0F, 5.0F, 6.0F, 2}, {9, 0, 0, 0, 3}}, 6);
        const fh1::Layer layer = fh1::loaders::navNodes(data, QStringLiteral("test.nav"));
        QCOMPARE(layer.features.size(), std::size_t{3});
        QCOMPARE(layer.features[1].name, QStringLiteral("Node 7"));
        QVERIFY(qFuzzyCompare(layer.features[1].position.z(), 6.0F));
        QCOMPARE(layer.features[0].group, QStringLiteral("Dead ends"));
        QCOMPARE(layer.features[1].group, QStringLiteral("Road"));
        QCOMPARE(layer.features[2].group, QStringLiteral("Junctions"));
    }

    void navErrors()
    {
        QVERIFY_THROWS_EXCEPTION(fh1::LoadError, fh1::loaders::navNodes(QByteArray(10, '\0'), {}));
        QVERIFY_THROWS_EXCEPTION(
            fh1::LoadError, fh1::loaders::navNodes(navFile({{0, 0, 0, 0, 1}}, 5), QStringLiteral("t.nav")));
        QByteArray truncated = navFile({{0, 0, 0, 0, 1}, {1, 0, 0, 0, 1}}, 2);
        truncated.chop(8);
        QVERIFY_THROWS_EXCEPTION(fh1::LoadError, fh1::loaders::navNodes(truncated, {}));
    }

    void aiRoutes()
    {
        const QList<QVector3D> points{{0, 0, 0}, {3, 0, 4}, {3, 0, 10}};
        const fh1::Feature open = fh1::loaders::aiRoute(owtFile(points, false), QStringLiteral("r"));
        QCOMPARE(open.shapes.size(), std::size_t{1});
        QCOMPARE(open.shapes[0].size(), std::size_t{3});
        const fh1::Feature loop = fh1::loaders::aiRoute(owtFile(points, true), QStringLiteral("r"));
        QCOMPARE(loop.shapes[0].size(), std::size_t{4});
        QCOMPARE(loop.shapes[0].back(), loop.shapes[0].front());
        bool lengthOk = false;
        for (const auto& property : open.properties) {
            lengthOk |= property.first == QLatin1String("Length") && property.second == QLatin1String("0.01 km");
        }
        QVERIFY(lengthOk);
    }

    void aiRouteErrors()
    {
        QVERIFY_THROWS_EXCEPTION(fh1::LoadError, fh1::loaders::aiRoute("NOPE", {}));
        QByteArray data = owtFile({{0, 0, 0}, {1, 1, 1}}, false);
        data.chop(1);
        QVERIFY_THROWS_EXCEPTION(fh1::LoadError, fh1::loaders::aiRoute(data, {}));
        QVERIFY_THROWS_EXCEPTION(fh1::LoadError, fh1::loaders::aiRoute(owtFile({{0, 0, 0}}, false), {}));
    }

    void calibration()
    {
        const auto background = fh1::knownBackground(QStringLiteral("Colorado"));
        QVERIFY(background.has_value());
        QCOMPARE(background->expectedSize, QSize(5120, 3072));
        const fh1::MapCalibration& c = background->calibration;
        const QPointF image = c.worldToImage(-1239.47, 1435.60);
        const QPointF world = c.imageToWorld(image);
        QVERIFY(std::abs(world.x() + 1239.47) < 1e-6);
        QVERIFY(std::abs(world.y() - 1435.60) < 1e-6);
        QVERIFY(!fh1::knownBackground(QStringLiteral("TestBed")).has_value());
    }

    void gameInstall()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFile(dir.filePath(QStringLiteral("Media/DB/gamedb.slt")), {}));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("Media/Tracks/Colorado/Ribbon_00"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("Media/Tracks/NoRibbon"))));

        fh1::GameInstall install;
        QVERIFY2(install.open(dir.path()), qPrintable(install.errorString()));
        QCOMPARE(install.trackFolders(), QStringList{QStringLiteral("Colorado")});
        QVERIFY(!install.resolve(QStringLiteral("tracks/colorado/ribbon_00")).isEmpty());
        QVERIFY(install.resolve(QStringLiteral("tracks/colorado/missing.xml")).isEmpty());

        fh1::GameInstall fromMedia;
        QVERIFY(fromMedia.open(dir.filePath(QStringLiteral("Media"))));
        fh1::GameInstall wrong;
        QVERIFY(!wrong.open(dir.filePath(QStringLiteral("Media/Tracks"))));
        QVERIFY(!wrong.errorString().isEmpty());
    }

    void mapLoaderWithPartialData()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString ribbon = dir.filePath(QStringLiteral("media/tracks/testbed/Ribbon_00/"));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("media/db"))));
        QVERIFY(writeFile(ribbon + QStringLiteral("CollObjs.xml"), kCollObjs));
        QVERIFY(writeFile(ribbon + QStringLiteral("GameObjs.xml"), kGameObjs));
        QVERIFY(writeFile(ribbon + QStringLiteral("TrackRoute000.xml"), kTrackRoute));
        QVERIFY(writeFile(ribbon + QStringLiteral("PostProcessingZones_Safe.xml"), "<broken"));
        QVERIFY(
            writeFile(dir.filePath(QStringLiteral("media/tracks/testbed/testbed.nav")), navFile({{0, 1, 2, 3, 1}}, 1)));

        fh1::GameInstall install;
        QVERIFY(install.open(dir.path()));
        QStringList steps;
        const fh1::MapData map
            = fh1::MapLoader::load(install, QStringLiteral("testbed"), [&steps](const QString& s) { steps.append(s); });

        QStringList ids;
        for (const fh1::Layer& layer : map.layers) {
            ids.append(layer.id);
        }
        QCOMPARE(ids,
            (QStringList{QStringLiteral("nav"), QStringLiteral("collobjs"), QStringLiteral("trackroutes"),
                QStringLiteral("gameobjs")}));
        QVERIFY(map.background.isNull());
        QVERIFY(!steps.isEmpty());
        // The route file is kept by its number; without a database there
        // are no races to run on it.
        QCOMPARE(map.raceRoutes.size(), std::size_t{1});
        QCOMPARE(map.raceRoutes[0].routeId, 0);
        QCOMPARE(map.raceRoutes[0].source, QStringLiteral("Ribbon_00/TrackRoute000.xml"));
        QVERIFY(map.races.empty());

        // The broken zones file and the missing disc-wide files (AI archive,
        // database, string tables, icons) are reported without stopping the
        // load; a track file that is simply absent (ParticleEmitters.xml here)
        // only means that layer is empty.
        const QString warnings = map.warnings.join(QLatin1Char('\n'));
        QVERIFY2(warnings.contains(QLatin1String("PostProcessingZones_Safe.xml")), qPrintable(warnings));
        QVERIFY2(warnings.contains(QLatin1String("aiopenworld.zip")), qPrintable(warnings));
        QVERIFY2(warnings.contains(QLatin1String("gamedb.slt")), qPrintable(warnings));
        QVERIFY2(!warnings.contains(QLatin1String("ParticleEmitters.xml")), qPrintable(warnings));
    }

    void mapLoaderMissingTrack()
    {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("media/db"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("media/tracks"))));
        fh1::GameInstall install;
        QVERIFY(install.open(dir.path()));
        QVERIFY_THROWS_EXCEPTION(fh1::LoadError, fh1::MapLoader::load(install, QStringLiteral("nowhere")));
    }
};

QTEST_GUILESS_MAIN(TestLoaders)
#include "tst_loaders.moc"
