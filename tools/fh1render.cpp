// Renders one view of a track's 3D world to a PNG without a window, using the
// same renderer as the viewer. Useful for scripted captures and for checking
// the renderer on machines (or platforms) where the interactive view cannot
// open.

#include "EntityRenderer.h"
#include "ForzaZip.h"
#include "GameInstall.h"
#include "MapLoader.h"
#include "TrackTextures.h"
#include "WorldIndex.h"
#include "WorldRenderer.h"
#include "WorldTiles.h"

#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QPainter>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentMap>

#include <cstdio>
#include <memory>
#include <numbers>

namespace {

int fail(const QString& message)
{
    std::fprintf(stderr, "%s\n", qPrintable(message));
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    // Same names as the viewer, so both share the world index cache.
    QGuiApplication::setApplicationName(QStringLiteral("fh1-map-viewer"));
    QGuiApplication::setOrganizationName(QStringLiteral("fh1-tools"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Renders a view of a track's 3D world to a PNG file."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("game-folder"), QStringLiteral("Extracted disc or its media folder."));
    parser.addPositionalArgument(QStringLiteral("output"), QStringLiteral("PNG file to write."));
    const QCommandLineOption trackOption(
        QStringLiteral("track"), QStringLiteral("Track folder."), QStringLiteral("name"), QStringLiteral("colorado"));
    const QCommandLineOption cameraOption(QStringLiteral("camera"),
        QStringLiteral("World X,Y,Z, heading and pitch in degrees (heading 0 = east, 90 = north)."),
        QStringLiteral("x,y,z,heading,pitch"), QStringLiteral("-1239,300,700,90,-20"));
    const QCommandLineOption sizeOption(
        QStringLiteral("size"), QStringLiteral("Image size."), QStringLiteral("WxH"), QStringLiteral("1600x900"));
    const QCommandLineOption distanceOption(QStringLiteral("distance"), QStringLiteral("View distance in metres."),
        QStringLiteral("metres"), QStringLiteral("9000"));
    const QCommandLineOption untexturedOption(
        QStringLiteral("untextured"), QStringLiteral("Draw without the game's textures, in plain ground colours."));
    const QCommandLineOption layersOption(QStringLiteral("layers"),
        QStringLiteral("Comma-separated map layer ids to draw over the world, with their labels (gameobjs, airoutes, "
                       "trackroutes, collobjs, particles, nav, ppzones)."),
        QStringLiteral("ids"));
    parser.addOptions({trackOption, cameraOption, sizeOption, distanceOption, untexturedOption, layersOption});
    parser.process(app);
    const QStringList args = parser.positionalArguments();
    if (args.size() != 2) {
        parser.showHelp(2);
    }

    const QStringList cameraParts = parser.value(cameraOption).split(QLatin1Char(','));
    const QStringList sizeParts = parser.value(sizeOption).toLower().split(QLatin1Char('x'));
    if (cameraParts.size() != 5 || sizeParts.size() != 2) {
        return fail(QStringLiteral("--camera needs 5 values and --size WxH"));
    }
    WorldCamera camera;
    camera.position = QVector3D(cameraParts[0].toFloat(), cameraParts[1].toFloat(), cameraParts[2].toFloat());
    camera.yaw = cameraParts[3].toFloat() * std::numbers::pi_v<float> / 180.0F;
    camera.pitch = cameraParts[4].toFloat() * std::numbers::pi_v<float> / 180.0F;
    const QSize size(sizeParts[0].toInt(), sizeParts[1].toInt());
    if (size.isEmpty()) {
        return fail(QStringLiteral("invalid --size"));
    }

    QElapsedTimer timer;
    timer.start();
    fh1::GameInstall install;
    if (!install.open(args[0])) {
        return fail(install.errorString());
    }
    const QString track = parser.value(trackOption);
    const auto mapData = std::make_shared<const fh1::MapData>(fh1::MapLoader::load(install, track));
    const fh1::MapData& map = *mapData;

    const QString archivePath = install.resolve(QStringLiteral("tracks/%1/bin.zip").arg(track));
    fh1::ForzaZip archive;
    if (archivePath.isEmpty() || !archive.open(archivePath)) {
        return fail(QStringLiteral("cannot open tracks/%1/bin.zip").arg(track));
    }
    const QString cachePath
        = QStringLiteral("%1/world/%2.index")
              .arg(QStandardPaths::writableLocation(QStandardPaths::CacheLocation), track.toLower());
    const QString signature = fh1::WorldIndex::archiveSignature(archivePath);
    std::optional<fh1::WorldIndex> index = fh1::WorldIndex::load(cachePath, signature);
    if (!index) {
        index = fh1::WorldIndex::build(archive);
        if (!index) {
            return fail(QStringLiteral("could not index %1").arg(archivePath));
        }
        index->save(cachePath, signature);
    }
    const fh1::WorldTileGrid grid(*index, 500.0F);

    std::optional<fh1::TrackTextures> textures;
    if (!parser.isSet(untexturedOption)) {
        QString textureError = QStringLiteral("the track has no PVS file");
        const QString pvsPath = install.trackPvsPath(track);
        if (!pvsPath.isEmpty()) {
            QFile pvs(pvsPath);
            if (pvs.open(QIODevice::ReadOnly)) {
                textures = fh1::TrackTextures::load(pvs.readAll(), archive, &textureError);
            } else {
                textureError = pvs.errorString();
            }
        }
        if (!textures) {
            std::fprintf(stderr, "No game textures: %s\n", qPrintable(textureError));
        }
    }

    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create()) {
        return fail(QStringLiteral("cannot create an OpenGL 3.3 context"));
    }
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!context.makeCurrent(&surface)) {
        return fail(QStringLiteral("cannot make the OpenGL context current"));
    }

    WorldRenderer renderer;
    if (!renderer.initialize()) {
        return fail(renderer.errorString());
    }
    renderer.setViewDistance(parser.value(distanceOption).toFloat());
    renderer.setGrid(&grid);

    const std::vector<TileRequest> requests = renderer.requests(camera, {});
    const std::vector<fh1::TileMesh> meshes
        = QtConcurrent::blockingMapped<std::vector<fh1::TileMesh>>(requests, [&](const TileRequest& request) {
              return fh1::buildTileMesh(archive, *index, request.chunks, textures ? &*textures : nullptr);
          });
    int failed = 0;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        renderer.upload(requests[i].tile, requests[i].state, meshes[i]);
        failed += meshes[i].failedChunks;
    }
    // Tiles only ask for textures when the texture tables loaded.
    if (textures) {
        const fh1::TrackTextures& trackTextures = *textures;
        const std::vector<std::uint32_t> textureIds = renderer.takeTextureRequests();
        const std::vector<std::optional<fh1::TextureMipChain>> chains
            = QtConcurrent::blockingMapped<std::vector<std::optional<fh1::TextureMipChain>>>(
                textureIds, [&](std::uint32_t id) { return trackTextures.loadTexture(archive, id); });
        for (std::size_t i = 0; i < textureIds.size(); ++i) {
            if (const std::optional<fh1::TextureMipChain>& chain = chains[i]; chain.has_value()) {
                renderer.uploadTexture(textureIds[i], *chain);
            } else {
                renderer.failTexture(textureIds[i]);
            }
        }
    }
    const WorldRenderer::TextureStats textureStats = renderer.textureStats();

