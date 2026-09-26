// Checks against a real extracted Forza Horizon disc. Set FH1_GAME_DIR to the
// disc folder (holding media) to run these; they are skipped otherwise.

#include "ForzaZip.h"
#include "GameInstall.h"
#include "MapLoader.h"
#include "RenderMesh.h"
#include "TrackTextures.h"
#include "WorldIndex.h"

#include <QTest>

#include <QFile>
#include <QSet>
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
        // local-space props and a few dozen are cages and shadow casters.
        QVERIFY2(index->chunks().size() > 11500 && index->chunks().size() < 12500,
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
