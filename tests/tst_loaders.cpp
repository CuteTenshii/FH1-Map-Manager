#include "GameInstall.h"
#include "Loaders.h"
#include "MapCalibration.h"
#include "MapLoader.h"

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
	</NamedTransforms>
</TrackRoute>
)";

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
        fh1::loaders::appendTrackRoute(kTrackRoute, QStringLiteral("TrackRoute000"), layer);
        QCOMPARE(layer.features.size(), std::size_t{2});
        QCOMPARE(layer.features[0].group, QStringLiteral("start_location"));
        QCOMPARE(layer.features[1].group, QStringLiteral("anim_couple"));
        QVERIFY(qFuzzyCompare(layer.features[0].forward.z(), -0.949661F));
        bool hasRouteFile = false;
        for (const auto& property : layer.features[0].properties) {
            hasRouteFile |= property.second == QLatin1String("TrackRoute000");
        }
        QVERIFY(hasRouteFile);
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