    QOpenGLFramebufferObjectFormat fboFormat;
    fboFormat.setAttachment(QOpenGLFramebufferObject::Depth);
    fboFormat.setSamples(4);
    QOpenGLFramebufferObject fbo(size, fboFormat);
    const QStringList layerIds = parser.value(layersOption).split(QLatin1Char(','), Qt::SkipEmptyParts);
    EntityRenderer entities;
    if (!layerIds.isEmpty()) {
        std::vector<std::vector<bool>> visible;
        for (const fh1::Layer& layer : map.layers) {
            const LayerStyle style = LayerStyle::of(layer, static_cast<int>(visible.size()));
            visible.emplace_back(static_cast<std::size_t>(style.groups.size()), layerIds.contains(layer.id));
        }
        if (!entities.initialize()) {
            return fail(entities.errorString());
        }
        entities.setMap(mapData, visible);
    }

    fbo.bind();
    const WorldRenderer::Stats stats = renderer.draw(camera, size);
    const QMatrix4x4 worldViewProjection = renderer.worldViewProjection(camera, size);
    if (entities.isReady()) {
        entities.draw(
            worldViewProjection, size, camera.position, renderer.fogDistance(), WorldRenderer::fogColour(), 1.0F);
    }
    fbo.release();
    QImage image = fbo.toImage();
    if (entities.isReady()) {
        QPainter painter(&image);
        EntityRenderer::paintLabels(painter,
            entities.labels(worldViewProjection, size, camera.position, renderer.viewDistance() * 0.3F, 1.0F), QFont());
        painter.end();
        entities.release();
    }
    renderer.release();
    if (!image.save(args[1], "PNG")) {
        return fail(QStringLiteral("cannot write %1").arg(args[1]));
    }
    std::printf("%zu tiles built (%d chunks failed), %d drawn, %.1f M triangles, %d draw calls; "
                "%d textures (%.0f MB), %d missing; %lld ms\n",
        requests.size(), failed, stats.drawnTiles, static_cast<double>(stats.drawnTriangles) / 1e6, stats.drawCalls,
        textureStats.loaded, static_cast<double>(textureStats.bytes) / (1024.0 * 1024.0), textureStats.failed,
        timer.elapsed());
    return 0;
}
